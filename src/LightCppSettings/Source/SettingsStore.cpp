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

#include <algorithm>

#ifdef Q_OS_WIN
#include <QSysInfo>
#endif

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

#ifdef Q_OS_WIN
[[nodiscard]] QString runningPlatform()
{
    return QStringLiteral("windows");
}
#else
[[nodiscard]] QString runningPlatform()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QStringLiteral("windows");
#endif
}
#endif

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
    return QDir(exeDir).filePath(QStringLiteral("config"));
}

bool SettingsStore::writeDocument(const QString& filePath,
                                  const QJsonDocument& document)
{
    const QFileInfo info(filePath);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray bytes =
        document.toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size();
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
    mergedToolchains_ = QJsonObject();
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
    if (QFileInfo(base).isDir() && kitDocs_.isEmpty() && mergedToolchains_.isEmpty()
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
    if (type == QLatin1String("toolchains")) {
        mergedToolchains_ = mergeObjects(mergedToolchains_, document);
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

ToolchainLists SettingsStore::toolchains() const
{
    ToolchainLists out;
    fromJson(mergedToolchains_, out);
    return out;
}

Preferences SettingsStore::preferences() const
{
    Preferences out;
    fromJson(mergedPreferences_, out);
    return out;
}

QList<Kit> SettingsStore::kits() const
{
    QList<Kit> out;
    for (const auto& entry : kitDocs_) {
        Kit kit;
        fromJson(entry.second, kit);
        out.append(kit);
    }
    return out;
}

std::optional<Kit> SettingsStore::kit(const QString& id) const
{
    for (const auto& entry : kitDocs_) {
        if (entry.first == id) {
            Kit kit;
            fromJson(entry.second, kit);
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
    // "auto" (or unset): the platform override wins, then the platform's
    // own convention.
    const QString overrideEnding =
        prefs.lineEndingPlatform.value(runningPlatform());
    if (overrideEnding.compare(QLatin1String("CRLF"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("CRLF");
    }
    if (overrideEnding.compare(QLatin1String("LF"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("LF");
    }
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

}  // namespace LightCpp::Settings
