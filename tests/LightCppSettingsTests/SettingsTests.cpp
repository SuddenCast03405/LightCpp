// SettingsTests — the configuration model's merge and resolve rules.
//
// These are the parts where a wrong answer is silent: an overlay that
// clobbers the base instead of extending it, a kit that loses its flags in
// a merge, a default kit that resolves to nothing. All of that is exercised
// here against real JSON documents, including the shipped examples.

#include "LightCpp/Settings/SettingsJson.hpp"
#include "LightCpp/Settings/SettingsStore.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <optional>

using namespace LightCpp::Settings;

namespace {

/// The kit document from the settings spec (the MSYS2 GCC kit), verbatim
/// apart from whitespace. If this ever fails to parse, the shipped default
/// configs are broken too.
QJsonObject exampleGccKitJson()
{
    QJsonObject kit;
    kit.insert("type", "kit");
    kit.insert("id", "msys2-ucrt64-gcc");
    kit.insert("name", "MSYS2 UCRT64 GCC");
    kit.insert("enabled", true);
    kit.insert("isDefault", true);
    kit.insert("cCompiler", "E:/msys64/ucrt64/bin/gcc.exe");
    kit.insert("cppCompiler", "E:/msys64/ucrt64/bin/g++.exe");
    kit.insert("debugger", QJsonValue::Null);
    kit.insert("clangd", "E:/msys64/ucrt64/bin/clangd.exe");
    kit.insert("clangFormat", "E:/msys64/ucrt64/bin/clang-format.exe");
    QJsonObject tools;
    tools.insert("cmake", "E:/msys64/ucrt64/bin/cmake.exe");
    tools.insert("ninja", "E:/msys64/ucrt64/bin/ninja.exe");
    tools.insert("gnuMake", "E:/msys64/ucrt64/bin/mingw32-make.exe");
    tools.insert("msbuild", QJsonValue::Null);
    kit.insert("buildTools", tools);
    kit.insert("cppStandard", "gnu++20");
    kit.insert("compileFlags", QJsonArray{"-Wall", "-Wextra",
                                          "-fdiagnostics-color=always"});
    kit.insert("linkFlags", QJsonArray{"-fdiagnostics-color=always"});
    QJsonObject git;
    git.insert("path", "E:/msys64/ucrt64/bin/git.exe");
    git.insert("args", QJsonArray{});
    kit.insert("git", git);
    kit.insert("clangFormatStyle", "file");
    kit.insert("clangdFallbackStyle", "file");

    QJsonObject debug;
    debug.insert("name", "Debug");
    debug.insert("inheritFromKit", true);
    debug.insert("compileFlags", QJsonArray{"-g", "-O0"});
    debug.insert("linkFlags", QJsonArray{});
    debug.insert("cppStandard", "gnu++20");
    debug.insert("buildToolArgs",
                 QJsonObject{{"cmake", QJsonArray{"-DCMAKE_BUILD_TYPE=Debug"}},
                             {"ninja", QJsonArray{}},
                             {"gnuMake", QJsonArray{}},
                             {"msbuild", QJsonArray{}}});

    QJsonObject release;
    release.insert("name", "Release");
    release.insert("inheritFromKit", true);
    release.insert("compileFlags", QJsonArray{"-O2", "-DNDEBUG"});
    release.insert("linkFlags", QJsonArray{});
    release.insert("cppStandard", "gnu++20");
    release.insert("buildToolArgs",
                   QJsonObject{{"cmake", QJsonArray{"-DCMAKE_BUILD_TYPE=Release"}},
                               {"ninja", QJsonArray{}},
                               {"gnuMake", QJsonArray{}},
                               {"msbuild", QJsonArray{}}});

    kit.insert("buildConfigurations", QJsonArray{debug, release});
    return kit;
}

QJsonObject examplePreferencesJson()
{
    QJsonObject prefs;
    prefs.insert("type", "preferences");
    prefs.insert("id", "global");
    prefs.insert("fonts",
                 QJsonObject{
                     {"toolbar", QJsonObject{{"family", "Segoe UI"}, {"size", 9}}},
                     {"editor", QJsonObject{{"family", "Cascadia Mono"}, {"size", 11}}},
                     {"output", QJsonObject{{"family", "Cascadia Mono"}, {"size", 10}}}});
    prefs.insert("files",
                 QJsonObject{
                     {"encoding", QJsonObject{{"default", "UTF-8"},
                                              {"detectionPriority",
                                               QJsonArray{"UTF-8 BOM", "UTF-8",
                                                          "GB18030", "GBK"}}}},
                     {"lineEnding",
                      QJsonObject{{"default", "auto"},
                                  {"platformOverrides",
                                   QJsonObject{{"windows", "CRLF"},
                                               {"linux", "LF"},
                                               {"macos", "LF"}}}}}});
    prefs.insert("shortcuts",
                 QJsonObject{{"clangd", "Alt+G"}, {"clangFormat", QJsonValue::Null}});
    prefs.insert("performance",
                 QJsonObject{{"largeFile",
                              QJsonObject{{"syntaxHighlightDisabledAboveMB", 5},
                                          {"indexDisabledAboveMB", 20},
                                          {"parseDisabledAboveMB", 10}}}});
    prefs.insert("languageService",
                 QJsonObject{
                     {"clangd", QJsonObject{{"enabledByDefault", true},
                                            {"completion", true},
                                            {"diagnostics", true},
                                            {"indexing", true},
                                            {"fallbackStyle", "file"}}},
                     {"clangFormat", QJsonObject{{"enabled", true},
                                                 {"formatOnSave", false},
                                                 {"style", "file"}}}});
    prefs.insert("formatting",
                 QJsonObject{{"globalStyle", "file"},
                             {"defaultStyleContent", "BasedOnStyle: LLVM\n"}});
    return prefs;
}

QJsonObject exampleToolchainJson()
{
    QJsonObject toolchain;
    toolchain.insert("type", "toolchain");
    toolchain.insert("id", "msys2-ucrt64");
    toolchain.insert("name", "MSYS2 UCRT64");
    toolchain.insert("compilers",
                     QJsonObject{{"gcc", "E:/msys64/ucrt64/bin/gcc.exe"},
                                 {"g++", "E:/msys64/ucrt64/bin/g++.exe"},
                                 {"clang", QJsonValue::Null},
                                 {"clang++", QJsonValue::Null},
                                 {"cl", QJsonValue::Null},
                                 {"gdb", QJsonValue::Null},
                                 {"lldb", QJsonValue::Null}});
    toolchain.insert("clangd", "E:/msys64/ucrt64/bin/clangd.exe");
    toolchain.insert("clangFormat", "E:/msys64/ucrt64/bin/clang-format.exe");
    toolchain.insert("buildTools",
                     QJsonObject{{"cmake", "E:/msys64/ucrt64/bin/cmake.exe"},
                                 {"ninja", "E:/msys64/ucrt64/bin/ninja.exe"},
                                 {"gnuMake", "E:/msys64/ucrt64/bin/mingw32-make.exe"},
                                 {"msbuild", QJsonValue::Null}});
    return toolchain;
}

QJsonObject exampleDefaultsJson()
{
    QJsonObject defaults;
    defaults.insert("type", "defaults");
    defaults.insert("id", "global");
    defaults.insert("defaultKitId", "msys2-ucrt64-gcc");
    defaults.insert("defaultBuildConfiguration", "Debug");
    defaults.insert("gitPath", "E:/msys64/ucrt64/bin/git.exe");
    defaults.insert("clangdPath", "E:/msys64/ucrt64/bin/clangd.exe");
    defaults.insert("clangFormatPath", "E:/msys64/ucrt64/bin/clang-format.exe");
    return defaults;
}

/// Write a JSON document to a path inside `dir`, creating directories.
void writeJson(const QString& path, const QJsonObject& object)
{
    ASSERT_TRUE(SettingsStore::writeDocument(
        path, QJsonDocument(object)));
}

/// Write a JSON document holding an array of documents.
void writeJson(const QString& path, const QJsonArray& array)
{
    ASSERT_TRUE(SettingsStore::writeDocument(path, QJsonDocument(array)));
}

}  // namespace

// ---------------------------------------------------------------------------
// Deserialisation
// ---------------------------------------------------------------------------

