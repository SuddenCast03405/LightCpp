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

QJsonObject exampleToolchainsJson()
{
    QJsonObject tools;
    tools.insert("type", "toolchains");
    tools.insert("id", "global");
    tools.insert("cCompilers", QJsonArray{"E:/msys64/ucrt64/bin/gcc.exe",
                                          "E:/msys64/ucrt64/bin/clang.exe"});
    tools.insert("cppCompilers", QJsonArray{"E:/msys64/ucrt64/bin/g++.exe",
                                            "E:/msys64/ucrt64/bin/clang++.exe"});
    tools.insert("debuggers", QJsonArray{});
    tools.insert("clangd", QJsonArray{"E:/msys64/ucrt64/bin/clangd.exe"});
    tools.insert("clangFormat",
                 QJsonArray{"E:/msys64/ucrt64/bin/clang-format.exe"});
    tools.insert("buildTools",
                 QJsonObject{{"cmake", QJsonArray{"E:/msys64/ucrt64/bin/cmake.exe"}},
                             {"ninja", QJsonArray{"E:/msys64/ucrt64/bin/ninja.exe"}},
                             {"gnuMake",
                              QJsonArray{"E:/msys64/ucrt64/bin/mingw32-make.exe"}},
                             {"msbuild", QJsonArray{}}});
    return tools;
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
    EXPECT_EQ(prefs.lineEndingPlatform.value("windows"), "CRLF");
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

TEST(SettingsJson, ExampleToolchainsAndDefaultsParse)
{
    ToolchainLists lists;
    fromJson(exampleToolchainsJson(), lists);
    EXPECT_EQ(lists.cCompilers.size(), 2);
    EXPECT_EQ(lists.buildTools.value("gnuMake"),
              QStringList{"E:/msys64/ucrt64/bin/mingw32-make.exe"});

    GlobalDefaults defaults;
    fromJson(exampleDefaultsJson(), defaults);
    EXPECT_EQ(defaults.defaultKitId, "msys2-ucrt64-gcc");
    EXPECT_EQ(defaults.defaultBuildConfiguration, "Debug");
    EXPECT_EQ(defaults.clangdPath, "E:/msys64/ucrt64/bin/clangd.exe");
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

    // auto + windows override -> CRLF (the platform override decides).
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

    // An explicit LF beats the platform override.
    QJsonObject prefs = examplePreferencesJson();
    QJsonObject files = prefs.value("files").toObject();
    files.insert("lineEnding",
                 QJsonObject{{"default", "LF"},
                             {"platformOverrides",
                              QJsonObject{{"windows", "CRLF"}}}});
    prefs.insert("files", files);
    writeJson(QDir(base.path()).filePath("preferences.json"), prefs);
    ASSERT_TRUE(store.reload());
    EXPECT_EQ(store.lineEndingForSaving(), "LF");
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
