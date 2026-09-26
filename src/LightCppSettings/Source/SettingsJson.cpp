// SettingsJson — typed (de)serialisation for the configuration documents.

#include "LightCpp/Settings/SettingsJson.hpp"

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
    object.insert(QStringLiteral("cCompiler"), toolchain.cCompiler);
    object.insert(QStringLiteral("cppCompiler"), toolchain.cppCompiler);
    object.insert(QStringLiteral("debugger"),
                  toolchain.debugger.isEmpty() ? QJsonValue::Null
                                               : QJsonValue(toolchain.debugger));
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
    object.insert(QStringLiteral("files"), files);

    QJsonObject general;
    general.insert(QStringLiteral("checkCompilersAtStartup"),
                   prefs.checkCompilersAtStartup);
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
    object.insert(QStringLiteral("run"), run);

    return object;
}

// ---------------------------------------------------------------------------
// fromJson
// ---------------------------------------------------------------------------

void fromJson(const QJsonObject& object, Toolchain& out)
{
    readString(object, "id", out.id);
    readString(object, "name", out.name);
    readString(object, "cCompiler", out.cCompiler);
    readString(object, "cppCompiler", out.cppCompiler);
    readOptionalString(object, "debugger", out.debugger);
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
        // Legacy "platformOverrides" objects are deliberately not read: the
        // per-platform line-ending overrides were removed from the model.
    }
    const QJsonValue generalValue = object.value(QLatin1String("general"));
    if (generalValue.isObject()) {
        const QJsonObject general = generalValue.toObject();
        readBool(general, "checkCompilersAtStartup", out.checkCompilersAtStartup);
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
    }
}

}  // namespace LightCpp::Settings