TEST(SettingsJson, ExampleKitParsesFully)
{
    Kit kit;
    fromJson(exampleGccKitJson(), kit);

    EXPECT_EQ(kit.id, "msys2-ucrt64-gcc");
    EXPECT_EQ(kit.name, "MSYS2 UCRT64 GCC");
    EXPECT_TRUE(kit.enabled);
    EXPECT_TRUE(kit.isDefault);
    EXPECT_EQ(kit.cCompiler, "E:/msys64/ucrt64/bin/gcc.exe");
    EXPECT_EQ(kit.cppCompiler, "E:/msys64/ucrt64/bin/g++.exe");
    // "debugger": null must keep the default (empty), not become garbage.
    EXPECT_TRUE(kit.debugger.isEmpty());
    EXPECT_EQ(kit.clangd, "E:/msys64/ucrt64/bin/clangd.exe");
    EXPECT_EQ(kit.buildTools.value("cmake"), "E:/msys64/ucrt64/bin/cmake.exe");
    EXPECT_TRUE(kit.buildTools.value("msbuild").isEmpty());
    EXPECT_EQ(kit.cppStandard, "gnu++20");
    EXPECT_EQ(kit.compileFlags,
              (QStringList{"-Wall", "-Wextra",
                           "-fdiagnostics-color=always"}));
    EXPECT_EQ(kit.gitPath, "E:/msys64/ucrt64/bin/git.exe");
    ASSERT_EQ(kit.buildConfigurations.size(), 2);
    EXPECT_EQ(kit.buildConfigurations[0].name, "Debug");
    EXPECT_TRUE(kit.buildConfigurations[0].inheritFromKit);
    EXPECT_EQ(kit.buildConfigurations[0].compileFlags,
              (QStringList{"-g", "-O0"}));
    EXPECT_EQ(kit.buildConfigurations[1].compileFlags,
              (QStringList{"-O2", "-DNDEBUG"}));
    EXPECT_TRUE(kit.isValid());
}

TEST(SettingsJson, ExamplePreferencesParsesFully)
{
    Preferences prefs;
    fromJson(examplePreferencesJson(), prefs);

    EXPECT_TRUE(prefs.editorFont.isValid());
    EXPECT_EQ(prefs.editorFont.family, "Cascadia Mono");
    EXPECT_DOUBLE_EQ(prefs.editorFont.size, 11.0);
    EXPECT_EQ(prefs.defaultEncoding, "UTF-8");
    EXPECT_EQ(prefs.detectionPriority,
              (QStringList{"UTF-8 BOM", "UTF-8", "GB18030", "GBK"}));
    EXPECT_EQ(prefs.lineEndingDefault, "auto");
    EXPECT_EQ(prefs.shortcutClangd, "Alt+G");
    EXPECT_TRUE(prefs.shortcutClangFormat.isEmpty());
    EXPECT_EQ(prefs.largeFile.syntaxHighlightDisabledAboveMB, 5);
    EXPECT_EQ(prefs.largeFile.indexDisabledAboveMB, 20);
    EXPECT_EQ(prefs.largeFile.parseDisabledAboveMB, 10);
    EXPECT_TRUE(prefs.clangd.enabledByDefault);
    EXPECT_TRUE(prefs.clangd.completion);
    EXPECT_TRUE(prefs.clangd.diagnostics);
    EXPECT_TRUE(prefs.clangd.indexing);
    EXPECT_TRUE(prefs.clangFormat.enabled);
    EXPECT_FALSE(prefs.clangFormat.formatOnSave);
    EXPECT_EQ(prefs.globalStyle, "file");
}

TEST(SettingsJson, LongLineModeReadsAndRoundTrips)
{
    // Absent -> the "scroll" default. Present -> parsed. Anything else ->
    // "scroll" again, so a typo in the file cannot leave the editor in a mode
    // the UI cannot show.
    Preferences prefs;
    fromJson(examplePreferencesJson(), prefs);
    EXPECT_EQ(prefs.longLineMode, "scroll");

    QJsonObject wrap = examplePreferencesJson();
    QJsonObject files = wrap.value("files").toObject();
    files.insert("longLineMode", "wrap");
    wrap.insert("files", files);
    Preferences wrapping;
    fromJson(wrap, wrapping);
    EXPECT_EQ(wrapping.longLineMode, "wrap");

    QJsonObject bogus = examplePreferencesJson();
    QJsonObject badFiles = bogus.value("files").toObject();
    badFiles.insert("longLineMode", "sideways");
    bogus.insert("files", badFiles);
    Preferences fallback;
    fromJson(bogus, fallback);
    EXPECT_EQ(fallback.longLineMode, "scroll");

    // Round trip: "wrap" survives toJson -> fromJson.
    Preferences written;
    written.longLineMode = "wrap";
    Preferences back;
    fromJson(toJson(written), back);
    EXPECT_EQ(back.longLineMode, "wrap");

    // An empty value is written out as the default rather than "".
    Preferences empty;
    empty.longLineMode.clear();
    const QJsonObject emptyObj = toJson(empty);
    EXPECT_EQ(emptyObj.value("files").toObject().value("longLineMode").toString(),
              "scroll");
}

TEST(SettingsJson, EditorDisplayFlagsReadAndRoundTrip)
{
    // Absent -> both display flags default to off.
    Preferences prefs;
    fromJson(examplePreferencesJson(), prefs);
    EXPECT_FALSE(prefs.showWhitespaces);
    EXPECT_FALSE(prefs.showIndentGuides);

    // Present -> parsed.
    QJsonObject on = examplePreferencesJson();
    QJsonObject files = on.value("files").toObject();
    files.insert("showWhitespaces", true);
    files.insert("showIndentGuides", true);
    on.insert("files", files);
    Preferences shown;
    fromJson(on, shown);
    EXPECT_TRUE(shown.showWhitespaces);
    EXPECT_TRUE(shown.showIndentGuides);

    // Round trip: true survives toJson -> fromJson.
    Preferences written;
    written.showWhitespaces = true;
    Preferences back;
    fromJson(toJson(written), back);
    EXPECT_TRUE(back.showWhitespaces);
    EXPECT_FALSE(back.showIndentGuides);
}

TEST(SettingsJson, AutoSaveReadsAndRoundTrips)
{
    // Absent -> off with the default period.
    Preferences prefs;
    fromJson(examplePreferencesJson(), prefs);
    EXPECT_FALSE(prefs.autoSave);
    EXPECT_EQ(prefs.autoSaveSeconds, 30);

    // Present -> parsed, and an insane period is clamped into 5..3600.
    QJsonObject on = examplePreferencesJson();
    QJsonObject general = on.value("general").toObject();
    general.insert("autoSave", true);
    general.insert("autoSaveSeconds", 99999);
    on.insert("general", general);
    Preferences enabled;
    fromJson(on, enabled);
    EXPECT_TRUE(enabled.autoSave);
    EXPECT_EQ(enabled.autoSaveSeconds, 3600);

    // Round trip.
    Preferences written;
    written.autoSave = true;
    written.autoSaveSeconds = 45;
    Preferences back;
    fromJson(toJson(written), back);
    EXPECT_TRUE(back.autoSave);
    EXPECT_EQ(back.autoSaveSeconds, 45);
}

TEST(SettingsJson, ExampleToolchainAndDefaultsParse)
{
    Toolchain toolchain;
    fromJson(exampleToolchainJson(), toolchain);
    EXPECT_EQ(toolchain.id, "msys2-ucrt64");
    EXPECT_EQ(toolchain.name, "MSYS2 UCRT64");
    EXPECT_EQ(toolchain.compilers.value("gcc"),
              "E:/msys64/ucrt64/bin/gcc.exe");
    EXPECT_EQ(toolchain.compilers.value("g++"),
              "E:/msys64/ucrt64/bin/g++.exe");
    // "clang": null must keep the key absent, not become garbage.
    EXPECT_TRUE(toolchain.compilers.value("clang").isEmpty());
    EXPECT_TRUE(toolchain.compilers.value("cl").isEmpty());
    EXPECT_EQ(toolchain.buildTools.value("gnuMake"),
              "E:/msys64/ucrt64/bin/mingw32-make.exe");
    EXPECT_TRUE(toolchain.isValid());

    GlobalDefaults defaults;
    fromJson(exampleDefaultsJson(), defaults);
    EXPECT_EQ(defaults.defaultKitId, "msys2-ucrt64-gcc");
    EXPECT_EQ(defaults.defaultBuildConfiguration, "Debug");
    EXPECT_EQ(defaults.clangdPath, "E:/msys64/ucrt64/bin/clangd.exe");
}

