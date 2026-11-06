// SettingsJson — typed (de)serialisation for the configuration documents.

#include "LightCpp/Settings/SettingsJson.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>

namespace LightCpp::Settings {

namespace {

/// The four build-tool keys every buildTools object understands, in a fixed
/// order so generated JSON is stable and readable.
const char* const kBuildToolKeys[] = {"cmake", "ninja", "gnuMake", "msbuild"};

[[nodiscard]] QStringList toStringList(const QJsonValue& value)
{
    QStringList out;
    const QJsonArray array = value.toArray();
    for (const QJsonValue& item : array) {
        if (item.isString()) {
            out.append(item.toString());
        }
    }
    return out;
}

[[nodiscard]] QJsonArray fromStringList(const QStringList& list)
{
    QJsonArray array;
    for (const QString& item : list) {
        array.append(item);
    }
    return array;
}

void readString(const QJsonObject& object, const char* key, QString& out)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isString()) {
        out = value.toString();
    }
}

void readBool(const QJsonObject& object, const char* key, bool& out)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isBool()) {
        out = value.toBool();
    }
}

void readInt(const QJsonObject& object, const char* key, int& out)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isDouble()) {
        out = value.toInt();
    }
}

void readStringList(const QJsonObject& object, const char* key, QStringList& out)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isArray()) {
        out = toStringList(value);
    }
}

/// A string-or-null field: JSON null leaves the current value untouched,
/// which is how a kit overlay can clear an optional path... by simply not
/// mentioning it. (An explicit "" is the way to say "unset".)
void readOptionalString(const QJsonObject& object, const char* key, QString& out)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isString()) {
        out = value.toString();
    }
}

/// A plain string field read into a local (legacy documents only).
[[nodiscard]] QString readLegacyString(const QJsonObject& object, const char* key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : QString();
}

void readFontSpec(const QJsonObject& object, const char* key, FontSpec& out)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isObject()) {
        return;
    }
    const QJsonObject font = value.toObject();
    readString(font, "family", out.family);
    const QJsonValue size = font.value(QLatin1String("size"));
    if (size.isDouble()) {
        out.size = size.toDouble();
    }
}

/// buildTools objects map tool name -> single path (kit) or path list
/// (registry). Unknown tool names are ignored: the four supported keys are
/// the contract, and a typo should not silently register a sixth tool.
void readBuildToolMap(const QJsonObject& object, const char* key,
                      QHash<QString, QStringList>& out)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isObject()) {
        return;
    }
    const QJsonObject tools = value.toObject();
    for (const char* tool : kBuildToolKeys) {
        const QJsonValue entry = tools.value(QLatin1String(tool));
        if (entry.isArray()) {
            out.insert(QLatin1String(tool), toStringList(entry));
        }
    }
}

void writeBuildToolMap(QJsonObject& object, const char* key,
                       const QHash<QString, QStringList>& map)
{
    QJsonObject tools;
    for (const char* tool : kBuildToolKeys) {
        tools.insert(QLatin1String(tool),
                     fromStringList(map.value(QLatin1String(tool))));
    }
    object.insert(QLatin1String(key), tools);
}

void readBuildToolSingleMap(const QJsonObject& object, const char* key,
                            QHash<QString, QString>& out)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isObject()) {
        return;
    }
    const QJsonObject tools = value.toObject();
    for (const char* tool : kBuildToolKeys) {
        const QJsonValue entry = tools.value(QLatin1String(tool));
        // A string sets the path; null / absent leaves it unset. Anything
        // else (a stray array) is a config mistake and is ignored.
        if (entry.isString()) {
            out.insert(QLatin1String(tool), entry.toString());
        }
    }
}

void writeBuildToolSingleMap(QJsonObject& object, const char* key,
                             const QHash<QString, QString>& map)
{
    QJsonObject tools;
    for (const char* tool : kBuildToolKeys) {
        const QString path = map.value(QLatin1String(tool));
        if (path.isEmpty()) {
            tools.insert(QLatin1String(tool), QJsonValue::Null);
        } else {
            tools.insert(QLatin1String(tool), path);
        }
    }
    object.insert(QLatin1String(key), tools);
}

