// DefaultSettings — the starter configuration written on first run.

#include "LightCpp/App/DefaultSettings.hpp"

#include "LightCpp/App/MsvcToolchain.hpp"
#include "LightCpp/App/ToolchainPaths.hpp"
#include "LightCpp/Settings/SettingsJson.hpp"
#include "LightCpp/Settings/SettingsStore.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace LightCpp::App::DefaultSettings {

namespace {

using LightCpp::Settings::GlobalDefaults;
using LightCpp::Settings::Kit;
using LightCpp::Settings::KitBuildConfiguration;
using LightCpp::Settings::Preferences;
using LightCpp::Settings::Toolchain;

/// The first existing candidate, or empty when none does.
QString existingPath(const QStringList& candidates)
{
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

/// A compiler/tool inside the MSYS2 bin directory, when it exists.
QString msys2Tool(const QString& name)
{
    const QString path = QDir(LightCpp::App::kMingwBin).filePath(name);
    return QFileInfo::exists(path) ? path : QString();
}

/// The kit's buildTools object: every tool that exists is named, the rest
/// stay null — visible placeholders showing what a kit *can* name.
QHash<QString, QString> toolchainBuildTools(const QString& cmake, const QString& ninja,
                                            const QString& gnuMake)
{
    QHash<QString, QString> tools;
    tools.insert(QStringLiteral("cmake"), cmake);
    tools.insert(QStringLiteral("ninja"), ninja);
    tools.insert(QStringLiteral("gnuMake"), gnuMake);
    tools.insert(QStringLiteral("msbuild"), QString());
    return tools;
}

/// The Debug/Release configuration pair every starter kit ships: inherit
/// the kit's flags and add the optimisation level per configuration.
QList<KitBuildConfiguration> standardConfigurations(const QStringList& debugFlags,
                                                    const QStringList& releaseFlags,
                                                    const QString& cppStandard)
{
    KitBuildConfiguration debug;
    debug.name = QStringLiteral("Debug");
    debug.inheritFromKit = true;
    debug.compileFlags = debugFlags;
    debug.cppStandard = cppStandard;
    debug.buildToolArgs.insert(QStringLiteral("cmake"),
                               {QStringLiteral("-DCMAKE_BUILD_TYPE=Debug")});

    KitBuildConfiguration release;
    release.name = QStringLiteral("Release");
    release.inheritFromKit = true;
    release.compileFlags = releaseFlags;
    release.cppStandard = cppStandard;
    release.buildToolArgs.insert(QStringLiteral("cmake"),
                                 {QStringLiteral("-DCMAKE_BUILD_TYPE=Release")});
    return {debug, release};
}

/// The GNU kit for the MSYS2 UCRT64 toolchain (gcc/g++ or clang/clang++).
/// The kit inherits every tool from the toolchain named by `toolchainId`;
/// only its flags, standard and configurations are its own.
Kit gnuKit(const QString& id, const QString& name, const QString& toolchainId,
           bool isDefault, const QStringList& warningFlags,
           const QString& cppStandard)
{
    Kit kit;
    kit.id = id;
    kit.name = name;
    kit.enabled = true;
    kit.isDefault = isDefault;
    kit.inheritToolchain = true;
    kit.toolchainId = toolchainId;
    kit.cppStandard = cppStandard;
    kit.compileFlags = warningFlags;
    kit.linkFlags = warningFlags;
    kit.clangFormatStyle = QStringLiteral("file");
    kit.clangdFallbackStyle = QStringLiteral("file");
    kit.buildConfigurations = standardConfigurations(
        {QStringLiteral("-g"), QStringLiteral("-O0")},
        {QStringLiteral("-O2"), QStringLiteral("-DNDEBUG")}, cppStandard);
    return kit;
}

Toolchain msvcToolchain(const LightCpp::App::Msvc::Toolchain& toolchain)
{
    Toolchain out;
    out.id = QStringLiteral("msvc");
    out.name = toolchain.label;
    out.cCompiler = toolchain.clPath;
    out.cppCompiler = toolchain.clPath;
    return out;
}

Kit msvcKit(const QString& toolchainId)
{
    Kit kit;
    kit.id = QStringLiteral("msvc");
    kit.name = QStringLiteral("MSVC");
    kit.enabled = true;
    kit.isDefault = false;
    kit.inheritToolchain = true;
    kit.toolchainId = toolchainId;
    // The MSVC compile path builds its own base line (/nologo /W4 /EHsc /MT)
    // and takes the optimisation flags from the configuration below, like
    // the flagsFor() table it replaces.
    kit.cppStandard.clear();  // the compiler's own default
    kit.buildConfigurations =
        standardConfigurations({QStringLiteral("/Od")},
                               {QStringLiteral("/O2"), QStringLiteral("/DNDEBUG")},
                               QString());
    return kit;
}

Preferences starterPreferences()
{
    Preferences prefs;
    prefs.toolbarFont.family = QStringLiteral("Segoe UI");
    prefs.toolbarFont.size = 9;
    prefs.editorFont.family = QStringLiteral("Cascadia Mono");
    prefs.editorFont.size = 11;
    prefs.outputFont.family = QStringLiteral("Cascadia Mono");
    prefs.outputFont.size = 10;

    prefs.defaultEncoding = QStringLiteral("UTF-8");
    prefs.detectionPriority = {QStringLiteral("UTF-8 BOM"),
                               QStringLiteral("UTF-8"),
                               QStringLiteral("GB18030"),
                               QStringLiteral("GBK"),
                               QStringLiteral("UTF-16LE"),
                               QStringLiteral("UTF-16BE"),
                               QStringLiteral("Windows-1252")};
    prefs.lineEndingDefault = QStringLiteral("auto");

    prefs.shortcutClangd = QStringLiteral("Alt+G");
    prefs.shortcutClangFormat.clear();

    prefs.checkCompilersAtStartup = true;

    prefs.largeFile.syntaxHighlightDisabledAboveMB = 5;
    prefs.largeFile.indexDisabledAboveMB = 20;
    prefs.largeFile.parseDisabledAboveMB = 10;

    prefs.clangd.enabledByDefault = true;
    prefs.clangd.completion = true;
    prefs.clangd.diagnostics = true;
    prefs.clangd.indexing = true;
    prefs.clangd.fallbackStyle = QStringLiteral("file");

    prefs.clangFormat.enabled = true;
    prefs.clangFormat.formatOnSave = false;
    prefs.clangFormat.style = QStringLiteral("file");

    prefs.globalStyle = QStringLiteral("file");
    prefs.defaultStyleContent =
        QStringLiteral("BasedOnStyle: LLVM\nIndentWidth: 4\nColumnLimit: 100\n");

    // "Program Output" (captured in the IDE) is the default run mode;
    // "consolePauser" switches to the external ConsolePauser.exe console.
    prefs.programOutput = QStringLiteral("program");
    return prefs;
}

}  // namespace

WriteResult ensureDefaultConfigFiles(const QString& dir)
{
    WriteResult result;
    if (dir.isEmpty()) {
        return result;
    }

    const QString clangd = msys2Tool(QStringLiteral("clangd.exe"));
    const QString clangFormat = msys2Tool(QStringLiteral("clang-format.exe"));
    const QString git = existingPath(
        {LightCpp::App::kGitPath,
         QStandardPaths::findExecutable(QStringLiteral("git"))});

    // --- toolchains.json (an array of per-toolchain documents) --------------
    const QString gdb = msys2Tool(QStringLiteral("gdb.exe"));
    Toolchain msys2;
    msys2.id = QStringLiteral("msys2-ucrt64");
    msys2.name = QStringLiteral("MSYS2 UCRT64");
    for (const QString& c : {msys2Tool(QStringLiteral("gcc.exe")),
                             msys2Tool(QStringLiteral("clang.exe"))}) {
        if (!c.isEmpty()) {
            if (msys2.cCompiler.isEmpty()) {
                msys2.cCompiler = c;
            }
            if (!c.contains(QStringLiteral("clang"))) {
                msys2.cCompiler = c;  // prefer gcc when it exists
            }
        }
    }
    for (const QString& cpp : {msys2Tool(QStringLiteral("g++.exe")),
                               msys2Tool(QStringLiteral("clang++.exe"))}) {
        if (!cpp.isEmpty()) {
            if (msys2.cppCompiler.isEmpty()) {
                msys2.cppCompiler = cpp;
            }
            if (!cpp.contains(QStringLiteral("clang"))) {
                msys2.cppCompiler = cpp;  // prefer g++ when it exists
            }
        }
    }
    msys2.debugger = gdb;
    msys2.clangd = clangd;
    msys2.clangFormat = clangFormat;
    msys2.buildTools = toolchainBuildTools(
        msys2Tool(QStringLiteral("cmake.exe")),
        msys2Tool(QStringLiteral("ninja.exe")),
        msys2Tool(QStringLiteral("mingw32-make.exe")));

    QList<Toolchain> toolchains;
    if (msys2.isValid()) {
        toolchains.append(msys2);
    }
    const LightCpp::App::Msvc::Toolchain msvc = LightCpp::App::Msvc::detect();
    if (msvc.isValid()) {
        toolchains.append(msvcToolchain(msvc));
    }

    // --- kits.json -----------------------------------------------------------
    // Kits inherit their tools from the toolchains above; nothing here
    // names a compiler directly.
    QList<Kit> kits;
    if (msys2.isValid()) {
        kits.append(gnuKit(QStringLiteral("msys2-ucrt64-gcc"),
                           QStringLiteral("MSYS2 UCRT64 GCC"),
                           msys2.id, true,
                           {QStringLiteral("-Wall"), QStringLiteral("-Wextra"),
                            QStringLiteral("-fdiagnostics-color=always")},
                           QStringLiteral("gnu++20")));
        // A second, Clang-based kit as soon as clang++ exists — the pair is
        // the usual MSYS2 install and shows how multiple kits coexist.
        const QString clangCpp = msys2Tool(QStringLiteral("clang++.exe"));
        if (!clangCpp.isEmpty()) {
            kits.append(gnuKit(QStringLiteral("msys2-ucrt64-clang"),
                               QStringLiteral("MSYS2 UCRT64 Clang"),
                               msys2.id, false,
                               {QStringLiteral("-Wall"), QStringLiteral("-Wextra"),
                                QStringLiteral("-fcolor-diagnostics")},
                               QStringLiteral("c++20")));
        }
    }
    if (msvc.isValid()) {
        kits.append(msvcKit(msvcToolchain(msvc).id));
    }

    // --- defaults.json -------------------------------------------------------
    GlobalDefaults defaults;
    if (!kits.isEmpty()) {
        defaults.defaultKitId = kits.first().id;
    }
    defaults.defaultBuildConfiguration = QStringLiteral("Debug");
    defaults.gitPath = git;
    defaults.clangdPath = clangd;
    defaults.clangFormatPath = clangFormat;

    // Write what there is to write; skip files the user already has.
    const auto writeOnce = [&dir, &result](const QString& fileName,
                                           const QJsonDocument& document) {
        const QString path = QDir(dir).filePath(fileName);
        if (QFileInfo::exists(path)) {
            result.kept.append(path);
            return;
        }
        if (LightCpp::Settings::SettingsStore::writeDocument(path, document)) {
            result.written.append(path);
        }
    };

    if (!toolchains.isEmpty()) {
        QJsonArray toolchainArray;
        for (const Toolchain& toolchain : toolchains) {
            toolchainArray.append(LightCpp::Settings::toJson(toolchain));
        }
        writeOnce(QStringLiteral("toolchains.json"), QJsonDocument(toolchainArray));
    }
    if (!kits.isEmpty()) {
        QJsonArray kitArray;
        for (const Kit& kit : kits) {
            kitArray.append(LightCpp::Settings::toJson(kit));
        }
        writeOnce(QStringLiteral("kits.json"), QJsonDocument(kitArray));
    }
    if (!defaults.defaultKitId.isEmpty() || !defaults.gitPath.isEmpty()) {
        writeOnce(QStringLiteral("defaults.json"),
                  QJsonDocument(LightCpp::Settings::toJson(defaults)));
    }
    writeOnce(QStringLiteral("preferences.json"),
              QJsonDocument(LightCpp::Settings::toJson(starterPreferences())));

    return result;
}

}  // namespace LightCpp::App::DefaultSettings
