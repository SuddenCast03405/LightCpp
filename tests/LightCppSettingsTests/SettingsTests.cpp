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
    toolchain.insert("cCompiler", "E:/msys64/ucrt64/bin/gcc.exe");
    toolchain.insert("cppCompiler", "E:/msys64/ucrt64/bin/g++.exe");
    toolchain.insert("debugger", QJsonValue::Null);
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

TEST(SettingsJson, ExampleToolchainAndDefaultsParse)
{
    Toolchain toolchain;
    fromJson(exampleToolchainJson(), toolchain);
    EXPECT_EQ(toolchain.id, "msys2-ucrt64");
    EXPECT_EQ(toolchain.name, "MSYS2 UCRT64");
    EXPECT_EQ(toolchain.cCompiler, "E:/msys64/ucrt64/bin/gcc.exe");
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
    kitJson.insert("cCompiler", "");
    kitJson.insert("cppCompiler", "");

    Kit kit;
    fromJson(kitJson, kit);
    EXPECT_TRUE(kit.inheritToolchain);
    EXPECT_EQ(kit.toolchainId, "msys2-ucrt64");

    const Kit parsed = [&] {
        Kit k;
        fromJson(toJson(kit), k);
        return k;
    }();
    EXPECT_EQ(parsed.inheritToolchain, kit.inheritToolchain);
    EXPECT_EQ(parsed.toolchainId, kit.toolchainId);
    EXPECT_TRUE(parsed.cCompiler.isEmpty());
    EXPECT_TRUE(parsed.cppCompiler.isEmpty());
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

    // The kit names no tools of its own; everything comes from the
    // toolchain it references.
    QJsonObject kitJson = exampleGccKitJson();
    kitJson.insert("inheritToolchain", true);
    kitJson.insert("toolchainId", "msys2-ucrt64");
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
    EXPECT_EQ(store.toolchains()[0].cCompiler,
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