TEST(SettingsJson, CheckCompilersAtStartupParses)
{
    // Absent -> the default (on). Present -> parsed; wrong type -> default.
    Preferences prefs;
    fromJson(examplePreferencesJson(), prefs);
    EXPECT_TRUE(prefs.checkCompilersAtStartup);

    QJsonObject withFlag = examplePreferencesJson();
    withFlag.insert("general",
                    QJsonObject{{"checkCompilersAtStartup", false}});
    Preferences off;
    fromJson(withFlag, off);
    EXPECT_FALSE(off.checkCompilersAtStartup);

    QJsonObject broken = examplePreferencesJson();
    broken.insert("general", QJsonObject{{"checkCompilersAtStartup", "no"}});
    Preferences kept;
    fromJson(broken, kept);
    EXPECT_TRUE(kept.checkCompilersAtStartup);
}

TEST(SettingsJson, KitToolchainInheritanceRoundTrips)
{
    QJsonObject kitJson = exampleGccKitJson();
    kitJson.insert("inheritToolchain", true);
    kitJson.insert("toolchainId", "msys2-ucrt64");
    kitJson.insert("compilerFamily", "gcc");
    kitJson.insert("cCompiler", "");
    kitJson.insert("cppCompiler", "");

    Kit kit;
    fromJson(kitJson, kit);
    EXPECT_TRUE(kit.inheritToolchain);
    EXPECT_EQ(kit.toolchainId, "msys2-ucrt64");
    EXPECT_EQ(kit.compilerFamily, "gcc");

    const Kit parsed = [&] {
        Kit k;
        fromJson(toJson(kit), k);
        return k;
    }();
    EXPECT_EQ(parsed.inheritToolchain, kit.inheritToolchain);
    EXPECT_EQ(parsed.toolchainId, kit.toolchainId);
    EXPECT_EQ(parsed.compilerFamily, kit.compilerFamily);
    EXPECT_TRUE(parsed.cCompiler.isEmpty());
    EXPECT_TRUE(parsed.cppCompiler.isEmpty());
}

TEST(SettingsJson, ToolchainCompilersRoundTrip)
{
    Toolchain toolchain;
    fromJson(exampleToolchainJson(), toolchain);
    const Toolchain parsed = [&] {
        Toolchain t;
        fromJson(toJson(toolchain), t);
        return t;
    }();
    EXPECT_EQ(parsed.compilers.value("gcc"),
              "E:/msys64/ucrt64/bin/gcc.exe");
    EXPECT_EQ(parsed.compilers.value("g++"),
              "E:/msys64/ucrt64/bin/g++.exe");
    EXPECT_TRUE(parsed.compilers.value("clang").isEmpty());
    EXPECT_TRUE(parsed.compilers.value("clang++").isEmpty());
}

TEST(SettingsJson, LegacyToolchainFieldsMapOntoCompilers)
{
    // A pre-compilers document: the old single-compiler fields land in the
    // compilers map under the key their file name implies.
    QJsonObject legacy = exampleToolchainJson();
    legacy.remove("compilers");
    legacy.insert("cCompiler", "E:/msys64/ucrt64/bin/gcc.exe");
    legacy.insert("cppCompiler", "E:/msys64/ucrt64/bin/g++.exe");
    legacy.insert("debugger", "E:/msys64/ucrt64/bin/gdb.exe");

    Toolchain toolchain;
    fromJson(legacy, toolchain);
    EXPECT_EQ(toolchain.compilers.value("gcc"),
              "E:/msys64/ucrt64/bin/gcc.exe");
    EXPECT_EQ(toolchain.compilers.value("g++"),
              "E:/msys64/ucrt64/bin/g++.exe");
    EXPECT_EQ(toolchain.compilers.value("gdb"),
              "E:/msys64/ucrt64/bin/gdb.exe");
    EXPECT_TRUE(toolchain.isValid());
}

TEST(SettingsJson, LegacyClangToolchainFieldsMapByFileName)
{
    QJsonObject legacy;
    legacy.insert("type", "toolchain");
    legacy.insert("id", "clang64");
    legacy.insert("cCompiler", "E:/msys64/clang64/bin/clang.exe");
    legacy.insert("cppCompiler", "E:/msys64/clang64/bin/clang++.exe");

    Toolchain toolchain;
    fromJson(legacy, toolchain);
    EXPECT_EQ(toolchain.compilers.value("clang"),
              "E:/msys64/clang64/bin/clang.exe");
    EXPECT_EQ(toolchain.compilers.value("clang++"),
              "E:/msys64/clang64/bin/clang++.exe");
    EXPECT_TRUE(toolchain.compilers.value("gcc").isEmpty());
}

TEST(SettingsJson, WrongTypesKeepDefaults)
{
    QJsonObject broken;
    broken.insert("type", "kit");
    broken.insert("id", "x");
    broken.insert("enabled", "yes");           // string where bool belongs
    broken.insert("compileFlags", "-Wall");    // string where array belongs
    broken.insert("buildConfigurations", 42);  // number where array belongs
    broken.insert("cppStandard", 20);          // number where string belongs

    Kit kit;
    kit.enabled = false;
    kit.compileFlags = {"-kept"};
    fromJson(broken, kit);

    EXPECT_EQ(kit.id, "x");  // correct type still parsed
    EXPECT_TRUE(kit.enabled == false);  // wrong type -> untouched
    EXPECT_EQ(kit.compileFlags, QStringList{"-kept"});
    EXPECT_TRUE(kit.buildConfigurations.isEmpty());
}

TEST(SettingsJson, RunOutputParsesAndRoundTrips)
{
    // Absent -> the default ("program": captured in the IDE).
    Preferences fresh;
    fromJson(QJsonObject{{"type", "preferences"}}, fresh);
    EXPECT_EQ(fresh.programOutput, "program");

    // Present -> parsed, and survives a write/read round trip.
    QJsonObject pauser = examplePreferencesJson();
    pauser.insert("run", QJsonObject{{"output", "consolePauser"}});
    Preferences prefs;
    fromJson(pauser, prefs);
    EXPECT_EQ(prefs.programOutput, "consolePauser");

    const Preferences roundTripped = [&] {
        Preferences p;
        fromJson(toJson(prefs), p);
        return p;
    }();
    EXPECT_EQ(roundTripped.programOutput, "consolePauser");
}

TEST(SettingsJson, RunTerminalsParseAndRoundTrip)
{
    // Absent -> the two default Windows terminals are seeded.
    Preferences fresh;
    fromJson(QJsonObject{{"type", "preferences"}}, fresh);
    ASSERT_EQ(fresh.terminals.size(), 2);
    EXPECT_EQ(fresh.terminals[0].id, "cmd");
    EXPECT_EQ(fresh.terminals[0].name,
              QString::fromUtf8("Windows Command Prompt"));
    EXPECT_TRUE(fresh.terminals[0].embedded);
    EXPECT_EQ(fresh.terminals[1].id, "powershell");
    EXPECT_TRUE(fresh.terminals[1].embedded);

    // Present -> the list replaces the defaults verbatim (an empty array
    // stays empty: the user deleted every terminal on purpose).
    const QJsonArray terminalArray{
        QJsonObject{{"id", "myterm"},
                     {"name", "My Terminal"},
                     {"command", "cmd.exe /k"},
                     {"embedded", false}},
    };
    Preferences prefs;
    fromJson(QJsonObject{{"type", "preferences"},
                          {"run", QJsonObject{{"output", "myterm"},
                                               {"terminals", terminalArray}}}},
             prefs);
    ASSERT_EQ(prefs.terminals.size(), 1);
    EXPECT_EQ(prefs.terminals[0].id, "myterm");
    EXPECT_EQ(prefs.terminals[0].name, "My Terminal");
    EXPECT_EQ(prefs.terminals[0].command, "cmd.exe /k");
    EXPECT_FALSE(prefs.terminals[0].embedded);

    const Preferences roundTripped = [&] {
        Preferences p;
        fromJson(toJson(prefs), p);
        return p;
    }();
    ASSERT_EQ(roundTripped.terminals.size(), 1);
    EXPECT_EQ(roundTripped.terminals[0].id, "myterm");
    EXPECT_EQ(roundTripped.terminals[0].name, "My Terminal");
    EXPECT_EQ(roundTripped.terminals[0].command, "cmd.exe /k");
    EXPECT_FALSE(roundTripped.terminals[0].embedded);
}