/// The fixed compiler/debugger keys a toolchain's `compilers` object can
/// carry. Anything outside this set is ignored on read.
constexpr const char* kCompilerKeys[] = {
    "gcc", "g++", "clang", "clang++", "cl", "gdb", "lldb", "cdb"};

}  // namespace

// ---------------------------------------------------------------------------
// toJson
// ---------------------------------------------------------------------------

QJsonObject toJson(const Toolchain& toolchain)
{
    QJsonObject object;
    object.insert(QStringLiteral("type"), QStringLiteral("toolchain"));
    object.insert(QStringLiteral("id"), toolchain.id);
    object.insert(QStringLiteral("name"), toolchain.name);
    QJsonObject compilers;
    for (const char* key : kCompilerKeys) {
        const QString path =
            toolchain.compilers.value(QLatin1String(key));
        compilers.insert(QLatin1String(key),
                         path.isEmpty() ? QJsonValue::Null
                                        : QJsonValue(path));
    }
    object.insert(QStringLiteral("compilers"), compilers);
    object.insert(QStringLiteral("clangd"),
                  toolchain.clangd.isEmpty() ? QJsonValue::Null
                                             : QJsonValue(toolchain.clangd));
    object.insert(QStringLiteral("clangFormat"),
                  toolchain.clangFormat.isEmpty()
                      ? QJsonValue::Null
                      : QJsonValue(toolchain.clangFormat));
    writeBuildToolSingleMap(object, "buildTools", toolchain.buildTools);
    return object;
}

QJsonObject toJson(const GlobalDefaults& defaults)
{
    QJsonObject object;
    object.insert(QStringLiteral("type"), QStringLiteral("defaults"));
    object.insert(QStringLiteral("id"), QStringLiteral("global"));
    object.insert(QStringLiteral("defaultKitId"), defaults.defaultKitId);
    object.insert(QStringLiteral("defaultBuildConfiguration"),
                  defaults.defaultBuildConfiguration);
    object.insert(QStringLiteral("gitPath"), defaults.gitPath);
    object.insert(QStringLiteral("clangdPath"), defaults.clangdPath);
    object.insert(QStringLiteral("clangFormatPath"), defaults.clangFormatPath);
    return object;
}

QJsonObject toJson(const KitBuildConfiguration& config)
{
    QJsonObject object;
    object.insert(QStringLiteral("name"), config.name);
    object.insert(QStringLiteral("inheritFromKit"), config.inheritFromKit);
    object.insert(QStringLiteral("compileFlags"), fromStringList(config.compileFlags));
    object.insert(QStringLiteral("linkFlags"), fromStringList(config.linkFlags));
    object.insert(QStringLiteral("cppStandard"), config.cppStandard);
    writeBuildToolMap(object, "buildToolArgs", config.buildToolArgs);
    return object;
}

QJsonObject toJson(const Kit& kit)
{
    QJsonObject object;
    object.insert(QStringLiteral("type"), QStringLiteral("kit"));
    object.insert(QStringLiteral("id"), kit.id);
    object.insert(QStringLiteral("name"), kit.name);
    object.insert(QStringLiteral("enabled"), kit.enabled);
    object.insert(QStringLiteral("isDefault"), kit.isDefault);
    object.insert(QStringLiteral("inheritToolchain"), kit.inheritToolchain);
    object.insert(QStringLiteral("toolchainId"),
                  kit.toolchainId.isEmpty() ? QJsonValue::Null
                                            : QJsonValue(kit.toolchainId));
    object.insert(QStringLiteral("compilerFamily"),
                  kit.compilerFamily.isEmpty()
                      ? QJsonValue::Null
                      : QJsonValue(kit.compilerFamily));
    object.insert(QStringLiteral("cCompiler"), kit.cCompiler);
    object.insert(QStringLiteral("cppCompiler"), kit.cppCompiler);
    object.insert(QStringLiteral("debugger"),
                  kit.debugger.isEmpty() ? QJsonValue::Null : QJsonValue(kit.debugger));
    object.insert(QStringLiteral("clangd"),
                  kit.clangd.isEmpty() ? QJsonValue::Null : QJsonValue(kit.clangd));
    object.insert(QStringLiteral("clangFormat"),
                  kit.clangFormat.isEmpty() ? QJsonValue::Null
                                            : QJsonValue(kit.clangFormat));
    writeBuildToolSingleMap(object, "buildTools", kit.buildTools);
    object.insert(QStringLiteral("cppStandard"), kit.cppStandard);
    object.insert(QStringLiteral("compileFlags"), fromStringList(kit.compileFlags));
    object.insert(QStringLiteral("linkFlags"), fromStringList(kit.linkFlags));
    QJsonObject git;
    git.insert(QStringLiteral("path"), kit.gitPath);
    git.insert(QStringLiteral("args"), fromStringList(kit.gitArgs));
    object.insert(QStringLiteral("git"), git);
    object.insert(QStringLiteral("clangFormatStyle"),
                  kit.clangFormatStyle.isEmpty() ? QJsonValue::Null
                                                 : QJsonValue(kit.clangFormatStyle));
    object.insert(QStringLiteral("clangdFallbackStyle"),
                  kit.clangdFallbackStyle.isEmpty()
                      ? QJsonValue::Null
                      : QJsonValue(kit.clangdFallbackStyle));
    QJsonArray configs;
    for (const KitBuildConfiguration& config : kit.buildConfigurations) {
        configs.append(toJson(config));
    }
    object.insert(QStringLiteral("buildConfigurations"), configs);
    return object;
}

