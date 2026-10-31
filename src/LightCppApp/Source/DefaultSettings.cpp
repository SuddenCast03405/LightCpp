// DefaultSettings — the starter configuration written on first run.

#include "LightCpp/App/DefaultSettings.hpp"

#include "LightCpp/App/ToolchainPaths.hpp"
#include "LightCpp/Settings/SettingsJson.hpp"
#include "LightCpp/Settings/SettingsStore.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace LightCpp::App::DefaultSettings {

namespace {

using LightCpp::Settings::GlobalDefaults;
using LightCpp::Settings::Kit;
using LightCpp::Settings::KitBuildConfiguration;
using LightCpp::Settings::Preferences;
using LightCpp::Settings::Toolchain;

/// The bin directory of a toolchain shipped next to the IDE:
/// <exe>/MinGW64, <exe>/Clang64, <exe>/MinGW32, <exe>/Clang32 — the first
/// one that exists AND carries a compiler in its bin. Nothing else is
/// identified: when none of the four exists, no default toolchain is
/// generated at all (MSVC lives elsewhere and is never identified here).
QString portableToolchainBin()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    for (const QString& name : {QStringLiteral("MinGW64"),
                                QStringLiteral("Clang64"),
                                QStringLiteral("MinGW32"),
                                QStringLiteral("Clang32")}) {
        const QString bin = QDir(exeDir).filePath(
            name + QStringLiteral("/bin"));
        const bool hasCompiler =
            QFileInfo::exists(QDir(bin).filePath(QStringLiteral("g++")))
            || QFileInfo::exists(QDir(bin).filePath(QStringLiteral("g++.exe")))
            || QFileInfo::exists(QDir(bin).filePath(QStringLiteral("gcc")))
            || QFileInfo::exists(QDir(bin).filePath(QStringLiteral("gcc.exe")))
            || QFileInfo::exists(QDir(bin).filePath(QStringLiteral("clang++")))
            || QFileInfo::exists(QDir(bin).filePath(QStringLiteral("clang++.exe")))
            || QFileInfo::exists(QDir(bin).filePath(QStringLiteral("clang")))
            || QFileInfo::exists(QDir(bin).filePath(QStringLiteral("clang.exe")));
        if (hasCompiler) {
            return bin;
        }
    }
    // A system installation is just as valid as a toolchain bundled beside
    // the IDE. This is the normal path on Linux and macOS.
    return LightCpp::App::ToolchainPaths::mingwBin();
}