TEST(SettingsJson, PluginDisableListsParseAndRoundTrip)
{
    // Absent -> empty lists (nothing disabled, nothing pending).
    Preferences fresh;
    fromJson(QJsonObject{{"type", "preferences"}}, fresh);
    EXPECT_TRUE(fresh.disabledPlugins.isEmpty());
    EXPECT_TRUE(fresh.pendingPluginDeletes.isEmpty());

    // Present -> parsed, and survives a write/read round trip.
    Preferences prefs;
    fromJson(QJsonObject{
                 {"type", "preferences"},
                 {"plugins", QJsonObject{
                                 {"disabled", QJsonArray{"hello.dll",
                                                         "lua/timer.lua"}},
                                 {"pendingDeletes", QJsonArray{"old.dll"}}}}},
             prefs);
    ASSERT_EQ(prefs.disabledPlugins.size(), 2);
    EXPECT_EQ(prefs.disabledPlugins[0], "hello.dll");
    EXPECT_EQ(prefs.disabledPlugins[1], "lua/timer.lua");
    ASSERT_EQ(prefs.pendingPluginDeletes.size(), 1);
    EXPECT_EQ(prefs.pendingPluginDeletes[0], "old.dll");

    const Preferences roundTripped = [&] {
        Preferences p;
        fromJson(toJson(prefs), p);
        return p;
    }();
    EXPECT_EQ(roundTripped.disabledPlugins, prefs.disabledPlugins);
    EXPECT_EQ(roundTripped.pendingPluginDeletes, prefs.pendingPluginDeletes);

    // Wrong type -> untouched defaults.
    Preferences guarded;
    fromJson(QJsonObject{{"type", "preferences"},
                          {"plugins", QJsonObject{{"disabled", 42}}}},
             guarded);
    EXPECT_TRUE(guarded.disabledPlugins.isEmpty());
}

TEST(SettingsJson, StartupPagePrefsParseAndRoundTrip)
{
    // Absent -> defaults: empty recents, page on, checks on.
    Preferences fresh;
    fromJson(QJsonObject{{"type", "preferences"}}, fresh);
    EXPECT_TRUE(fresh.recentFiles.isEmpty());
    EXPECT_TRUE(fresh.showStartupPage);
    EXPECT_TRUE(fresh.checkToolchainsAtStartup);

    // Present -> parsed, and survives a write/read round trip.
    Preferences prefs;
    fromJson(QJsonObject{
                 {"type", "preferences"},
                 {"general", QJsonObject{
                                 {"showStartupPage", false},
                                 {"checkToolchainsAtStartup", false},
                                 {"recentFiles",
                                  QJsonArray{"E:/demo/main.cpp",
                                             "E:/demo/CMakeLists.txt"}}}}},
             prefs);
    ASSERT_EQ(prefs.recentFiles.size(), 2);
    EXPECT_EQ(prefs.recentFiles[0], "E:/demo/main.cpp");
    EXPECT_EQ(prefs.recentFiles[1], "E:/demo/CMakeLists.txt");
    EXPECT_FALSE(prefs.showStartupPage);
    EXPECT_FALSE(prefs.checkToolchainsAtStartup);

    const Preferences roundTripped = [&] {
        Preferences p;
        fromJson(toJson(prefs), p);
        return p;
    }();
    EXPECT_EQ(roundTripped.recentFiles, prefs.recentFiles);
    EXPECT_EQ(roundTripped.showStartupPage, prefs.showStartupPage);
    EXPECT_EQ(roundTripped.checkToolchainsAtStartup,
              prefs.checkToolchainsAtStartup);

    // Wrong type -> untouched defaults.
    Preferences guarded;
    fromJson(QJsonObject{{"type", "preferences"},
                          {"general", QJsonObject{{"recentFiles", 42}}}},
             guarded);
    EXPECT_TRUE(guarded.recentFiles.isEmpty());
    EXPECT_TRUE(guarded.showStartupPage);
}

// ---------------------------------------------------------------------------
// Store: layers, merging, resolution
// ---------------------------------------------------------------------------

TEST(SettingsStore, MissingDirectoriesAreNotAnError)
{
    SettingsStore store;
    store.setBaseDirectory(QStringLiteral("Z:/definitely-not-here/config"));
    store.setOverlayDirectories({QStringLiteral("Z:/also-not-here/config")});
    EXPECT_TRUE(store.reload());
    EXPECT_TRUE(store.lastErrors().isEmpty());
    EXPECT_FALSE(store.defaultKit().has_value());
    EXPECT_EQ(store.clangdPath(), QString());  // no fallback set -> empty
}

TEST(SettingsStore, KitArrayFileLoadsInOrder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QJsonArray kits;
    kits.append(exampleGccKitJson());
    QJsonObject clangKit = exampleGccKitJson();
    clangKit.insert("id", "msys2-ucrt64-clang");
    clangKit.insert("isDefault", false);
    kits.append(clangKit);
    ASSERT_TRUE(SettingsStore::writeDocument(
        QDir(dir.path()).filePath("kits.json"), QJsonDocument(kits)));

    SettingsStore store;
    store.setBaseDirectory(dir.path());
    EXPECT_TRUE(store.reload());
    ASSERT_EQ(store.kits().size(), 2);
    EXPECT_EQ(store.kits()[0].id, "msys2-ucrt64-gcc");
    EXPECT_EQ(store.kits()[1].id, "msys2-ucrt64-clang");
    ASSERT_TRUE(store.defaultKit().has_value());
    EXPECT_EQ(store.defaultKit()->id, "msys2-ucrt64-gcc");
}

TEST(SettingsStore, OverlayMergesSameKitFieldWise)
{
    QTemporaryDir base;
    QTemporaryDir overlay;
    ASSERT_TRUE(base.isValid());
    ASSERT_TRUE(overlay.isValid());

    writeJson(QDir(base.path()).filePath("kits.json"), exampleGccKitJson());

    // The overlay only touches two fields of the same kit; everything else
    // (name, compilers, configurations...) must survive the merge.
    QJsonObject overlayKit;
    overlayKit.insert("type", "kit");
    overlayKit.insert("id", "msys2-ucrt64-gcc");
    overlayKit.insert("cppStandard", "c++23");
    overlayKit.insert("compileFlags", QJsonArray{"-Wall", "-Wpedantic"});
    writeJson(QDir(overlay.path()).filePath("kit-override.json"), overlayKit);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    store.setOverlayDirectories({overlay.path()});
    EXPECT_TRUE(store.reload());

    const std::optional<Kit> kit = store.kit("msys2-ucrt64-gcc");
    ASSERT_TRUE(kit.has_value());
    EXPECT_EQ(kit->cppStandard, "c++23");                  // overlay wins
    EXPECT_EQ(kit->compileFlags, (QStringList{"-Wall", "-Wpedantic"}));
    EXPECT_EQ(kit->name, "MSYS2 UCRT64 GCC");              // base survives
    EXPECT_EQ(kit->cppCompiler, "E:/msys64/ucrt64/bin/g++.exe");
    ASSERT_EQ(kit->buildConfigurations.size(), 2);         // base survives
    EXPECT_EQ(kit->buildConfigurations[0].compileFlags,
              (QStringList{"-g", "-O0"}));
}

TEST(SettingsStore, OverlayCanAddANewKitAndDefaults)
{
    QTemporaryDir base;
    QTemporaryDir overlay;
    ASSERT_TRUE(base.isValid());
    ASSERT_TRUE(overlay.isValid());

    writeJson(QDir(base.path()).filePath("defaults.json"),
              exampleDefaultsJson());
    writeJson(QDir(base.path()).filePath("kits.json"), exampleGccKitJson());

    QJsonObject extraKit;
    extraKit.insert("type", "kit");
    extraKit.insert("id", "cross-arm-gcc");
    extraKit.insert("name", "ARM GCC");
    extraKit.insert("cCompiler", "D:/arm/bin/arm-none-eabi-gcc.exe");
    extraKit.insert("cppCompiler", "D:/arm/bin/arm-none-eabi-g++.exe");
    writeJson(QDir(overlay.path()).filePath("kits.json"), extraKit);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    store.setOverlayDirectories({overlay.path()});
    EXPECT_TRUE(store.reload());

    EXPECT_EQ(store.kits().size(), 2);
    // Document order: base kits first, overlay kits after.
    EXPECT_EQ(store.kits()[1].id, "cross-arm-gcc");
    // The default is still the one the settings name.
    ASSERT_TRUE(store.defaultKit().has_value());
    EXPECT_EQ(store.defaultKit()->id, "msys2-ucrt64-gcc");
}

