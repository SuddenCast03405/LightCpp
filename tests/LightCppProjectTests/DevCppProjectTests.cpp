// DevCppProjectTests — reading Bloodshed Dev-C++ `.dev` project files.
//
// The fixtures are written out as literal INI text rather than produced by a
// Dev-C++ install, so each dialect quirk (the `_@@_` separator, [UnitN]
// sections against UnitN= keys, CppName in templates) is pinned by an input
// that says exactly which shape it is testing.

#include "LightCpp/Project/DevCppProject.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>

namespace {

using LightCpp::Project::BuildConfiguration;
using LightCpp::Project::DevCppProject;
using LightCpp::Project::gnuBuildArguments;
using LightCpp::Project::makeArguments;
using LightCpp::Project::parseDevProject;
using LightCpp::Project::splitDevList;
using LightCpp::Project::splitFlagString;
using LightCpp::Project::windresArguments;

const QString kDevPath = QStringLiteral("E:/proj/hello.dev");

std::optional<DevCppProject> parse(const char* content, QString* error = nullptr)
{
    return parseDevProject(kDevPath, QByteArray(content), error);
}

/// A Dev-C++ 5.x file the way the IDE actually writes one: everything in
/// [Project] except the unit list, which lives in [UnitN] sections.
const char* kDevCpp5Project =
    "[Project]\n"
    "FileName=hello.dev\n"
    "Name=hello\n"
    "Type=0\n"
    "Ver=1\n"
    "ObjFiles=\n"
    "Includes=D:\\libs\\include_@@_E:/other/inc\n"
    "Libs=D:\\libs\\lib\n"
    "PrivateResource=\n"
    "ResourceIncludes=\n"
    "MakeIncludes=\n"
    "Compiler=\n"
    "CppCompiler=-std=c++11_@@_-Wall\n"
    "Linker=-static_@@_D:\\libs\\lib\\libfoo.a_@@_m\n"
    "IsCpp=1\n"
    "Icon=\n"
    "ExeOutput=bin\n"
    "ObjectOutput=obj\n"
    "OverrideOutput=0\n"
    "OverrideOutputName=\n"
    "HostApplication=\n"
    "Folders=src\n"
    "CommandLine=--verbose -q\n"
    "UseCustomMakefile=0\n"
    "CustomMakefile=\n"
    "IncludeVersionInfo=0\n"
    "SupportXPThemes=0\n"
    "CompilerSet=0\n"
    "CompilerSettings=\n"
    "UnitCount=3\n"
    "[Unit1]\n"
    "FileName=main.cpp\n"
    "CompileCpp=1\n"
    "Folder=\n"
    "Compile=1\n"
    "Link=1\n"
    "Priority=1000\n"
    "OverrideBuildCmd=0\n"
    "BuildCmd=\n"
    "[Unit2]\n"
    "FileName=src\\util.cpp\n"
    "CompileCpp=1\n"
    "Folder=src\n"
    "Compile=1\n"
    "Link=1\n"
    "Priority=1000\n"
    "OverrideBuildCmd=0\n"
    "BuildCmd=\n"
    "[Unit3]\n"
    "FileName=README.md\n"
    "CompileCpp=1\n"
    "Folder=\n"
    "Compile=0\n"
    "Link=0\n"
    "Priority=1000\n"
    "[VersionInfo]\n"
    "Major=0\n"
    "Minor=1\n"
    "Release=1\n"
    "Build=1\n";

}  // namespace

TEST(DevCppProject, ParsesADevCpp5Project)
{
    const auto project = parse(kDevCpp5Project);
    ASSERT_TRUE(project.has_value());
    EXPECT_TRUE(project->isValid());
    EXPECT_EQ(project->name, QStringLiteral("hello"));
    EXPECT_EQ(project->filePath, kDevPath);
    EXPECT_EQ(project->projectDir, QStringLiteral("E:/proj"));
    EXPECT_TRUE(project->isCpp);
    EXPECT_EQ(project->type, 0);
    EXPECT_TRUE(project->producesExecutable());
    EXPECT_EQ(project->targetLabel(), QStringLiteral("application"));
}