QJsonObject toJson(const Preferences& prefs)
{
    QJsonObject object;
    object.insert(QStringLiteral("type"), QStringLiteral("preferences"));
    object.insert(QStringLiteral("id"), QStringLiteral("global"));

    QJsonObject fonts;
    auto writeFont = [&fonts](const char* key, const FontSpec& font) {
        if (!font.isValid()) {
            return;
        }
        QJsonObject fontObject;
        fontObject.insert(QStringLiteral("family"), font.family);
        fontObject.insert(QStringLiteral("size"), font.size);
        fonts.insert(QLatin1String(key), fontObject);
    };
    writeFont("toolbar", prefs.toolbarFont);
    writeFont("editor", prefs.editorFont);
    writeFont("output", prefs.outputFont);
    object.insert(QStringLiteral("fonts"), fonts);

    QJsonObject files;
    QJsonObject encoding;
    encoding.insert(QStringLiteral("default"), prefs.defaultEncoding);
    encoding.insert(QStringLiteral("detectionPriority"),
                    fromStringList(prefs.detectionPriority));
    files.insert(QStringLiteral("encoding"), encoding);
    QJsonObject lineEnding;
    lineEnding.insert(QStringLiteral("default"), prefs.lineEndingDefault);
    files.insert(QStringLiteral("lineEnding"), lineEnding);
    files.insert(QStringLiteral("longLineMode"),
                 prefs.longLineMode.isEmpty() ? QStringLiteral("scroll")
                                              : prefs.longLineMode);
    files.insert(QStringLiteral("showWhitespaces"), prefs.showWhitespaces);
    files.insert(QStringLiteral("showIndentGuides"), prefs.showIndentGuides);
    object.insert(QStringLiteral("files"), files);

    QJsonObject general;
    general.insert(QStringLiteral("checkCompilersAtStartup"),
                   prefs.checkCompilersAtStartup);
    general.insert(QStringLiteral("showStartupPage"), prefs.showStartupPage);
    general.insert(QStringLiteral("restoreSession"), prefs.restoreSession);
    general.insert(QStringLiteral("checkToolchainsAtStartup"),
                   prefs.checkToolchainsAtStartup);
    general.insert(QStringLiteral("autoSave"), prefs.autoSave);
    general.insert(QStringLiteral("autoSaveSeconds"), prefs.autoSaveSeconds);
    general.insert(QStringLiteral("recentFiles"),
                   fromStringList(prefs.recentFiles));
    object.insert(QStringLiteral("general"), general);

    QJsonObject shortcuts;
    shortcuts.insert(QStringLiteral("clangd"),
                     prefs.shortcutClangd.isEmpty()
                         ? QJsonValue::Null
                         : QJsonValue(prefs.shortcutClangd));
    shortcuts.insert(QStringLiteral("clangFormat"),
                     prefs.shortcutClangFormat.isEmpty()
                         ? QJsonValue::Null
                         : QJsonValue(prefs.shortcutClangFormat));
    object.insert(QStringLiteral("shortcuts"), shortcuts);

    QJsonObject performance;
    QJsonObject largeFile;
    largeFile.insert(QStringLiteral("syntaxHighlightDisabledAboveMB"),
                     prefs.largeFile.syntaxHighlightDisabledAboveMB);
    largeFile.insert(QStringLiteral("indexDisabledAboveMB"),
                     prefs.largeFile.indexDisabledAboveMB);
    largeFile.insert(QStringLiteral("parseDisabledAboveMB"),
                     prefs.largeFile.parseDisabledAboveMB);
    performance.insert(QStringLiteral("largeFile"), largeFile);
    object.insert(QStringLiteral("performance"), performance);

    // The debugger block sits next to "performance" rather than inside
    // "languageService": the debugger is not a language service, and grouping
    // it there is how it ended up with nowhere to live at all.
    QJsonObject debugger;
    debugger.insert(QStringLiteral("prettyPrinting"), prefs.debugger.prettyPrinting);
    debugger.insert(QStringLiteral("showEnumerators"), prefs.debugger.showEnumerators);
    debugger.insert(QStringLiteral("dynamicType"), prefs.debugger.dynamicType);
    debugger.insert(QStringLiteral("sourceMappings"),
                    fromStringList(prefs.debugger.sourceMappings));
    debugger.insert(QStringLiteral("symbolSearchPaths"),
                    fromStringList(prefs.debugger.symbolSearchPaths));
    debugger.insert(QStringLiteral("natvisFiles"),
                    fromStringList(prefs.debugger.natvisFiles));
    object.insert(QStringLiteral("debugger"), debugger);

    QJsonObject languageService;
    QJsonObject clangd;
    clangd.insert(QStringLiteral("enabledByDefault"), prefs.clangd.enabledByDefault);
    clangd.insert(QStringLiteral("completion"), prefs.clangd.completion);
    clangd.insert(QStringLiteral("diagnostics"), prefs.clangd.diagnostics);
    clangd.insert(QStringLiteral("indexing"), prefs.clangd.indexing);
    clangd.insert(QStringLiteral("fallbackStyle"), prefs.clangd.fallbackStyle);
    languageService.insert(QStringLiteral("clangd"), clangd);
    QJsonObject completion;
    completion.insert(QStringLiteral("backend"), prefs.completion.backend);
    completion.insert(QStringLiteral("retryCount"), prefs.completion.retryCount);
    languageService.insert(QStringLiteral("completion"), completion);
    QJsonObject clangFormat;
    clangFormat.insert(QStringLiteral("enabled"), prefs.clangFormat.enabled);
    clangFormat.insert(QStringLiteral("formatOnSave"), prefs.clangFormat.formatOnSave);
    clangFormat.insert(QStringLiteral("style"), prefs.clangFormat.style);
    languageService.insert(QStringLiteral("clangFormat"), clangFormat);
    object.insert(QStringLiteral("languageService"), languageService);

    QJsonObject formatting;
    formatting.insert(QStringLiteral("globalStyle"), prefs.globalStyle);
    formatting.insert(QStringLiteral("defaultStyleContent"), prefs.defaultStyleContent);
    object.insert(QStringLiteral("formatting"), formatting);

    QJsonObject run;
    run.insert(QStringLiteral("output"), prefs.programOutput);
    QJsonArray terminals;
    for (const RunTerminal& terminal : prefs.terminals) {
        terminals.append(QJsonObject{
            {QStringLiteral("id"), terminal.id},
            {QStringLiteral("name"), terminal.name},
            {QStringLiteral("command"), terminal.command},
            {QStringLiteral("embedded"), terminal.embedded},
        });
    }
    run.insert(QStringLiteral("terminals"), terminals);
    object.insert(QStringLiteral("run"), run);

    QJsonArray disabled;
    for (const QString& path : prefs.disabledPlugins) {
        disabled.append(path);
    }
    QJsonArray pendingDeletes;
    for (const QString& path : prefs.pendingPluginDeletes) {
        pendingDeletes.append(path);
    }
    QJsonObject plugins;
    plugins.insert(QStringLiteral("disabled"), disabled);
    plugins.insert(QStringLiteral("pendingDeletes"), pendingDeletes);
    object.insert(QStringLiteral("plugins"), plugins);

    return object;
}