TEST(SettingsStore, DefaultKitFallsBackWhenNamedKitIsDisabled)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());

    QJsonObject kit = exampleGccKitJson();
    kit.insert("enabled", false);
    writeJson(QDir(base.path()).filePath("kits.json"), kit);

    QJsonObject other = exampleGccKitJson();
    other.insert("id", "backup-kit");
    other.insert("isDefault", true);
    writeJson(QDir(base.path()).filePath("kits2.json"), other);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());

    // defaults.json names the disabled kit; the enabled isDefault kit wins.
    const std::optional<Kit> chosen = store.defaultKit();
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->id, "backup-kit");
}

TEST(SettingsStore, InvalidKitIsNeverChosenAsDefault)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());

    // No compilers named: valid() is false, so this must not be chosen.
    QJsonObject broken = exampleGccKitJson();
    broken.insert("cCompiler", "");
    broken.insert("cppCompiler", "");
    writeJson(QDir(base.path()).filePath("kits.json"), broken);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
    EXPECT_FALSE(store.defaultKit().has_value());
}

TEST(SettingsStore, EffectiveBuildConfigInheritsKitFlags)
{
    const Kit kit = [] {
        Kit k;
        fromJson(exampleGccKitJson(), k);
        return k;
    }();
    SettingsStore store;

    const EffectiveBuildConfig debug = store.effectiveBuildConfig(kit, "Debug");
    // Kit flags first, then the configuration's own.
    EXPECT_EQ(debug.compileFlags,
              (QStringList{"-Wall", "-Wextra", "-fdiagnostics-color=always",
                           "-g", "-O0"}));
    EXPECT_EQ(debug.linkFlags,
              QStringList{"-fdiagnostics-color=always"});
    EXPECT_EQ(debug.cppStandard, "gnu++20");
    EXPECT_EQ(debug.buildToolArgs.value("cmake"),
              QStringList{"-DCMAKE_BUILD_TYPE=Debug"});

    // An unknown configuration name falls back to the kit's own flags.
    const EffectiveBuildConfig custom =
        store.effectiveBuildConfig(kit, "Profile");
    EXPECT_EQ(custom.compileFlags,
              (QStringList{"-Wall", "-Wextra", "-fdiagnostics-color=always"}));
    EXPECT_EQ(custom.cppStandard, "gnu++20");
}

TEST(SettingsStore, EffectiveBuildConfigWithoutInheritanceReplaces)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());

    QJsonObject kitJson = exampleGccKitJson();
    // Turn inheritance off on Debug: its flags replace the kit's.
    QJsonObject debug = kitJson.value("buildConfigurations").toArray()[0].toObject();
    debug.insert("inheritFromKit", false);
    kitJson.insert("buildConfigurations", QJsonArray{debug});
    writeJson(QDir(base.path()).filePath("kits.json"), kitJson);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    ASSERT_TRUE(store.reload());
    const std::optional<Kit> kit = store.kit("msys2-ucrt64-gcc");
    ASSERT_TRUE(kit.has_value());

    const EffectiveBuildConfig debugConfig =
        store.effectiveBuildConfig(*kit, "Debug");
    EXPECT_EQ(debugConfig.compileFlags, (QStringList{"-g", "-O0"}));
    EXPECT_TRUE(debugConfig.linkFlags.isEmpty());
}

TEST(SettingsStore, BuildConfigurationNamesDefaultToDebugRelease)
{
    Kit kit;
    fromJson(exampleGccKitJson(), kit);
    SettingsStore store;
    EXPECT_EQ(store.buildConfigurationNames(kit),
              (QStringList{"Debug", "Release"}));

    Kit bare;
    bare.id = "bare";
    EXPECT_EQ(store.buildConfigurationNames(bare),
              (QStringList{"Debug", "Release"}));

    Kit custom;
    custom.buildConfigurations.append(KitBuildConfiguration{QStringLiteral("Profile"), true, {}, {}, {}, {}});
    custom.buildConfigurations.append(KitBuildConfiguration{QStringLiteral("MinSize"), true, {}, {}, {}, {}});
    EXPECT_EQ(store.buildConfigurationNames(custom),
              (QStringList{"Profile", "MinSize"}));
}

TEST(SettingsStore, ToolPathsPreferSettingsOverFallbacks)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());
    writeJson(QDir(base.path()).filePath("defaults.json"),
              exampleDefaultsJson());

    SettingsStore store;
    store.setBaseDirectory(base.path());
    store.setFallbackToolPaths("fallback/clangd.exe", "fallback/cf.exe",
                               "fallback/git.exe");
    EXPECT_TRUE(store.reload());
    EXPECT_EQ(store.clangdPath(), "E:/msys64/ucrt64/bin/clangd.exe");
    EXPECT_EQ(store.gitPath(), "E:/msys64/ucrt64/bin/git.exe");

    // Without the settings document, the compiled-in fallbacks apply.
    SettingsStore empty;
    empty.setBaseDirectory(QStringLiteral("Z:/nothing/config"));
    empty.setFallbackToolPaths("fallback/clangd.exe", "fallback/cf.exe",
                               "fallback/git.exe");
    EXPECT_TRUE(empty.reload());
    EXPECT_EQ(empty.clangdPath(), "fallback/clangd.exe");
    EXPECT_EQ(empty.clangFormatPath(), "fallback/cf.exe");
    EXPECT_EQ(empty.gitPath(), "fallback/git.exe");
}

TEST(SettingsStore, LineEndingResolution)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());

    // auto (the default in the example) -> the platform's own convention.
    writeJson(QDir(base.path()).filePath("preferences.json"),
              examplePreferencesJson());
    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
#ifdef Q_OS_WIN
    EXPECT_EQ(store.lineEndingForSaving(), "CRLF");
#else
    EXPECT_EQ(store.lineEndingForSaving(), "LF");
#endif

    // An explicit LF always wins.
    QJsonObject prefs = examplePreferencesJson();
    QJsonObject files = prefs.value("files").toObject();
    files.insert("lineEnding", QJsonObject{{"default", "LF"}});
    prefs.insert("files", files);
    writeJson(QDir(base.path()).filePath("preferences.json"), prefs);
    ASSERT_TRUE(store.reload());
    EXPECT_EQ(store.lineEndingForSaving(), "LF");
}

TEST(SettingsStore, KitInheritsToolchainFields)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());

    QJsonArray toolchains;
    toolchains.append(exampleToolchainJson());
    writeJson(QDir(base.path()).filePath("toolchains.json"),
              toolchains);

    // The kit names no tools of its own; its compilerFamily maps the
    // toolchain's gcc/g++ onto the C/C++ slots.
    QJsonObject kitJson = exampleGccKitJson();
    kitJson.insert("inheritToolchain", true);
    kitJson.insert("toolchainId", "msys2-ucrt64");
    kitJson.insert("compilerFamily", "gcc");
    kitJson.insert("cCompiler", "");
    kitJson.insert("cppCompiler", "");
    kitJson.insert("clangd", "");
    kitJson.remove("buildTools");
    writeJson(QDir(base.path()).filePath("kits.json"), kitJson);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
    const std::optional<Kit> kit = store.kit("msys2-ucrt64-gcc");
    ASSERT_TRUE(kit.has_value());
    EXPECT_EQ(kit->cCompiler, "E:/msys64/ucrt64/bin/gcc.exe");
    EXPECT_EQ(kit->cppCompiler, "E:/msys64/ucrt64/bin/g++.exe");
    EXPECT_EQ(kit->clangd, "E:/msys64/ucrt64/bin/clangd.exe");
    EXPECT_EQ(kit->buildTools.value("cmake"),
              "E:/msys64/ucrt64/bin/cmake.exe");
    EXPECT_TRUE(kit->isValid());

    // The kit's own values win over the toolchain's: name one explicitly.
    QJsonObject overrideJson = kitJson;
    overrideJson.insert("cppCompiler", "D:/custom/g++.exe");
    writeJson(QDir(base.path()).filePath("kits.json"), overrideJson);
    ASSERT_TRUE(store.reload());
    const std::optional<Kit> overridden = store.kit("msys2-ucrt64-gcc");
    ASSERT_TRUE(overridden.has_value());
    EXPECT_EQ(overridden->cppCompiler, "D:/custom/g++.exe");
    EXPECT_EQ(overridden->cCompiler, "E:/msys64/ucrt64/bin/gcc.exe");
}