TEST(DevCppProject, ParsesTheCompilerAndLinkerSettings)
{
    const auto project = parse(kDevCpp5Project);
    ASSERT_TRUE(project.has_value());

    // _@@_ separates entries, so "-std=c++11" and "-Wall" are two switches.
    EXPECT_EQ(project->compilerFlags,
              QStringList({QStringLiteral("-std=c++11"), QStringLiteral("-Wall")}));
    // Paths are normalised to forward slashes; switches are left alone.
    EXPECT_EQ(project->includeDirs,
              QStringList({QStringLiteral("D:/libs/include"), QStringLiteral("E:/other/inc")}));
    EXPECT_EQ(project->libraryDirs, QStringList({QStringLiteral("D:/libs/lib")}));
    EXPECT_EQ(project->linkerFlags,
              QStringList({QStringLiteral("-static"),
                           QStringLiteral("D:\\libs\\lib\\libfoo.a"),
                           QStringLiteral("m")}));
    // No _@@_ in this one, so it falls back to the whitespace dialect.
    EXPECT_EQ(project->runArguments,
              QStringList({QStringLiteral("--verbose"), QStringLiteral("-q")}));
}

TEST(DevCppProject, UnitsComeOutInUnitOrder)
{
    const auto project = parse(kDevCpp5Project);
    ASSERT_TRUE(project.has_value());
    ASSERT_EQ(project->units.size(), 3);
    EXPECT_EQ(project->units.at(0).fileName, QStringLiteral("main.cpp"));
    EXPECT_EQ(project->units.at(1).fileName, QStringLiteral("src\\util.cpp"));
    EXPECT_EQ(project->units.at(2).fileName, QStringLiteral("README.md"));
}

TEST(DevCppProject, UnitsExcludedFromTheBuildAreDropped)
{
    const auto project = parse(kDevCpp5Project);
    ASSERT_TRUE(project.has_value());
    // README.md carries Compile=0 / Link=0 — it is in the project tree so it
    // can be edited, but it is not a translation unit.
    EXPECT_EQ(project->buildUnitPaths(),
              QStringList({QStringLiteral("E:/proj/main.cpp"),
                           QStringLiteral("E:/proj/src/util.cpp")}));
}

TEST(DevCppProject, HeadersAreNeverHandedToTheCompiler)
{
    // Dev-C++ lists headers as units. Passing one to g++ would make it emit a
    // precompiled header into the user's source tree, so they are filtered.
    const auto withHeader = parse(
        "[Project]\n"
        "Name=mixed\n"
        "IsCpp=1\n"
        "UnitCount=2\n"
        "[Unit1]\n"
        "FileName=main.cpp\n"
        "Compile=1\n"
        "Link=1\n"
        "[Unit2]\n"
        "FileName=include\\thing.h\n"
        "Compile=1\n"
        "Link=1\n");
    ASSERT_TRUE(withHeader.has_value());
    EXPECT_EQ(withHeader->units.size(), 2);
    EXPECT_EQ(withHeader->buildUnitPaths(),
              QStringList({QStringLiteral("E:/proj/main.cpp")}));
    EXPECT_TRUE(withHeader->isValid());
}

TEST(DevCppProject, AHeaderOnlyProjectIsRejected)
{
    QString error;
    const auto project = parse(
        "[Project]\n"
        "Name=headers\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=thing.h\n",
        &error);
    EXPECT_FALSE(project.has_value());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(error.contains(QStringLiteral("compilable")));
}