// ---------------------------------------------------------------------------
// fromJson
// ---------------------------------------------------------------------------

void fromJson(const QJsonObject& object, Toolchain& out)
{
    readString(object, "id", out.id);
    readString(object, "name", out.name);
    const QJsonValue compilersValue = object.value(QLatin1String("compilers"));
    if (compilersValue.isObject()) {
        const QJsonObject compilers = compilersValue.toObject();
        for (const char* key : kCompilerKeys) {
            const QJsonValue value = compilers.value(QLatin1String(key));
            if (value.isString() && !value.toString().isEmpty()) {
                out.compilers.insert(QLatin1String(key), value.toString());
            }
        }
    } else {
        // Legacy document (pre-compilers map): the old single-compiler
        // fields map onto the new keys by file name.
        const QString c = readLegacyString(object, "cCompiler");
        const QString cpp = readLegacyString(object, "cppCompiler");
        const QString dbg = readLegacyString(object, "debugger");
        auto legacyKey = [](const QString& path) -> QString {
            const QString name = QFileInfo(path).fileName().toLower();
            if (name.startsWith(QLatin1String("clang++"))) {
                return QStringLiteral("clang++");
            }
            if (name.startsWith(QLatin1String("clang"))) {
                return QStringLiteral("clang");
            }
            if (name == QLatin1String("cl.exe") || name == QLatin1String("cl")) {
                return QStringLiteral("cl");
            }
            if (name == QLatin1String("g++.exe") || name == QLatin1String("g++")) {
                return QStringLiteral("g++");
            }
            return QStringLiteral("gcc");
        };
        if (!c.isEmpty()) {
            out.compilers.insert(legacyKey(c), c);
        }
        if (!cpp.isEmpty()) {
            out.compilers.insert(legacyKey(cpp), cpp);
        }
        if (!dbg.isEmpty()) {
            const QString name = QFileInfo(dbg).fileName().toLower();
            out.compilers.insert(name.startsWith(QLatin1String("lldb"))
                                     ? QStringLiteral("lldb")
                                     : QStringLiteral("gdb"),
                                 dbg);
        }
    }
    readOptionalString(object, "clangd", out.clangd);
    readOptionalString(object, "clangFormat", out.clangFormat);
    readBuildToolSingleMap(object, "buildTools", out.buildTools);
}