TEST(SettingsStore, LlvmFamilyInheritsClangFromTheSameToolchain)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());

    // One toolchain carrying both families: gcc/g++ plus clang/clang++.
    QJsonObject compilers;
    compilers.insert("gcc", "E:/msys64/ucrt64/bin/gcc.exe");
    compilers.insert("g++", "E:/msys64/ucrt64/bin/g++.exe");
    compilers.insert("clang", "E:/msys64/ucrt64/bin/clang.exe");
    compilers.insert("clang++", "E:/msys64/ucrt64/bin/clang++.exe");
    QJsonObject bothToolchain = exampleToolchainJson();
    bothToolchain.insert("compilers", compilers);
    QJsonArray toolchains;
    toolchains.append(bothToolchain);
    writeJson(QDir(base.path()).filePath("toolchains.json"), toolchains);

    QJsonObject kitJson = exampleGccKitJson();
    kitJson.insert("id", "mixed-clang");
    kitJson.insert("inheritToolchain", true);
    kitJson.insert("toolchainId", "msys2-ucrt64");
    kitJson.insert("compilerFamily", "llvm");
    kitJson.insert("cCompiler", "");
    kitJson.insert("cppCompiler", "");
    writeJson(QDir(base.path()).filePath("kits.json"), kitJson);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
    const std::optional<Kit> kit = store.kit("mixed-clang");
    ASSERT_TRUE(kit.has_value());
    // A "… Clang" kit compiles with clang — the family says so, even
    // though the toolchain also carries gcc.
    EXPECT_EQ(kit->cCompiler, "E:/msys64/ucrt64/bin/clang.exe");
    EXPECT_EQ(kit->cppCompiler, "E:/msys64/ucrt64/bin/clang++.exe");
}

TEST(SettingsStore, ElseFamilyInheritsNoCompiler)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());

    QJsonArray toolchains;
    toolchains.append(exampleToolchainJson());
    writeJson(QDir(base.path()).filePath("toolchains.json"), toolchains);

    // No compilerFamily at all: "else" — nothing is inherited, the kit
    // must name its own compilers (it does not here, so it stays invalid).
    QJsonObject kitJson = exampleGccKitJson();
    kitJson.insert("inheritToolchain", true);
    kitJson.insert("toolchainId", "msys2-ucrt64");
    kitJson.insert("cCompiler", "");
    kitJson.insert("cppCompiler", "");
    writeJson(QDir(base.path()).filePath("kits.json"), kitJson);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
    const std::optional<Kit> kit = store.kit("msys2-ucrt64-gcc");
    ASSERT_TRUE(kit.has_value());
    EXPECT_TRUE(kit->cCompiler.isEmpty());
    EXPECT_TRUE(kit->cppCompiler.isEmpty());
    EXPECT_FALSE(kit->isValid());
}

TEST(SettingsStore, MsvcFamilyInheritsClBothWays)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());

    QJsonObject msvcToolchain = exampleToolchainJson();
    msvcToolchain.insert("id", "msvc");
    QJsonObject compilers;
    compilers.insert("cl", "D:/VS/bin/cl.exe");
    compilers.insert("cdb", "D:/VS/bin/cdb.exe");
    msvcToolchain.insert("compilers", compilers);
    QJsonArray toolchains;
    toolchains.append(msvcToolchain);
    writeJson(QDir(base.path()).filePath("toolchains.json"), toolchains);

    QJsonObject kitJson = exampleGccKitJson();
    kitJson.insert("id", "msvc-kit");
    kitJson.insert("inheritToolchain", true);
    kitJson.insert("toolchainId", "msvc");
    kitJson.insert("compilerFamily", "msvc");
    kitJson.insert("cCompiler", "");
    kitJson.insert("cppCompiler", "");
    writeJson(QDir(base.path()).filePath("kits.json"), kitJson);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
    const std::optional<Kit> kit = store.kit("msvc-kit");
    ASSERT_TRUE(kit.has_value());
    // cl compiles both C and C++; the debugger comes from cdb.
    EXPECT_EQ(kit->cCompiler, "D:/VS/bin/cl.exe");
    EXPECT_EQ(kit->cppCompiler, "D:/VS/bin/cl.exe");
    EXPECT_EQ(kit->debugger, "D:/VS/bin/cdb.exe");
}

TEST(SettingsStore, InheritingKitWithUnknownToolchainIsNotUsable)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());

    // toolchains.json is absent: the reference cannot resolve, the kit has
    // no compilers of its own and must not be chosen as the default.
    QJsonObject kitJson = exampleGccKitJson();
    kitJson.insert("inheritToolchain", true);
    kitJson.insert("toolchainId", "nowhere");
    kitJson.insert("cCompiler", "");
    kitJson.insert("cppCompiler", "");
    writeJson(QDir(base.path()).filePath("kits.json"), kitJson);

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
    EXPECT_FALSE(store.defaultKit().has_value());
}

TEST(SettingsStore, LegacyToolchainsDocumentIsAcceptedSilently)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());
    writeJson(QDir(base.path()).filePath("defaults.json"),
              exampleDefaultsJson());

    // The pre-2.0 registry format: type "toolchains" with path lists. It is
    // no longer consumed, but an old config directory must not turn into an
    // error report.
    QJsonObject legacy;
    legacy.insert("type", "toolchains");
    legacy.insert("id", "global");
    legacy.insert("cCompilers", QJsonArray{"E:/msys64/ucrt64/bin/gcc.exe"});
    writeJson(QDir(base.path()).filePath("toolchains.json"),
              QJsonArray{legacy});

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
    EXPECT_EQ(store.defaults().defaultKitId, "msys2-ucrt64-gcc");
    EXPECT_TRUE(store.lastErrors().isEmpty());
    EXPECT_TRUE(store.toolchains().isEmpty());
}

TEST(SettingsStore, ToolchainOverlayMergesFieldWise)
{
    QTemporaryDir base;
    QTemporaryDir overlay;
    ASSERT_TRUE(base.isValid());
    ASSERT_TRUE(overlay.isValid());

    QJsonArray toolchains;
    toolchains.append(exampleToolchainJson());
    writeJson(QDir(base.path()).filePath("toolchains.json"),
              toolchains);

    // The overlay renames the same toolchain; compilers survive the merge.
    QJsonObject overlayToolchain;
    overlayToolchain.insert("type", "toolchain");
    overlayToolchain.insert("id", "msys2-ucrt64");
    overlayToolchain.insert("name", "MSYS2 UCRT64 (patched)");
    writeJson(QDir(overlay.path()).filePath("toolchains.json"),
              QJsonArray{overlayToolchain});

    SettingsStore store;
    store.setBaseDirectory(base.path());
    store.setOverlayDirectories({overlay.path()});
    EXPECT_TRUE(store.reload());
    ASSERT_EQ(store.toolchains().size(), 1);
    EXPECT_EQ(store.toolchains()[0].name, "MSYS2 UCRT64 (patched)");
    EXPECT_EQ(store.toolchains()[0].compilers.value("gcc"),
              "E:/msys64/ucrt64/bin/gcc.exe");
}

TEST(SettingsStore, BrokenFileIsReportedAndSkipped)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());
    writeJson(QDir(base.path()).filePath("defaults.json"),
              exampleDefaultsJson());

    {
        QFile garbage(QDir(base.path()).filePath("broken.json"));
        ASSERT_TRUE(garbage.open(QIODevice::WriteOnly));
        garbage.write("{ this is not json");
    }

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
    // The good file still loaded...
    EXPECT_EQ(store.defaults().defaultKitId, "msys2-ucrt64-gcc");
    // ...and the bad one is named in the errors.
    ASSERT_FALSE(store.lastErrors().isEmpty());
    EXPECT_TRUE(store.lastErrors().join(QLatin1Char('\n'))
                    .contains("broken.json"));
}