TEST(DevCppProject, TemplateStyleUnitsUseCppNameAndStartAtUnitZero)
{
    // The IDE's own template files name the unit differently, number from
    // zero, and list it as a UnitN= key inside [Project].
    const auto project = parse(
        "[Template]\n"
        "ver=3\n"
        "Name=Console Application\n"
        "[Project]\n"
        "Name=Console App\n"
        "UnitCount=1\n"
        "Unit1=main.cpp\n"
        "[Unit0]\n"
        "CppName=main.cpp\n"
        "Cpp=consoleapp_cpp.txt\n"
        "[Profile0]\n"
        "Type=1\n"
        "CppCompiler=-O2\n");
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->name, QStringLiteral("Console App"));
    ASSERT_EQ(project->units.size(), 1);
    EXPECT_EQ(project->units.at(0).fileName, QStringLiteral("main.cpp"));
    EXPECT_EQ(project->buildUnitPaths(), QStringList({QStringLiteral("E:/proj/main.cpp")}));
}

TEST(DevCppProject, DuplicateUnitsAcrossBothSpellingsAreCountedOnce)
{
    const auto project = parse(
        "[Project]\n"
        "Name=dupes\n"
        "UnitCount=2\n"
        "Unit1=main.cpp\n"
        "Unit2=util.cpp\n"
        "[Unit1]\n"
        "FileName=main.cpp\n"
        "Compile=1\n"
        "Link=1\n"
        "[Unit2]\n"
        "FileName=.\\util.cpp\n"
        "Compile=1\n"
        "Link=1\n");
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->units.size(), 2);
    EXPECT_EQ(project->buildUnitPaths(),
              QStringList({QStringLiteral("E:/proj/main.cpp"),
                           QStringLiteral("E:/proj/util.cpp")}));
}

TEST(DevCppProject, CProjectsReadTheCompilerKey)
{
    const auto project = parse(
        "[Project]\n"
        "Name=cprog\n"
        "IsCpp=0\n"
        "Compiler=-std=c99\n"
        "CppCompiler=-std=c++20\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=main.c\n");
    ASSERT_TRUE(project.has_value());
    EXPECT_FALSE(project->isCpp);
    EXPECT_EQ(project->compilerFlags, QStringList({QStringLiteral("-std=c99")}));
}

TEST(DevCppProject, ExeOutputDirectoryIsWhereTheProgramLands)
{
    const auto project = parse(kDevCpp5Project);
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->outputDirectory(), QStringLiteral("E:/proj/bin"));
    EXPECT_EQ(project->outputPath(), QStringLiteral("E:/proj/bin/hello.exe"));
}

TEST(DevCppProject, OverrideOutputNameWinsAndMayCarryADirectory)
{
    const auto project = parse(
        "[Project]\n"
        "Name=hello\n"
        "OverrideOutput=1\n"
        "OverrideOutputName=out\\custom.exe\n"
        "ExeOutput=bin\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=main.cpp\n");
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->outputPath(), QStringLiteral("E:/proj/bin/out/custom.exe"));
}

TEST(DevCppProject, DynamicLibrariesGetADllNameAndSharedFlag)
{
    const auto project = parse(
        "[Project]\n"
        "Name=plugin\n"
        "Type=3\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=plugin.cpp\n");
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->targetLabel(), QStringLiteral("dynamic library"));
    EXPECT_FALSE(project->producesExecutable());
    EXPECT_TRUE(project->outputPath().endsWith(QStringLiteral("plugin.dll")));

    const QStringList args = gnuBuildArguments(*project, BuildConfiguration::Debug,
                                              QStringLiteral("g++"), project->outputPath());
    EXPECT_TRUE(args.contains(QStringLiteral("-shared")));
}