/// First existing executable among `names` inside `bin`, or empty.
QString binTool(const QString& bin,
                const std::initializer_list<const char*>& names)
{
    for (const char* name : names) {
        const QString path = QDir(bin).filePath(QString::fromLatin1(name));
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return QString();
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

/// A toolchain document wired to one bin directory: every compiler and
/// debugger found there is recorded under its own key ("gcc", "g++",
/// "clang", "clang++", "gdb", "lldb") — the kit's compilerFamily picks
/// what it needs, so nothing is dropped or merged. Build tools prefer
/// mingw32-make over make; every tool that does not exist stays unset.
Toolchain toolchainFromBin(const QString& bin, const QString& id,
                           const QString& name)
{
    Toolchain out;
    out.id = id;
    out.name = name;
    const auto note = [&out](const char* key, const QString& path) {
        if (!path.isEmpty()) {
            out.compilers.insert(QLatin1String(key), path);
        }
    };
    note("gcc", binTool(bin, {"gcc", "gcc.exe"}));
    note("g++", binTool(bin, {"g++", "g++.exe"}));
    note("clang", binTool(bin, {"clang", "clang.exe"}));
    note("clang++", binTool(bin, {"clang++", "clang++.exe"}));
    note("gdb", binTool(bin, {"gdb", "gdb.exe"}));
    note("lldb", binTool(bin, {"lldb", "lldb.exe"}));
    out.clangd = binTool(bin, {"clangd", "clangd.exe"});
    out.clangFormat = binTool(bin, {"clang-format", "clang-format.exe"});
    out.buildTools = toolchainBuildTools(
        binTool(bin, {"cmake", "cmake.exe"}), binTool(bin, {"ninja", "ninja.exe"}),
        binTool(bin, {"mingw32-make.exe", "gmake", "make.exe", "make"}));
    return out;
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

/// A starter kit for one compiler family inside the toolchain: the kit
/// inherits every tool from the toolchain named by `toolchainId`, and its
/// compilerFamily picks gcc/g++, clang/clang++ — or nothing (else). Only
/// its flags, standard and configurations are its own.
Kit gnuKit(const QString& id, const QString& name, const QString& toolchainId,
           const QString& compilerFamily, bool isDefault,
           const QStringList& warningFlags, const QString& cppStandard)
{
    Kit kit;
    kit.id = id;
    kit.name = name;
    kit.enabled = true;
    kit.isDefault = isDefault;
    kit.inheritToolchain = true;
    kit.toolchainId = toolchainId;
    kit.compilerFamily = compilerFamily;
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

    // A toolchain shipped next to the IDE (MinGW64 / Clang64 / MinGW32 /
    // Clang32 beside the executable) is the only thing the starter
    // identifies — no other location is scanned. clangd and clang-format
    // come from that bin when it carries them.
    const QString portableBin = portableToolchainBin();
    const QString effectiveClangd = portableBin.isEmpty()
        ? QString() : binTool(portableBin, {"clangd", "clangd.exe"});
    const QString effectiveClangFormat = portableBin.isEmpty()
        ? QString() : binTool(portableBin, {"clang-format", "clang-format.exe"});
    // Same two steps as everywhere else, and the same guarantee: only a path
    // that exists comes back, so a starter file never points at a directory
    // this machine does not have.
    const QString git = LightCpp::App::ToolchainPaths::gitPath();

    // --- toolchains.json (an array of per-toolchain documents) --------------
    QList<Toolchain> toolchains;
    QList<Kit> kits;
    if (!portableBin.isEmpty()) {
        const QString dirName =
            QFileInfo(portableBin).dir().dirName();  // "MinGW64" etc.
        const QString lower = dirName.toLower();
        Toolchain primary = toolchainFromBin(portableBin, lower, dirName);
        if (primary.isValid()) {
            toolchains.append(primary);
            // One kit per compiler family the bin actually carries: the
            // toolchain records every compiler it found, the kits select
            // by compilerFamily — a "… Clang" kit compiles with clang
            // because the family says so, not because a path was copied.
            const bool hasGcc =
                !primary.compilers.value(QStringLiteral("g++")).isEmpty();
            const bool hasClang =
                !primary.compilers.value(QStringLiteral("clang++")).isEmpty();
            if (hasGcc) {
                kits.append(gnuKit(
                    lower + QStringLiteral("-gcc"),
                    dirName + QStringLiteral(" GCC"), primary.id,
                    QStringLiteral("gcc"), true,
                    {QStringLiteral("-Wall"), QStringLiteral("-Wextra"),
                     QStringLiteral("-fdiagnostics-color=always")},
                    QStringLiteral("gnu++20")));
            }
            if (hasClang) {
                kits.append(gnuKit(
                    lower + QStringLiteral("-clang"),
                    dirName + QStringLiteral(" Clang"), primary.id,
                    QStringLiteral("llvm"), !hasGcc,
                    {QStringLiteral("-Wall"), QStringLiteral("-Wextra"),
                     QStringLiteral("-fcolor-diagnostics")},
                    QStringLiteral("c++20")));
            }
        }
    }
    // MSVC lives outside the application directory and is deliberately not
    // identified here: the starter only ever names toolchains shipped next
    // to the executable.

    // --- defaults.json -------------------------------------------------------
    GlobalDefaults defaults;
    if (!kits.isEmpty()) {
        defaults.defaultKitId = kits.first().id;
    }
    defaults.defaultBuildConfiguration = QStringLiteral("Debug");
    defaults.gitPath = git;
    defaults.clangdPath = effectiveClangd;
    defaults.clangFormatPath = effectiveClangFormat;

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