TEST(SettingsStore, UnknownDocumentTypeIsIgnoredWithAHint)
{
    QTemporaryDir base;
    ASSERT_TRUE(base.isValid());
    QJsonObject odd;
    odd.insert("type", "mystery");
    odd.insert("id", "x");
    writeJson(QDir(base.path()).filePath("mystery.json"), odd);
    writeJson(QDir(base.path()).filePath("defaults.json"),
              exampleDefaultsJson());

    SettingsStore store;
    store.setBaseDirectory(base.path());
    EXPECT_TRUE(store.reload());
    EXPECT_EQ(store.defaults().defaultKitId, "msys2-ucrt64-gcc");
    EXPECT_FALSE(store.lastErrors().isEmpty());
}

// ---------------------------------------------------------------------------
// Round-trip
// ---------------------------------------------------------------------------

TEST(SettingsJson, RoundTripKeepsValues)
{
    Kit kit;
    fromJson(exampleGccKitJson(), kit);
    const Kit parsed = [&] {
        Kit k;
        fromJson(toJson(kit), k);
        return k;
    }();
    EXPECT_EQ(parsed.id, kit.id);
    EXPECT_EQ(parsed.name, kit.name);
    EXPECT_EQ(parsed.cCompiler, kit.cCompiler);
    EXPECT_EQ(parsed.cppCompiler, kit.cppCompiler);
    EXPECT_EQ(parsed.cppStandard, kit.cppStandard);
    EXPECT_EQ(parsed.compileFlags, kit.compileFlags);
    EXPECT_EQ(parsed.linkFlags, kit.linkFlags);
    EXPECT_EQ(parsed.gitPath, kit.gitPath);
    EXPECT_EQ(parsed.buildTools.value("cmake"), kit.buildTools.value("cmake"));
    ASSERT_EQ(parsed.buildConfigurations.size(),
              kit.buildConfigurations.size());
    EXPECT_EQ(parsed.buildConfigurations[0].name,
              kit.buildConfigurations[0].name);
    EXPECT_EQ(parsed.buildConfigurations[0].compileFlags,
              kit.buildConfigurations[0].compileFlags);
    EXPECT_EQ(parsed.buildConfigurations[0].inheritFromKit,
              kit.buildConfigurations[0].inheritFromKit);

    Preferences prefs;
    fromJson(examplePreferencesJson(), prefs);
    const Preferences parsedPrefs = [&] {
        Preferences p;
        fromJson(toJson(prefs), p);
        return p;
    }();
    EXPECT_EQ(parsedPrefs.editorFont.family, prefs.editorFont.family);
    EXPECT_DOUBLE_EQ(parsedPrefs.editorFont.size, prefs.editorFont.size);
    EXPECT_EQ(parsedPrefs.largeFile.parseDisabledAboveMB,
              prefs.largeFile.parseDisabledAboveMB);
    EXPECT_EQ(parsedPrefs.clangd.completion, prefs.clangd.completion);
    EXPECT_EQ(parsedPrefs.globalStyle, prefs.globalStyle);
}

// --- mergeUnknownKeys ---------------------------------------------------

TEST(SettingsStore, MergeUnknownKeysKeepsHandAddedKeys)
{
    QJsonObject previous;
    previous.insert("type", "preferences");
    QJsonObject prefs;
    prefs.insert("editor.font", "custom");
    prefs.insert("myOwn.key", QJsonValue(true));   // unknown to the model
    QJsonObject nested;
    nested.insert("kept", 1);
    previous.insert("preferences", prefs);
    previous.insert("nested", nested);

    QJsonObject fresh;
    QJsonObject freshPrefs;
    freshPrefs.insert("editor.font", "changed");
    fresh.insert("type", "preferences");
    fresh.insert("preferences", freshPrefs);
    QJsonObject freshNested;
    freshNested.insert("kept", 2);
    freshNested.insert("added", 3);
    fresh.insert("nested", freshNested);

    const QJsonObject merged = SettingsStore::mergeUnknownKeys(fresh, previous);
    EXPECT_EQ(merged.value("preferences").toObject().value("editor.font"),
              QJsonValue(QStringLiteral("changed")));  // fresh wins on known keys
    EXPECT_EQ(merged.value("preferences").toObject().value("myOwn.key"),
              QJsonValue(true));                       // unknown key survives
    EXPECT_EQ(merged.value("nested").toObject().value("kept"), QJsonValue(2));
    EXPECT_EQ(merged.value("nested").toObject().value("added"), QJsonValue(3));
    // A key the fresh document dropped entirely comes back.
    EXPECT_TRUE(merged.contains("type"));
}

TEST(SettingsStore, MergeUnknownKeysHandlesBrokenOrEmptyPrevious)
{
    QJsonObject fresh;
    fresh.insert("type", "preferences");

    const QJsonObject fromEmpty = SettingsStore::mergeUnknownKeys(fresh, {});
    EXPECT_EQ(fromEmpty, fresh);
}

// ---------------------------------------------------------------------------
// Layers: what a layer is entitled to write (R2-05)
//
// A layer's document is serialised from a whole model, so a field the user
// never touched comes out as its struct default — an empty string, an empty
// list. Since the layers merge later-wins, writing those defaults out is not
// "no opinion", it is an instruction to blank the layer below. These tests
// pin down the rule that keeps an untouched field out of the file, and the
// end-to-end consequence for a project layer over a real global config.
// ---------------------------------------------------------------------------

namespace {

/// Base-layer documents holding settings a user would notice losing.
void writeRealBaseConfig(const QString& dir)
{
    QJsonObject defaults = toJson(GlobalDefaults{});
    defaults.insert("defaultKitId", "msys2-ucrt64-gcc");
    defaults.insert("defaultBuildConfiguration", "Debug");
    defaults.insert("gitPath", "E:/msys64/ucrt64/bin/git.exe");
    defaults.insert("clangdPath", "E:/msys64/ucrt64/bin/clangd.exe");
    defaults.insert("clangFormatPath", "E:/msys64/ucrt64/bin/clang-format.exe");
    writeJson(QDir(dir).filePath("defaults.json"), defaults);

    Preferences prefs;
    prefs.defaultEncoding = "GBK";
    prefs.lineEndingDefault = "CRLF";
    prefs.recentFiles = QStringList{QStringLiteral("E:/demo/main.cpp")};
    prefs.disabledPlugins =
        QStringList{QStringLiteral("E:/demo/plugins/old.dll")};
    writeJson(QDir(dir).filePath("preferences.json"), toJson(prefs));
}

}  // namespace

TEST(SettingsLayer, DeltaIsEmptyWhenNothingWasTouched)
{
    // The layer read nothing of its own and the user changed nothing: the
    // serialised model is byte-for-byte what it was at load time.
    const QJsonObject fresh = toJson(GlobalDefaults{});
    const QJsonObject delta =
        SettingsStore::layerDelta(fresh, toJson(GlobalDefaults{}), {});

    EXPECT_FALSE(SettingsStore::layerDeltaHasContent(delta));
    // The header still rides along, so the result would be a valid document.
    EXPECT_EQ(delta.value("type"), QJsonValue(QStringLiteral("defaults")));
    EXPECT_EQ(delta.value("id"), QJsonValue(QStringLiteral("global")));
    EXPECT_FALSE(delta.contains("gitPath"));
    EXPECT_FALSE(delta.contains("defaultKitId"));
}

TEST(SettingsLayer, DeltaKeepsOnlyTheFieldTheUserChanged)
{
    QJsonObject fresh = toJson(GlobalDefaults{});
    fresh.insert("gitPath", "D:/project/tools/git.exe");

    const QJsonObject delta =
        SettingsStore::layerDelta(fresh, toJson(GlobalDefaults{}), {});

    EXPECT_TRUE(SettingsStore::layerDeltaHasContent(delta));
    EXPECT_EQ(delta.value("gitPath"),
              QJsonValue(QStringLiteral("D:/project/tools/git.exe")));
    EXPECT_FALSE(delta.contains("clangdPath"));  // left alone -> left out
    EXPECT_FALSE(delta.contains("defaultKitId"));
}