void fromJson(const QJsonObject& object, GlobalDefaults& out)
{
    readString(object, "defaultKitId", out.defaultKitId);
    readString(object, "defaultBuildConfiguration", out.defaultBuildConfiguration);
    readString(object, "gitPath", out.gitPath);
    readString(object, "clangdPath", out.clangdPath);
    readString(object, "clangFormatPath", out.clangFormatPath);
}

void fromJson(const QJsonObject& object, KitBuildConfiguration& out)
{
    readString(object, "name", out.name);
    readBool(object, "inheritFromKit", out.inheritFromKit);
    readStringList(object, "compileFlags", out.compileFlags);
    readStringList(object, "linkFlags", out.linkFlags);
    readOptionalString(object, "cppStandard", out.cppStandard);
    readBuildToolMap(object, "buildToolArgs", out.buildToolArgs);
}

void fromJson(const QJsonObject& object, Kit& out)
{
    readString(object, "id", out.id);
    readString(object, "name", out.name);
    readBool(object, "enabled", out.enabled);
    readBool(object, "isDefault", out.isDefault);
    readBool(object, "inheritToolchain", out.inheritToolchain);
    readOptionalString(object, "toolchainId", out.toolchainId);
    readOptionalString(object, "compilerFamily", out.compilerFamily);
    readString(object, "cCompiler", out.cCompiler);
    readString(object, "cppCompiler", out.cppCompiler);
    readOptionalString(object, "debugger", out.debugger);
    readOptionalString(object, "clangd", out.clangd);
    readOptionalString(object, "clangFormat", out.clangFormat);
    readBuildToolSingleMap(object, "buildTools", out.buildTools);
    readOptionalString(object, "cppStandard", out.cppStandard);
    readStringList(object, "compileFlags", out.compileFlags);
    readStringList(object, "linkFlags", out.linkFlags);
    const QJsonValue gitValue = object.value(QLatin1String("git"));
    if (gitValue.isObject()) {
        const QJsonObject git = gitValue.toObject();
        readString(git, "path", out.gitPath);
        readStringList(git, "args", out.gitArgs);
    }
    readOptionalString(object, "clangFormatStyle", out.clangFormatStyle);
    readOptionalString(object, "clangdFallbackStyle", out.clangdFallbackStyle);
    const QJsonValue configsValue = object.value(QLatin1String("buildConfigurations"));
    if (configsValue.isArray()) {
        out.buildConfigurations.clear();
        for (const QJsonValue& item : configsValue.toArray()) {
            if (!item.isObject()) {
                continue;
            }
            KitBuildConfiguration config;
            fromJson(item.toObject(), config);
            // A configuration without a name cannot be selected; skip it.
            if (!config.name.isEmpty()) {
                out.buildConfigurations.append(config);
            }
        }
    }
}