TEST(DevCppProject, StaticLibrariesAreReportedAndNotRunnable)
{
    const auto project = parse(
        "[Project]\n"
        "Name=mylib\n"
        "Type=2\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=lib.cpp\n");
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->targetLabel(), QStringLiteral("static library"));
    EXPECT_FALSE(project->producesExecutable());
    // The artifact is named like a library, not like a program.
    EXPECT_TRUE(project->outputPath().endsWith(QStringLiteral("mylib.a")));

    // A static library needs `ar` after the compile, which one g++ invocation
    // cannot do — the window refuses this up front rather than half-building
    // it, so -shared must not appear either.
    const QStringList args = gnuBuildArguments(*project, BuildConfiguration::Debug,
                                              QStringLiteral("g++"), project->outputPath());
    EXPECT_FALSE(args.contains(QStringLiteral("-shared")));
}

TEST(DevCppProject, GnuBuildArgumentsIncludeTheProjectSettings)
{
    const auto project = parse(
        "[Project]\n"
        "Name=tiny\n"
        "IsCpp=1\n"
        "CppCompiler=-std=c++17\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=main.cpp\n");
    ASSERT_TRUE(project.has_value());

    EXPECT_EQ(gnuBuildArguments(*project, BuildConfiguration::Debug,
                                QStringLiteral("g++"), QStringLiteral("E:/proj/tiny.exe")),
              QStringList({QStringLiteral("-Wall"), QStringLiteral("-Wextra"),
                           QStringLiteral("-g"),
                           // No -std= of our own: the project already says
                           // which standard it wants.
                           QStringLiteral("-std=c++17"),
                           QStringLiteral("E:/proj/main.cpp"),
                           QStringLiteral("-o"), QStringLiteral("E:/proj/tiny.exe")}));

    EXPECT_EQ(gnuBuildArguments(*project, BuildConfiguration::Release,
                                QStringLiteral("g++"), QStringLiteral("E:/proj/tiny.exe")),
              QStringList({QStringLiteral("-Wall"), QStringLiteral("-Wextra"),
                           QStringLiteral("-O2"), QStringLiteral("-DNDEBUG"),
                           QStringLiteral("-std=c++17"),
                           QStringLiteral("E:/proj/main.cpp"),
                           QStringLiteral("-o"), QStringLiteral("E:/proj/tiny.exe")}));
}

TEST(DevCppProject, LinkerEntriesAreClassified)
{
    const auto project = parse(kDevCpp5Project);
    ASSERT_TRUE(project.has_value());
    const QStringList args = gnuBuildArguments(*project, BuildConfiguration::Debug,
                                              QStringLiteral("g++"),
                                              QStringLiteral("E:/proj/bin/hello.exe"));
    // A switch goes through as a switch.
    EXPECT_TRUE(args.contains(QStringLiteral("-static")));
    // An absolute .a path is linked by path, with the separator normalised.
    EXPECT_TRUE(args.contains(QStringLiteral("D:/libs/lib/libfoo.a")));
    // A bare name is a library to search for.
    EXPECT_TRUE(args.contains(QStringLiteral("-lm")));
    EXPECT_TRUE(args.contains(QStringLiteral("-I")));
    EXPECT_TRUE(args.contains(QStringLiteral("D:/libs/include")));
    EXPECT_TRUE(args.contains(QStringLiteral("-L")));
    EXPECT_TRUE(args.contains(QStringLiteral("D:/libs/lib")));
}

TEST(DevCppProject, CustomMakefileReplacesTheGeneratedCommand)
{
    const auto project = parse(
        "[Project]\n"
        "Name=hello\n"
        "UseCustomMakefile=1\n"
        "CustomMakefile=Makefile.win\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=main.cpp\n");
    ASSERT_TRUE(project.has_value());
    EXPECT_TRUE(project->useCustomMakefile);
    EXPECT_EQ(project->makefilePath(), QStringLiteral("E:/proj/Makefile.win"));
    EXPECT_EQ(makeArguments(*project),
              QStringList({QStringLiteral("-f"), QStringLiteral("E:/proj/Makefile.win"),
                           QStringLiteral("-C"), QStringLiteral("E:/proj")}));
}