TEST(SettingsLayer, DeltaKeepsKeysTheLayerAlreadyHad)
{
    // The layer's own file named clangdPath before (nothing else); the user
    // opens the dialog and saves without touching anything. Dropping the key
    // now would silently un-inherit the project's override.
    QJsonObject previous;
    previous.insert("type", "defaults");
    previous.insert("id", "global");
    previous.insert("clangdPath", "D:/project/tools/clangd.exe");
    QJsonObject loaded = toJson(GlobalDefaults{});
    loaded.insert("clangdPath", "D:/project/tools/clangd.exe");

    const QJsonObject delta = SettingsStore::layerDelta(loaded, loaded, previous);

    EXPECT_TRUE(SettingsStore::layerDeltaHasContent(delta));
    EXPECT_EQ(delta.value("clangdPath"),
              QJsonValue(QStringLiteral("D:/project/tools/clangd.exe")));
    EXPECT_FALSE(delta.contains("gitPath"));  // the layer never said
    EXPECT_FALSE(delta.contains("defaultKitId"));
}

TEST(SettingsLayer, DeltaKeepsAnExplicitClear)
{
    // "" in the layer's own file is how a layer says "do not use the one
    // below". The dialog cannot express it, but an unrelated save must not
    // throw it away either.
    QJsonObject previous;
    previous.insert("type", "defaults");
    previous.insert("id", "global");
    previous.insert("gitPath", QString());
    QJsonObject loaded = toJson(GlobalDefaults{});
    loaded.insert("gitPath", QString());

    const QJsonObject delta = SettingsStore::layerDelta(loaded, loaded, previous);

    ASSERT_TRUE(delta.contains("gitPath"));
    EXPECT_EQ(delta.value("gitPath"), QJsonValue(QString()));
    EXPECT_FALSE(delta.contains("clangdPath"));
}

TEST(SettingsLayer, DeltaRecursesIntoNestedObjects)
{
    // Preferences are nested. Changing one field must not hand over the
    // whole sibling object, or "files" alone would reset the encoding,
    // the line ending and the long-line mode together.
    Preferences edited;
    edited.defaultEncoding = "GBK";
    const QJsonObject delta = SettingsStore::layerDelta(
        toJson(edited), toJson(Preferences{}), {});

    ASSERT_TRUE(delta.contains("files"));
    const QJsonObject files = delta.value("files").toObject();
    ASSERT_TRUE(files.contains("encoding"));
    EXPECT_EQ(files.value("encoding").toObject().value("default"),
              QJsonValue(QStringLiteral("GBK")));
    EXPECT_FALSE(files.contains("lineEnding"));
    EXPECT_FALSE(files.contains("longLineMode"));
    EXPECT_FALSE(delta.contains("general"));
    EXPECT_FALSE(delta.contains("plugins"));
}

TEST(SettingsLayer, DeltaCarriesKeysTheModelDoesNotProduce)
{
    // Same courtesy as mergeUnknownKeys(): a key this build knows nothing
    // about belongs to whoever wrote it.
    QJsonObject previous;
    previous.insert("type", "defaults");
    previous.insert("id", "global");
    previous.insert("myOwnTool",
                    QJsonObject{{QStringLiteral("path"),
                                 QStringLiteral("D:/tool.exe")}});

    const QJsonObject delta = SettingsStore::layerDelta(
        toJson(GlobalDefaults{}), toJson(GlobalDefaults{}), previous);

    EXPECT_TRUE(SettingsStore::layerDeltaHasContent(delta));
    EXPECT_EQ(delta.value("myOwnTool"), previous.value("myOwnTool"));
}

TEST(SettingsLayer, ProjectLayerSaveLeavesTheGlobalSettingsAlone)
{
    QTemporaryDir base;
    QTemporaryDir overlay;
    ASSERT_TRUE(base.isValid());
    ASSERT_TRUE(overlay.isValid());
    writeRealBaseConfig(base.path());

    SettingsStore store;
    store.setBaseDirectory(base.path());
    store.setOverlayDirectories({overlay.path()});
    ASSERT_TRUE(store.reload());
    ASSERT_EQ(store.defaults().defaultKitId, QStringLiteral("msys2-ucrt64-gcc"));

    // The dialog opens on the project layer, which has no documents of its
    // own — so its state is the struct defaults, not the effective values.
    const QJsonObject loadedDefaults = toJson(GlobalDefaults{});
    const QJsonObject loadedPreferences = toJson(Preferences{});

    // Apply with nothing edited: the layer has nothing to say, so it writes
    // no file at all and the base keeps winning.
    EXPECT_FALSE(SettingsStore::layerDeltaHasContent(SettingsStore::layerDelta(
        loadedDefaults, loadedDefaults, {})));
    EXPECT_FALSE(SettingsStore::layerDeltaHasContent(SettingsStore::layerDelta(
        loadedPreferences, loadedPreferences, {})));
    EXPECT_TRUE(QDir(overlay.path())
                    .entryList(QStringList{QStringLiteral("*.json")}, QDir::Files)
                    .isEmpty());

    ASSERT_TRUE(store.reload());
    EXPECT_EQ(store.defaults().defaultKitId, QStringLiteral("msys2-ucrt64-gcc"));
    EXPECT_EQ(store.defaults().defaultBuildConfiguration, QStringLiteral("Debug"));
    EXPECT_EQ(store.gitPath(), QStringLiteral("E:/msys64/ucrt64/bin/git.exe"));
    EXPECT_EQ(store.clangdPath(), QStringLiteral("E:/msys64/ucrt64/bin/clangd.exe"));
    EXPECT_EQ(store.clangFormatPath(),
              QStringLiteral("E:/msys64/ucrt64/bin/clang-format.exe"));
    EXPECT_EQ(store.preferences().defaultEncoding, QStringLiteral("GBK"));
    EXPECT_EQ(store.lineEndingForSaving(), QStringLiteral("CRLF"));
    EXPECT_EQ(store.preferences().recentFiles,
              (QStringList{QStringLiteral("E:/demo/main.cpp")}));
    EXPECT_EQ(store.preferences().disabledPlugins,
              (QStringList{QStringLiteral("E:/demo/plugins/old.dll")}));

    // Now the user changes exactly one project setting. That one wins, and
    // nothing else moves.
    Preferences layerPrefs;
    layerPrefs.lineEndingDefault = "LF";
    const QJsonObject delta = SettingsStore::layerDelta(
        toJson(layerPrefs), loadedPreferences, {});
    ASSERT_TRUE(SettingsStore::layerDeltaHasContent(delta));
    writeJson(QDir(overlay.path()).filePath("preferences.json"), delta);

    ASSERT_TRUE(store.reload());
    EXPECT_EQ(store.lineEndingForSaving(), QStringLiteral("LF"));  // project wins
    EXPECT_EQ(store.preferences().defaultEncoding, QStringLiteral("GBK"));
    EXPECT_EQ(store.preferences().recentFiles,
              (QStringList{QStringLiteral("E:/demo/main.cpp")}));
    EXPECT_EQ(store.defaults().defaultKitId, QStringLiteral("msys2-ucrt64-gcc"));
    EXPECT_EQ(store.gitPath(), QStringLiteral("E:/msys64/ucrt64/bin/git.exe"));
}

TEST(SettingsLayer, FullDefaultsDocumentWouldWipeTheBase)
{
    // The hazard the delta rule exists to avoid, stated as a fact about the
    // merge so the two stay consistent: layers are later-wins, so a document
    // that spells out every default really does erase what came before —
    // including, notably, the recents list, which is an array and arrays do
    // not merge field-wise.
    QTemporaryDir base;
    QTemporaryDir overlay;
    ASSERT_TRUE(base.isValid());
    ASSERT_TRUE(overlay.isValid());
    writeRealBaseConfig(base.path());
    writeJson(QDir(overlay.path()).filePath("defaults.json"),
              toJson(GlobalDefaults{}));
    writeJson(QDir(overlay.path()).filePath("preferences.json"),
              toJson(Preferences{}));

    SettingsStore store;
    store.setBaseDirectory(base.path());
    store.setOverlayDirectories({overlay.path()});
    ASSERT_TRUE(store.reload());

    EXPECT_EQ(store.defaults().defaultKitId, QString());   // gone
    EXPECT_EQ(store.gitPath(), QString());                 // gone
    EXPECT_EQ(store.clangdPath(), QString());
    EXPECT_TRUE(store.preferences().recentFiles.isEmpty());  // gone
    EXPECT_TRUE(store.preferences().disabledPlugins.isEmpty());
}