void fromJson(const QJsonObject& object, Preferences& out)
{
    const QJsonValue fontsValue = object.value(QLatin1String("fonts"));
    if (fontsValue.isObject()) {
        const QJsonObject fonts = fontsValue.toObject();
        readFontSpec(fonts, "toolbar", out.toolbarFont);
        readFontSpec(fonts, "editor", out.editorFont);
        readFontSpec(fonts, "output", out.outputFont);
    }

    const QJsonValue filesValue = object.value(QLatin1String("files"));
    if (filesValue.isObject()) {
        const QJsonObject files = filesValue.toObject();
        const QJsonValue encodingValue = files.value(QLatin1String("encoding"));
        if (encodingValue.isObject()) {
            const QJsonObject encoding = encodingValue.toObject();
            readString(encoding, "default", out.defaultEncoding);
            readStringList(encoding, "detectionPriority", out.detectionPriority);
        }
        const QJsonValue lineEndingValue = files.value(QLatin1String("lineEnding"));
        if (lineEndingValue.isObject()) {
            const QJsonObject lineEnding = lineEndingValue.toObject();
            readString(lineEnding, "default", out.lineEndingDefault);
        }
        readString(files, "longLineMode", out.longLineMode);
        // Anything but the two known modes means the file says nothing useful.
        if (out.longLineMode != QLatin1String("scroll")
            && out.longLineMode != QLatin1String("wrap")) {
            out.longLineMode = QStringLiteral("scroll");
        }
        readBool(files, "showWhitespaces", out.showWhitespaces);
        readBool(files, "showIndentGuides", out.showIndentGuides);
        // Legacy "platformOverrides" objects are deliberately not read: the
        // per-platform line-ending overrides were removed from the model.
    }
    const QJsonValue generalValue = object.value(QLatin1String("general"));
    if (generalValue.isObject()) {
        const QJsonObject general = generalValue.toObject();
        readBool(general, "checkCompilersAtStartup", out.checkCompilersAtStartup);
        readBool(general, "showStartupPage", out.showStartupPage);
        readBool(general, "restoreSession", out.restoreSession);
        readBool(general, "checkToolchainsAtStartup",
                 out.checkToolchainsAtStartup);
        readBool(general, "autoSave", out.autoSave);
        if (const auto seconds = general.value(QLatin1String("autoSaveSeconds"));
            seconds.isDouble()) {
            const int v = int(seconds.toDouble());
            // Clamp instead of trusting: 5 s to 1 h keeps the timer sane.
            out.autoSaveSeconds = qBound(5, v, 3600);
        }
        readStringList(general, "recentFiles", out.recentFiles);
    }

    const QJsonValue debuggerValue = object.value(QLatin1String("debugger"));
    if (debuggerValue.isObject()) {
        const QJsonObject debugger = debuggerValue.toObject();
        readBool(debugger, "prettyPrinting", out.debugger.prettyPrinting);
        readBool(debugger, "showEnumerators", out.debugger.showEnumerators);
        readBool(debugger, "dynamicType", out.debugger.dynamicType);
        readStringList(debugger, "sourceMappings", out.debugger.sourceMappings);
        readStringList(debugger, "symbolSearchPaths", out.debugger.symbolSearchPaths);
        readStringList(debugger, "natvisFiles", out.debugger.natvisFiles);
        // A from without its to is half a rule; dropping the tail is better
        // than pairing it with an empty path, which would rewrite everything
        // under the source root to the root itself.
        if (out.debugger.sourceMappings.size() % 2 != 0) {
            out.debugger.sourceMappings.removeLast();
        }
    }

    const QJsonValue shortcutsValue = object.value(QLatin1String("shortcuts"));
    if (shortcutsValue.isObject()) {
        const QJsonObject shortcuts = shortcutsValue.toObject();
        readOptionalString(shortcuts, "clangd", out.shortcutClangd);
        readOptionalString(shortcuts, "clangFormat", out.shortcutClangFormat);
    }

    const QJsonValue performanceValue = object.value(QLatin1String("performance"));
    if (performanceValue.isObject()) {
        const QJsonObject performance = performanceValue.toObject();
        const QJsonValue largeValue = performance.value(QLatin1String("largeFile"));
        if (largeValue.isObject()) {
            const QJsonObject large = largeValue.toObject();
            readInt(large, "syntaxHighlightDisabledAboveMB",
                    out.largeFile.syntaxHighlightDisabledAboveMB);
            readInt(large, "indexDisabledAboveMB", out.largeFile.indexDisabledAboveMB);
            readInt(large, "parseDisabledAboveMB", out.largeFile.parseDisabledAboveMB);
        }
    }

    const QJsonValue languageValue = object.value(QLatin1String("languageService"));
    if (languageValue.isObject()) {
        const QJsonObject language = languageValue.toObject();
        const QJsonValue clangdValue = language.value(QLatin1String("clangd"));
        if (clangdValue.isObject()) {
            const QJsonObject clangd = clangdValue.toObject();
            readBool(clangd, "enabledByDefault", out.clangd.enabledByDefault);
            readBool(clangd, "completion", out.clangd.completion);
            readBool(clangd, "diagnostics", out.clangd.diagnostics);
            readBool(clangd, "indexing", out.clangd.indexing);
            readOptionalString(clangd, "fallbackStyle", out.clangd.fallbackStyle);
        }
        const QJsonValue completionValue = language.value(QLatin1String("completion"));
        if (completionValue.isObject()) {
            const QJsonObject completion = completionValue.toObject();
            readString(completion, "backend", out.completion.backend);
            readInt(completion, "retryCount", out.completion.retryCount);
            if (out.completion.retryCount < 0) {
                out.completion.retryCount = 0;
            }
        }
        const QJsonValue formatValue = language.value(QLatin1String("clangFormat"));
        if (formatValue.isObject()) {
            const QJsonObject format = formatValue.toObject();
            readBool(format, "enabled", out.clangFormat.enabled);
            readBool(format, "formatOnSave", out.clangFormat.formatOnSave);
            readOptionalString(format, "style", out.clangFormat.style);
        }
    }

    const QJsonValue formattingValue = object.value(QLatin1String("formatting"));
    if (formattingValue.isObject()) {
        const QJsonObject formatting = formattingValue.toObject();
        readString(formatting, "globalStyle", out.globalStyle);
        readString(formatting, "defaultStyleContent", out.defaultStyleContent);
    }

    const QJsonValue runValue = object.value(QLatin1String("run"));
    if (runValue.isObject()) {
        const QJsonObject run = runValue.toObject();
        readString(run, "output", out.programOutput);
        // Legacy documents have no terminals key: keep the seeded defaults
        // so "cmd"/"powershell" selections keep resolving. A present-but-
        // empty array means the user deleted every entry — honour that.
        if (run.contains(QLatin1String("terminals"))) {
            out.terminals.clear();
            const QJsonValue terminalsValue = run.value(QLatin1String("terminals"));
            if (terminalsValue.isArray()) {
                for (const QJsonValue& item : terminalsValue.toArray()) {
                    if (!item.isObject()) {
                        continue;
                    }
                    const QJsonObject terminal = item.toObject();
                    RunTerminal parsed;
                    readString(terminal, "id", parsed.id);
                    readString(terminal, "name", parsed.name);
                    readString(terminal, "command", parsed.command);
                    readBool(terminal, "embedded", parsed.embedded);
                    if (!parsed.id.isEmpty() && !parsed.name.isEmpty()) {
                        out.terminals.append(parsed);
                    }
                }
            }
        }
    }

    const QJsonValue pluginsValue = object.value(QLatin1String("plugins"));
    if (pluginsValue.isObject()) {
        const QJsonObject plugins = pluginsValue.toObject();
        readStringList(plugins, "disabled", out.disabledPlugins);
        readStringList(plugins, "pendingDeletes", out.pendingPluginDeletes);
    }
}

}  // namespace LightCpp::Settings