TEST(DevCppProject, AnIconBecomesAWindresStepOutsideTheProject)
{
    const auto project = parse(
        "[Project]\n"
        "Name=hello\n"
        "Icon=app.ico\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=main.cpp\n");
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->iconPath, QStringLiteral("E:/proj/app.ico"));

    // The compiled icon and its script are parked in temp: a build must not
    // leave anything in the user's source tree.
    EXPECT_TRUE(project->iconObjectPath().startsWith(QDir::tempPath()));
    EXPECT_TRUE(project->iconResourcePath().startsWith(QDir::tempPath()));
    EXPECT_TRUE(project->iconObjectPath().endsWith(QStringLiteral("hello-icon.o")));
    EXPECT_TRUE(project->iconResourceScript().contains("1 ICON"));
    EXPECT_TRUE(QString::fromUtf8(project->iconResourceScript())
                    .contains(QStringLiteral("E:/proj/app.ico")));

    const QStringList args = windresArguments(*project, project->iconObjectPath());
    EXPECT_EQ(args,
              QStringList({QStringLiteral("-i"), project->iconResourcePath(),
                           QStringLiteral("-O"), QStringLiteral("coff"),
                           QStringLiteral("-o"), project->iconObjectPath()}));
}

TEST(DevCppProject, BomCommentsAndBlankLinesAreIgnored)
{
    QByteArray content("\xEF\xBB\xBF", 3);
    content += "# Project generated by Dev-C++\n";
    content += "\n";
    content += "; a semicolon comment too\n";
    content += "[Project]\n";
    content += "Name=bommed\n";
    content += "UnitCount=1\n";
    content += "[Unit1]\n";
    content += "FileName=main.cpp\n";

    const auto project = parseDevProject(kDevPath, content);
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->name, QStringLiteral("bommed"));
    EXPECT_EQ(project->units.size(), 1);
}

TEST(DevCppProject, EmptyAndUnitlessProjectsAreRejectedWithAReason)
{
    QString error;
    EXPECT_FALSE(parseDevProject(kDevPath, QByteArray(), &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    error.clear();
    EXPECT_FALSE(parse("", &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    error.clear();
    EXPECT_FALSE(parse("[Project]\nName=empty\nUnitCount=0\n", &error).has_value());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(error.contains(QStringLiteral("no source files")));
}

TEST(DevCppProject, SplitFlagStringHandlesBothDialects)
{
    // The format's own separator keeps a value containing spaces intact.
    EXPECT_EQ(splitFlagString(QStringLiteral("-std=c++11_@@_-Wall")),
              QStringList({QStringLiteral("-std=c++11"), QStringLiteral("-Wall")}));
    EXPECT_EQ(splitFlagString(QStringLiteral("-IC:\\Program Files\\inc_@@_-O2")),
              QStringList({QStringLiteral("-IC:\\Program Files\\inc"),
                           QStringLiteral("-O2")}));
    // Without a separator, whitespace and quotes.
    EXPECT_EQ(splitFlagString(QStringLiteral("-O2 -static")),
              QStringList({QStringLiteral("-O2"), QStringLiteral("-static")}));
    EXPECT_EQ(splitFlagString(QStringLiteral("-I\"C:/Program Files/inc\" -O2")),
              QStringList({QStringLiteral("-IC:/Program Files/inc"),
                           QStringLiteral("-O2")}));
    EXPECT_TRUE(splitFlagString(QStringLiteral("   ")).isEmpty());
}

TEST(DevCppProject, SplitDevListDropsEmptyEntriesAndQuotes)
{
    EXPECT_EQ(splitDevList(QStringLiteral("a_@@_b_@@_")),
              QStringList({QStringLiteral("a"), QStringLiteral("b")}));
    EXPECT_EQ(splitDevList(QStringLiteral("\"C:/with space/inc\"_@@_C:/plain")),
              QStringList({QStringLiteral("C:/with space/inc"),
                           QStringLiteral("C:/plain")}));
    EXPECT_TRUE(splitDevList(QString()).isEmpty());
}
