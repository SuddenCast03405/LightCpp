// SettingsStore — reads and merges the configuration layers.

#include "LightCpp/Settings/SettingsStore.hpp"

#include "LightCpp/Settings/SettingsJson.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace LightCpp::Settings {

namespace {

/// Read a file fully; empty on any failure.
[[nodiscard]] QByteArray readFileBytes(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

/// All *.json files in `dir`, sorted by name so the merge order is stable
/// and independent of the file system's enumeration order.
[[nodiscard]] QStringList configFilesIn(const QString& dir)
{
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        return {};
    }
    QStringList files;
    QDirIterator it(dir, {QStringLiteral("*.json")}, QDir::Files);
    while (it.hasNext()) {
        files.append(it.next());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

QString SettingsStore::baseConfigDirectory()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (exeDir.isEmpty()) {
        // Outside an application (unit tests without QCoreApplication) —
        // a path relative to the CWD is the best that can be done.
        return QStringLiteral("config");
    }

    // Keep the portable layout when the installation is writable. A system
    // install under Program Files, /usr/bin or /Applications is commonly
    // read-only for the current user, however; putting config beside that
    // executable makes an otherwise successful first launch impossible to
    // persist. In that case use Qt's per-user application config location.
    const QString portableDir =
        QDir(exeDir).filePath(QStringLiteral("config"));
    const QFileInfo portableInfo(portableDir);
    const bool portableWritable = portableInfo.exists()
        ? portableInfo.isWritable()
        : QFileInfo(exeDir).isWritable();
    if (portableWritable) {
        return portableDir;
    }

    const QString userDir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (userDir.isEmpty()
        || (!QDir().mkpath(userDir) && !QFileInfo(userDir).isDir())
        || !QFileInfo(userDir).isWritable()) {
        // Preserve the old location as a last resort. The eventual write
        // reports its own error instead of silently inventing another path.
        return portableDir;
    }

    // Migrate existing portable settings once, without overwriting anything
    // already present in the per-user directory. This keeps an installed
    // copy's existing kits and preferences when the app moves to a protected
    // system location.
    if (QFileInfo(portableDir).isDir()) {
        QDirIterator it(portableDir, {QStringLiteral("*.json")}, QDir::Files);
        while (it.hasNext()) {
            const QString source = it.next();
            const QString destination =
                QDir(userDir).filePath(QFileInfo(source).fileName());
            if (!QFileInfo::exists(destination)) {
                QFile::copy(source, destination);
            }
        }
    }
    return userDir;
}

bool SettingsStore::writeDocument(const QString& filePath,
                                  const QJsonDocument& document)
{
    const QFileInfo info(filePath);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    // QSaveFile writes to a temporary file in the same directory and moves
    // it into place on commit — a crash or a full disk mid-write then
    // leaves the previous config intact instead of a half-written file
    // that fails to parse on the next start.
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray bytes =
        document.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QJsonObject SettingsStore::mergeUnknownKeys(const QJsonObject& fresh,
                                            const QJsonObject& previous)
{
    const auto grow = [](auto&& self, const QJsonObject& f,
                         const QJsonObject& p) -> QJsonObject {
        QJsonObject merged = f;
        for (auto it = p.constBegin(); it != p.constEnd(); ++it) {
            if (!merged.contains(it.key())) {
                merged.insert(it.key(), it.value());
            } else if (it.value().isObject()
                       && merged.value(it.key()).isObject()) {
                merged.insert(it.key(),
                              self(self, merged.value(it.key()).toObject(),
                                   it.value().toObject()));
            }
        }
        return merged;
    };
    return grow(grow, fresh, previous);
}

namespace {

/// The body of SettingsStore::layerDelta(): the fields, one level at a time,
/// with no notion of a document header. Splitting it out keeps "type" and
/// "id" from being treated as always-owned at every nesting level — only the
/// document's own top level has a header.
QJsonObject layerDeltaFields(const QJsonObject& fresh, const QJsonObject& loaded,
                             const QJsonObject& previous)
{
    QJsonObject delta;
    for (auto it = fresh.constBegin(); it != fresh.constEnd(); ++it) {
        const QString& key = it.key();
        const QJsonValue value = it.value();
        const QJsonValue loadedValue = loaded.value(key);
        if (value.isObject()) {
            const QJsonValue previousValue = previous.value(key);
            const QJsonObject nested = layerDeltaFields(
                value.toObject(),
                loadedValue.isObject() ? loadedValue.toObject() : QJsonObject{},
                previousValue.isObject() ? previousValue.toObject()
                                         : QJsonObject{});
            if (!nested.isEmpty()) {
                delta.insert(key, nested);
            }
            continue;
        }
        // Unmentioned before and unchanged since: not this layer's business.
        if (previous.value(key).isUndefined() && loadedValue == value) {
            continue;
        }
        delta.insert(key, value);
    }
    // Same courtesy as mergeUnknownKeys(): a key this build does not know
    // about was put there by the user (or a newer build) and stays.
    for (auto it = previous.constBegin(); it != previous.constEnd(); ++it) {
        if (!fresh.contains(it.key())) {
            delta.insert(it.key(), it.value());
        }
    }
    return delta;
}

}  // namespace

QJsonObject SettingsStore::layerDelta(const QJsonObject& fresh,
                                      const QJsonObject& loaded,
                                      const QJsonObject& previous)
{
    QJsonObject delta = layerDeltaFields(fresh, loaded, previous);
    // The header is not a setting: whatever the fields say, the document
    // must stay recognisable on the next load.
    for (const char* key : {"type", "id"}) {
        const QJsonValue value = fresh.value(QLatin1String(key));
        if (!value.isUndefined()) {
            delta.insert(QLatin1String(key), value);
        }
    }
    return delta;
}

bool SettingsStore::layerDeltaHasContent(const QJsonObject& delta)
{
    for (auto it = delta.constBegin(); it != delta.constEnd(); ++it) {
        if (it.key() != QLatin1String("type") && it.key() != QLatin1String("id")) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

void SettingsStore::setOverlayDirectories(const QStringList& dirs)
{
    overlayDirs_ = dirs;
}

void SettingsStore::setBaseDirectory(const QString& dir)
{
    baseDir_ = dir;
}

void SettingsStore::setFallbackToolPaths(const QString& clangdPath,
                                         const QString& clangFormatPath,
                                         const QString& gitPath)
{
    fallbackClangd_ = clangdPath;
    fallbackClangFormat_ = clangFormatPath;
    fallbackGit_ = gitPath;
}

bool SettingsStore::reload()
{
    kitDocs_.clear();
    toolchainDocs_.clear();
    mergedDefaults_ = QJsonObject();
    mergedPreferences_ = QJsonObject();
    errors_.clear();

    const QString base = baseDir_.isEmpty() ? baseConfigDirectory() : baseDir_;
    for (const QString& file : configFilesIn(base)) {
        mergeFile(file);
    }
    for (const QString& dir : overlayDirs_) {
        for (const QString& file : configFilesIn(dir)) {
            mergeFile(file);
        }
    }
    // The base layer existing but yielding nothing readable is worth
    // reporting; no config at all is the normal first-run state.
    if (QFileInfo(base).isDir() && kitDocs_.isEmpty() && toolchainDocs_.isEmpty()
        && mergedDefaults_.isEmpty() && mergedPreferences_.isEmpty()) {
        return false;
    }
    return true;
}

void SettingsStore::mergeFile(const QString& filePath)
{
    const QByteArray bytes = readFileBytes(filePath);
    if (bytes.isEmpty()) {
        if (QFileInfo(filePath).size() > 0) {
            errors_.append(QStringLiteral("%1: cannot be read").arg(filePath));
        }
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || document.isNull()) {
        errors_.append(QStringLiteral("%1: %2")
                           .arg(filePath, parseError.errorString()));
        return;
    }

    // A file is either one document (an object with a "type") or an array
    // of documents — which is how kits.json keeps several kits together.
    if (document.isObject()) {
        mergeDocument(document.object(), filePath);
        return;
    }
    if (document.isArray()) {
        for (const QJsonValue& item : document.array()) {
            if (item.isObject()) {
                mergeDocument(item.toObject(), filePath);
            }
        }
    }
}

QJsonObject SettingsStore::mergeObjects(const QJsonObject& base,
                                        const QJsonObject& overlay)
{
    QJsonObject merged = base;
    for (auto it = overlay.constBegin(); it != overlay.constEnd(); ++it) {
        const QJsonValue overlayValue = it.value();
        const auto baseValue = merged.constFind(it.key());
        if (overlayValue.isObject() && baseValue != merged.constEnd()
            && baseValue->isObject()) {
            merged.insert(it.key(), mergeObjects(baseValue->toObject(),
                                                 overlayValue.toObject()));
        } else {
            merged.insert(it.key(), overlayValue);
        }
    }
    return merged;
}

bool SettingsStore::mergeDocument(const QJsonObject& document, const QString& source)
{
    const QString type = document.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("toolchain")) {
        const QString id = document.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            errors_.append(QStringLiteral("%1: toolchain document without an id")
                               .arg(source));
            return false;
        }
        for (auto& entry : toolchainDocs_) {
            if (entry.first == id) {
                entry.second = mergeObjects(entry.second, document);
                return true;
            }
        }
        toolchainDocs_.append({id, document});
        return true;
    }
    if (type == QLatin1String("toolchains")) {
        // Legacy registry format (lists of candidate paths), superseded by
        // per-toolchain documents. Accepted so old config dirs stay quiet;
        // its contents are simply not consumed.
        return true;
    }
    if (type == QLatin1String("session")) {
        // Window/tab session state lives in the same directory but is written
        // by MainWindow, not by this class. The *.json sweep still reads it, so
        // accept it silently instead of reporting "unknown document type" on
        // every start/reload, which read like a broken configuration.
        return true;
    }
    if (type == QLatin1String("defaults")) {
        mergedDefaults_ = mergeObjects(mergedDefaults_, document);
        return true;
    }
    if (type == QLatin1String("preferences")) {
        mergedPreferences_ = mergeObjects(mergedPreferences_, document);
        return true;
    }
    if (type == QLatin1String("kit")) {
        const QString id = document.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            errors_.append(QStringLiteral("%1: kit document without an id")
                               .arg(source));
            return false;
        }
        for (auto& entry : kitDocs_) {
            if (entry.first == id) {
                entry.second = mergeObjects(entry.second, document);
                return true;
            }
        }
        kitDocs_.append({id, document});
        return true;
    }
    errors_.append(QStringLiteral("%1: unknown document type \"%2\"")
                       .arg(source, type));
    return false;
}

// ---------------------------------------------------------------------------
// Typed views
// ---------------------------------------------------------------------------

GlobalDefaults SettingsStore::defaults() const
{
    GlobalDefaults out;
    fromJson(mergedDefaults_, out);
    return out;
}

QList<Toolchain> SettingsStore::toolchains() const
{
    QList<Toolchain> out;
    for (const auto& entry : toolchainDocs_) {
        Toolchain toolchain;
        fromJson(entry.second, toolchain);
        out.append(toolchain);
    }
    return out;
}

std::optional<Toolchain> SettingsStore::toolchain(const QString& id) const
{
    for (const auto& entry : toolchainDocs_) {
        if (entry.first == id) {
            Toolchain toolchain;
            fromJson(entry.second, toolchain);
            return toolchain;
        }
    }
    return std::nullopt;
}

Preferences SettingsStore::preferences() const
{
    Preferences out;
    fromJson(mergedPreferences_, out);
    return out;
}

/// Fill the kit's empty tool fields from `toolchain`. The kit's compiler
/// family decides which toolchain compilers map onto the kit's C/C++
/// slots; the kit's own values always win, so inheritance is purely a
/// fallback. An unknown (or empty) family inherits no compiler at all —
/// an "else" kit names its compilers outright.
void applyToolchainInheritance(Kit& kit, const Toolchain& toolchain)
{
    const QString family = kit.compilerFamily;
    if (family == QLatin1String("gcc")) {
        if (kit.cCompiler.isEmpty()) {
            kit.cCompiler = toolchain.compilers.value(QStringLiteral("gcc"));
        }
        if (kit.cppCompiler.isEmpty()) {
            kit.cppCompiler = toolchain.compilers.value(QStringLiteral("g++"));
        }
    } else if (family == QLatin1String("llvm")) {
        if (kit.cCompiler.isEmpty()) {
            kit.cCompiler = toolchain.compilers.value(QStringLiteral("clang"));
        }
        if (kit.cppCompiler.isEmpty()) {
            kit.cppCompiler =
                toolchain.compilers.value(QStringLiteral("clang++"));
        }
    } else if (family == QLatin1String("msvc")) {
        // cl compiles both C and C++.
        if (kit.cCompiler.isEmpty()) {
            kit.cCompiler = toolchain.compilers.value(QStringLiteral("cl"));
        }
        if (kit.cppCompiler.isEmpty()) {
            kit.cppCompiler = toolchain.compilers.value(QStringLiteral("cl"));
        }
    }
    // else: "else" family — no compiler inheritance.

    if (kit.debugger.isEmpty()) {
        if (family == QLatin1String("msvc")) {
            kit.debugger = toolchain.compilers.value(QStringLiteral("cdb"));
        } else {
            // gcc/llvm kits debug with gdb when present, else lldb.
            const QString gdb = toolchain.compilers.value(QStringLiteral("gdb"));
            kit.debugger =
                !gdb.isEmpty() ? gdb
                               : toolchain.compilers.value(QStringLiteral("lldb"));
        }
    }
    if (kit.clangd.isEmpty()) {
        kit.clangd = toolchain.clangd;
    }
    if (kit.clangFormat.isEmpty()) {
        kit.clangFormat = toolchain.clangFormat;
    }
    for (const char* tool : {"cmake", "ninja", "gnuMake", "msbuild"}) {
        const QString inherited = toolchain.buildTools.value(QLatin1String(tool));
        if (!inherited.isEmpty()
            && kit.buildTools.value(QLatin1String(tool)).isEmpty()) {
            kit.buildTools.insert(QLatin1String(tool), inherited);
        }
    }
}

QList<Kit> SettingsStore::kits() const
{
    const QList<Toolchain> allToolchains = toolchains();
    QList<Kit> out;
    for (const auto& entry : kitDocs_) {
        Kit kit;
        fromJson(entry.second, kit);
        if (kit.inheritToolchain && !kit.toolchainId.isEmpty()) {
            for (const Toolchain& toolchain : allToolchains) {
                if (toolchain.id == kit.toolchainId) {
                    applyToolchainInheritance(kit, toolchain);
                    break;
                }
            }
        }
        out.append(kit);
    }
    return out;
}

std::optional<Kit> SettingsStore::kit(const QString& id) const
{
    const QList<Kit> all = kits();
    for (const Kit& kit : all) {
        if (kit.id == id) {
            return kit;
        }
    }
    return std::nullopt;
}

std::optional<Kit> SettingsStore::defaultKit() const
{
    const GlobalDefaults defaults = this->defaults();
    const QList<Kit> all = kits();

    if (!defaults.defaultKitId.isEmpty()) {
        for (const Kit& kit : all) {
            if (kit.id == defaults.defaultKitId) {
                if (kit.enabled && kit.isValid()) {
                    return kit;
                }
                // Named but unusable: fall through to the other rules
                // rather than returning a kit that cannot compile.
                break;
            }
        }
    }
    for (const Kit& kit : all) {
        if (kit.enabled && kit.isValid() && kit.isDefault) {
            return kit;
        }
    }
    for (const Kit& kit : all) {
        if (kit.enabled && kit.isValid()) {
            return kit;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

QString SettingsStore::clangdPath() const
{
    const QString path = defaults().clangdPath;
    return path.isEmpty() ? fallbackClangd_ : path;
}

QString SettingsStore::clangFormatPath() const
{
    const QString path = defaults().clangFormatPath;
    return path.isEmpty() ? fallbackClangFormat_ : path;
}

QString SettingsStore::gitPath() const
{
    const QString path = defaults().gitPath;
    return path.isEmpty() ? fallbackGit_ : path;
}

QString SettingsStore::lineEndingForSaving() const
{
    const Preferences prefs = preferences();
    if (prefs.lineEndingDefault.compare(QLatin1String("CRLF"),
                                        Qt::CaseInsensitive) == 0) {
        return QStringLiteral("CRLF");
    }
    if (prefs.lineEndingDefault.compare(QLatin1String("LF"),
                                        Qt::CaseInsensitive) == 0) {
        return QStringLiteral("LF");
    }
    // "auto" (or unset): the platform's own convention.
#ifdef Q_OS_WIN
    return QStringLiteral("CRLF");
#else
    return QStringLiteral("LF");
#endif
}

EffectiveBuildConfig SettingsStore::effectiveBuildConfig(
    const Kit& kit, const QString& configName) const
{
    EffectiveBuildConfig out;
    out.name = configName;

    for (const KitBuildConfiguration& config : kit.buildConfigurations) {
        if (config.name != configName) {
            continue;
        }
        // inheritFromKit prepends the kit's own flags; the configuration's
        // flags come after, so a repeated flag is decided by the config.
        if (config.inheritFromKit) {
            out.compileFlags = kit.compileFlags + config.compileFlags;
            out.linkFlags = kit.linkFlags + config.linkFlags;
        } else {
            out.compileFlags = config.compileFlags;
            out.linkFlags = config.linkFlags;
        }
        out.cppStandard = config.cppStandard.isEmpty() ? kit.cppStandard
                                                       : config.cppStandard;
        out.buildToolArgs = config.buildToolArgs;
        return out;
    }

    // No configuration of that name: the kit's own flags stand for it.
    out.compileFlags = kit.compileFlags;
    out.linkFlags = kit.linkFlags;
    out.cppStandard = kit.cppStandard;
    return out;
}

QStringList SettingsStore::buildConfigurationNames(const Kit& kit) const
{
    QStringList names;
    for (const KitBuildConfiguration& config : kit.buildConfigurations) {
        if (!config.name.isEmpty()) {
            names.append(config.name);
        }
    }
    if (names.isEmpty()) {
        names = {QStringLiteral("Debug"), QStringLiteral("Release")};
    }
    return names;
}

QList<Toolchain> mergedToolchains(const QList<Toolchain>& base,
                                  const QList<Toolchain>& overlay)
{
    QList<Toolchain> out = base;
    for (const Toolchain& toolchain : overlay) {
        bool replaced = false;
        for (Toolchain& existing : out) {
            if (!toolchain.id.isEmpty() && existing.id == toolchain.id) {
                existing = toolchain;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            out.append(toolchain);
        }
    }
    return out;
}

std::optional<Toolchain> toolchainById(const QList<Toolchain>& toolchains,
                                       const QString& id)
{
    for (const Toolchain& toolchain : toolchains) {
        if (toolchain.id == id) {
            return toolchain;
        }
    }
    return std::nullopt;
}

}  // namespace LightCpp::Settings

