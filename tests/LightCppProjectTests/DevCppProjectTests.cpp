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
#include <QTemporaryDir>

#include "LightCpp/Text/Encoding.hpp"

namespace {

using LightCpp::Project::BuildConfiguration;
using LightCpp::Project::DevCppMakefilePlan;
using LightCpp::Project::DevCppProject;
using LightCpp::Project::DevCppToolchain;
using LightCpp::Project::devCppMakefilePlan;
using LightCpp::Project::gnuBuildArguments;
using LightCpp::Project::makeArguments;
using LightCpp::Project::parseDevProject;
using LightCpp::Project::splitDevList;
using LightCpp::Project::splitFlagString;
using LightCpp::Project::windresArguments;
using LightCpp::Project::writeDevCppMakefile;

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

// ---------------------------------------------------------------------------
// Makefile generation — the build path the original IDE takes
// ---------------------------------------------------------------------------
namespace {

/// A toolchain the tests can predict: gxx is absolute, so gcc/windres derive
/// from the same directory.
DevCppToolchain testToolchain()
{
    DevCppToolchain tools;
    tools.gxx = QStringLiteral("E:/tools/g++.exe");
    return tools;
}

QStringList makefileLines(const QByteArray& content)
{
    return QString::fromUtf8(content).split(QLatin1Char('\n'));
}

QStringList makefileLines(const DevCppMakefilePlan& plan)
{
    return makefileLines(plan.content);
}

/// Same, for plans whose non-ASCII parts are in the machine's ANSI code
/// page (the makefile is written the way mingw32-make reads it).
QStringList localMakefileLines(const QByteArray& content)
{
    return QString::fromLocal8Bit(content).split(QLatin1Char('\n'));
}

/// Looks up the "NAME      = value" line of a generated makefile (the name
/// is padded to Dev-C++'s column, so the '=' never sits right after it).
QString variableValue(const QStringList& lines, const QString& name)
{
    const QString prefix = name + QChar(u' ');
    for (const QString& line : lines) {
        if (!line.startsWith(prefix)) {
            continue;
        }
        const qsizetype equals = line.indexOf(u'=');
        if (equals < 0) {
            continue;
        }
        QString tail = line.mid(equals + 1);
        if (tail.startsWith(u' ')) {
            tail.remove(0, 1);
        }
        return tail;
    }
    return {};
}

}  // namespace

TEST(DevCppProject, MakefileNamesTheToolsAndVariablesTheClassicWay)
{
    const auto project = parse(kDevCpp5Project);
    ASSERT_TRUE(project.has_value());
    const DevCppMakefilePlan plan =
        devCppMakefilePlan(*project, BuildConfiguration::Debug, testToolchain());

    // Same file, same place as Dev-C++ itself.
    EXPECT_EQ(plan.path, QStringLiteral("E:/proj/Makefile.win"));

    const QStringList lines = makefileLines(plan);
    EXPECT_EQ(variableValue(lines, QStringLiteral("CPP")),
              QStringLiteral("E:/tools/g++.exe"));
    // Derived from the C++ compiler's directory.
    EXPECT_EQ(variableValue(lines, QStringLiteral("CC")),
              QStringLiteral("E:/tools/gcc.exe"));
    EXPECT_EQ(variableValue(lines, QStringLiteral("WINDRES")),
              QStringLiteral("E:/tools/windres.exe"));
    // Objects collect under ObjectOutput=; the README unit is not compiled
    // or linked, so it does not appear at all.
    EXPECT_EQ(variableValue(lines, QStringLiteral("OBJ")),
              QStringLiteral("obj/main.o obj/util.o"));
    EXPECT_EQ(variableValue(lines, QStringLiteral("LINKOBJ")),
              QStringLiteral("obj/main.o obj/util.o"));
    EXPECT_EQ(variableValue(lines, QStringLiteral("BIN")),
              QStringLiteral("bin/hello.exe"));
    // Library directories, then the classified Linker= entries.
    EXPECT_EQ(variableValue(lines, QStringLiteral("LIBS")),
              QStringLiteral("-LD:/libs/lib -static D:/libs/lib/libfoo.a -lm"));
    EXPECT_EQ(variableValue(lines, QStringLiteral("INCS")),
              QStringLiteral("-ID:/libs/include -IE:/other/inc"));
    EXPECT_EQ(variableValue(lines, QStringLiteral("CXXFLAGS")),
              QStringLiteral("$(CXXINCS) -Wall -Wextra -g -std=c++11 -Wall"));
    EXPECT_EQ(variableValue(lines, QStringLiteral("CFLAGS")),
              QStringLiteral("$(INCS) -Wall -Wextra -g -std=c++11 -Wall"));
    EXPECT_EQ(variableValue(lines, QStringLiteral("RM")),
              QStringLiteral("rm.exe -f"));
}

TEST(DevCppProject, MakefileCarriesThePerUnitRules)
{
    const auto project = parse(kDevCpp5Project);
    ASSERT_TRUE(project.has_value());
    const DevCppMakefilePlan plan =
        devCppMakefilePlan(*project, BuildConfiguration::Debug, testToolchain());
    const QStringList lines = makefileLines(plan);

    // The standard target chain, in Dev-C++'s own order.
    const int phony = lines.indexOf(QStringLiteral(
        ".PHONY: all all-before all-after clean clean-custom"));
    ASSERT_GE(phony, 0);
    EXPECT_EQ(lines.at(phony + 2), QStringLiteral("all: all-before $(BIN) all-after"));
    EXPECT_TRUE(lines.contains(QStringLiteral("clean: clean-custom")));
    EXPECT_TRUE(lines.contains(QStringLiteral("\t${RM} $(OBJ) $(BIN)")));
    EXPECT_TRUE(lines.contains(QStringLiteral("$(BIN): $(OBJ)")));
    EXPECT_TRUE(lines.contains(
        QStringLiteral("\t$(CPP) $(LINKOBJ) -o $(BIN) $(LIBS)")));

    // One rule per compilable unit: object target, source dependency, then
    // the compile line through $(CPP) with the project's flags.
    EXPECT_TRUE(lines.contains(QStringLiteral("obj/main.o: main.cpp")));
    EXPECT_TRUE(lines.contains(
        QStringLiteral("\t$(CPP) -c main.cpp -o obj/main.o $(CXXFLAGS)")));
    EXPECT_TRUE(lines.contains(QStringLiteral("obj/util.o: src/util.cpp")));
    EXPECT_TRUE(lines.contains(
        QStringLiteral("\t$(CPP) -c src/util.cpp -o obj/util.o $(CXXFLAGS)")));
}

TEST(DevCppProject, MakefileDisambiguatesCollidingObjectNames)
{
    const auto project = parse(
        "[Project]\n"
        "Name=collide\n"
        "Type=0\n"
        "UnitCount=2\n"
        "ObjectOutput=obj\n"
        "[Unit1]\n"
        "FileName=src/util.cpp\n"
        "Compile=1\n"
        "Link=1\n"
        "[Unit2]\n"
        "FileName=lib/util.cpp\n"
        "Compile=1\n"
        "Link=1\n");
    ASSERT_TRUE(project.has_value());
    const DevCppMakefilePlan plan =
        devCppMakefilePlan(*project, BuildConfiguration::Debug, testToolchain());
    const QStringList lines = makefileLines(plan);

    // Two sources with the same file name in different folders would both
    // compile to obj/util.o and silently overwrite each other. Only these
    // units fold their sub-path into the object name; non-colliding projects
    // keep the original IDE's flat naming (see the per-unit-rules test).
    EXPECT_TRUE(lines.contains(QStringLiteral("obj/src_util.o: src/util.cpp")));
    EXPECT_TRUE(lines.contains(QStringLiteral("obj/lib_util.o: lib/util.cpp")));
    EXPECT_FALSE(lines.contains(QStringLiteral("obj/util.o: src/util.cpp")));
}

TEST(DevCppProject, MakefileDisambiguatesSameStemObjectNames)
{
    // a.c and a.cpp share a stem, so both compile to a.o even though their file
    // names differ — the collision check used to compare file names and miss
    // this, letting one object silently overwrite the other. Same-folder units
    // fold the source's own name (a.c -> a.c.o); the sub-path fold still
    // applies on top when the sources sit in a shared sub-directory.
    const auto project = parse(
        "[Project]\n"
        "Name=stem\n"
        "Type=0\n"
        "UnitCount=4\n"
        "ObjectOutput=obj\n"
        "[Unit1]\n"
        "FileName=a.c\n"
        "Compile=1\n"
        "Link=1\n"
        "[Unit2]\n"
        "FileName=a.cpp\n"
        "Compile=1\n"
        "Link=1\n"
        "[Unit3]\n"
        "FileName=src/b.c\n"
        "Compile=1\n"
        "Link=1\n"
        "[Unit4]\n"
        "FileName=src/b.cpp\n"
        "Compile=1\n"
        "Link=1\n");
    ASSERT_TRUE(project.has_value());
    const DevCppMakefilePlan plan =
        devCppMakefilePlan(*project, BuildConfiguration::Debug, testToolchain());
    const QStringList lines = makefileLines(plan);

    EXPECT_TRUE(lines.contains(QStringLiteral("obj/a.c.o: a.c")));
    EXPECT_TRUE(lines.contains(QStringLiteral("obj/a.cpp.o: a.cpp")));
    EXPECT_TRUE(lines.contains(QStringLiteral("obj/src_b.c.o: src/b.c")));
    EXPECT_TRUE(lines.contains(QStringLiteral("obj/src_b.cpp.o: src/b.cpp")));
    // No unit may claim the bare stem target the other one also wants.
    EXPECT_FALSE(lines.contains(QStringLiteral("obj/a.o: a.c")));
    EXPECT_FALSE(lines.contains(QStringLiteral("obj/src_b.o: src/b.c")));
}

TEST(DevCppProject, MakefileForACProjectLinksThroughCC)
{
    const auto project = parse(
        "[Project]\n"
        "Name=cprog\n"
        "IsCpp=0\n"
        "Compiler=-std=c99\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=main.c\n");
    ASSERT_TRUE(project.has_value());
    const DevCppMakefilePlan plan =
        devCppMakefilePlan(*project, BuildConfiguration::Debug, testToolchain());
    const QStringList lines = makefileLines(plan);

    // Objects sit next to their sources when ObjectOutput= is empty.
    EXPECT_EQ(variableValue(lines, QStringLiteral("OBJ")),
              QStringLiteral("main.o"));
    EXPECT_TRUE(lines.contains(QStringLiteral("\t$(CC) $(LINKOBJ) -o $(BIN) $(LIBS)")));
    EXPECT_TRUE(lines.contains(QStringLiteral("main.o: main.c")));
    EXPECT_TRUE(lines.contains(
        QStringLiteral("\t$(CC) -c main.c -o main.o $(CFLAGS)")));
}

TEST(DevCppProject, MakefileForStaticAndDynamicLibrariesMatchesDevCpp)
{
    const auto staticLib = parse(
        "[Project]\n"
        "Name=mylib\n"
        "Type=2\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=lib.cpp\n");
    ASSERT_TRUE(staticLib.has_value());
    const QStringList staticLines =
        makefileLines(devCppMakefilePlan(*staticLib, BuildConfiguration::Debug,
                                         testToolchain()));
    EXPECT_TRUE(staticLines.contains(QStringLiteral("$(BIN): $(OBJ)")));
    EXPECT_TRUE(staticLines.contains(QStringLiteral("\tar r $(BIN) $(LINKOBJ)")));
    EXPECT_TRUE(staticLines.contains(QStringLiteral("\tranlib $(BIN)")));

    const auto dynLib = parse(
        "[Project]\n"
        "Name=plugin\n"
        "Type=3\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=plugin.cpp\n");
    ASSERT_TRUE(dynLib.has_value());
    const DevCppMakefilePlan plan =
        devCppMakefilePlan(*dynLib, BuildConfiguration::Debug, testToolchain());
    const QStringList dynamicLines = makefileLines(plan);
    // The import library and .def land next to the DLL, like the original.
    EXPECT_EQ(variableValue(dynamicLines, QStringLiteral("DEF")),
              QStringLiteral("libplugin.def"));
    EXPECT_EQ(variableValue(dynamicLines, QStringLiteral("STATIC")),
              QStringLiteral("libplugin.a"));
    EXPECT_TRUE(dynamicLines.contains(QStringLiteral(
        "\t$(CPP) -shared $(LINKOBJ) -o $(BIN) $(LIBS) "
        "-Wl,--output-def,$(DEF),--out-implib,$(STATIC),--add-stdcall-alias")));
}

TEST(DevCppProject, MakefileCompilesTheProjectIconThroughWindres)
{
    const auto project = parse(
        "[Project]\n"
        "Name=hello\n"
        "Icon=app.ico\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=main.cpp\n");
    ASSERT_TRUE(project.has_value());
    const DevCppMakefilePlan plan =
        devCppMakefilePlan(*project, BuildConfiguration::Debug, testToolchain());

    // The generated script rides along with the plan; writeDevCppMakefile
    // parks it at iconResourcePath() before make runs.
    EXPECT_FALSE(plan.resourceScript.isEmpty());
    EXPECT_TRUE(plan.resourceScript.contains("1 ICON"));

    const QStringList lines = makefileLines(plan);
    // The RES object lives in temp; quoted exactly when the path needs it.
    const QString objectPath = project->iconObjectPath();
    const QString expectedRes = objectPath.contains(u' ')
                                    ? QStringLiteral("\"%1\"").arg(objectPath)
                                    : objectPath;
    EXPECT_EQ(variableValue(lines, QStringLiteral("RES")), expectedRes);
    const QString rule = QStringLiteral("%1: %2")
                             .arg(project->iconObjectPath(),
                                  project->iconResourcePath());
    const QString text = lines.join(QLatin1Char('\n'));
    EXPECT_TRUE(text.contains(rule));
    EXPECT_TRUE(text.contains(
        QStringLiteral("$(WINDRES) -i %1 --input-format=rc -o %2 -O coff")
            .arg(project->iconResourcePath(), project->iconObjectPath())));
}

TEST(DevCppProject, PrivateResourceReplacesTheGeneratedIconScript)
{
    const auto project = parse(
        "[Project]\n"
        "Name=hello\n"
        "PrivateResource=helloPrivate.rc\n"
        "Icon=app.ico\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=main.cpp\n");
    ASSERT_TRUE(project.has_value());
    const DevCppMakefilePlan plan =
        devCppMakefilePlan(*project, BuildConfiguration::Debug, testToolchain());
    // A project-carried .rc is what windres gets; nothing is generated.
    EXPECT_TRUE(plan.resourceScript.isEmpty());
    EXPECT_TRUE(makefileLines(plan)
                    .contains(QStringLiteral("helloPrivate.res: helloPrivate.rc")));
}

TEST(DevCppProject, MakefileHonoursAPerUnitOverrideBuildCmd)
{
    const auto project = parse(
        "[Project]\n"
        "Name=special\n"
        "UnitCount=1\n"
        "[Unit1]\n"
        "FileName=weird.cpp\n"
        "OverrideBuildCmd=1\n"
        "BuildCmd=$(CPP) -c <CRTAB> weird.cpp -o weird.o\n");
    ASSERT_TRUE(project.has_value());
    ASSERT_EQ(project->units.size(), 1);
    EXPECT_TRUE(project->units.at(0).overrideBuildCmd);
    const QStringList lines =
        makefileLines(devCppMakefilePlan(*project, BuildConfiguration::Debug,
                                         testToolchain()));
    // <CRTAB> became a real continuation: a tab on the next line.
    const QString text = lines.join(QLatin1Char('\n'));
    EXPECT_TRUE(text.contains(QStringLiteral("\t$(CPP) -c \n\t weird.cpp -o weird.o")));
}

TEST(DevCppProject, WriteDevCppMakefilePutsTheFileWhereDevCppDoes)
{
    const auto project = parse(kDevCpp5Project);
    ASSERT_TRUE(project.has_value());
    // Redirect the project into a temp directory so nothing leaks into the
    // repository.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    DevCppProject local = *project;
    local.filePath = dir.filePath(QStringLiteral("hello.dev"));
    local.projectDir = QDir(dir.path()).filePath(QStringLiteral("hello"));

    const DevCppMakefilePlan plan =
        devCppMakefilePlan(local, BuildConfiguration::Debug, testToolchain());
    ASSERT_TRUE(writeDevCppMakefile(plan, local));

    QFileInfo written(QDir(local.projectDir).filePath(QStringLiteral("Makefile.win")));
    ASSERT_TRUE(written.exists());
    QFile readBack(written.absoluteFilePath());
    ASSERT_TRUE(readBack.open(QIODevice::ReadOnly));
    EXPECT_EQ(readBack.readAll(), plan.content);

    // The ObjectOutput= directory is created up front: make will not.
    EXPECT_TRUE(QDir(QDir(local.projectDir).filePath(QStringLiteral("obj"))).exists());
}

TEST(DevCppProject, GbkEncodedDevFileDecodesItsName)
{
    // Only meaningful where the local ANSI page really is GBK — the fixture
    // is the bytes Dev-C++ writes on a zh-CN machine.
    if (LightCpp::Text::localAnsiEncoding()
        != LightCpp::Text::TextEncoding::Gbk) {
        GTEST_SKIP() << "local ANSI code page is not GBK";
    }
    // "Name=项目1" and "OverrideOutputName=项目1.exe", written as GBK bytes.
    QByteArray content = "[Project]\nName=";
    content += QByteArrayLiteral("\xCF\xEE\xC4\xBF" "1");
    content += "\nOverrideOutput=1\nOverrideOutputName=";
    content += QByteArrayLiteral("\xCF\xEE\xC4\xBF" "1.exe");
    content += "\nUnitCount=1\n[Unit1]\nFileName=main.cpp\n";

    const auto project = parseDevProject(kDevPath, content);
    ASSERT_TRUE(project.has_value());
    // FromUtf8 would have produced U+FFFD mush here — the exact bug that
    // made ld report "cannot open output file".
    EXPECT_EQ(project->name, QStringLiteral("\x9879\x76EE" "1"));
    EXPECT_EQ(project->overrideOutputName,
              QStringLiteral("\x9879\x76EE" "1.exe"));
    EXPECT_EQ(project->outputPath(),
              QStringLiteral("E:/proj/") + QStringLiteral("\x9879\x76EE" "1.exe"));
}

TEST(DevCppProject, MakefileRoutesANonAsciiProgramThroughAnIntermediate)
{
    // Only meaningful where the local ANSI page really is GBK (the encoding
    // the makefile is written in).
    if (LightCpp::Text::localAnsiEncoding()
        != LightCpp::Text::TextEncoding::Gbk) {
        GTEST_SKIP() << "local ANSI code page is not GBK";
    }
    // Name=项目1: recent MSYS2 linkers decode the makefile's ANSI bytes as
    // UTF-8, so the rule must produce an ASCII stand-in and move it.
    QByteArray dev = "[Project]\nName=";
    dev += QByteArrayLiteral("\xCF\xEE\xC4\xBF" "1");
    dev += "\nUnitCount=1\n[Unit1]\nFileName=main.cpp\n";
    const auto project = parseDevProject(kDevPath, dev);
    ASSERT_TRUE(project.has_value());
    const DevCppMakefilePlan plan =
        devCppMakefilePlan(*project, BuildConfiguration::Debug, testToolchain());
    const QStringList lines = localMakefileLines(plan.content);

    // The target keeps its real name — make's timestamp logic watches it ...
    EXPECT_EQ(variableValue(lines, QStringLiteral("BIN")),
              QStringLiteral("\x9879\x76EE" "1.exe"));
    // ... while the linker writes the ASCII stand-in that cmd moves into
    // place (cmd is Unicode, so the ANSI command line round-trips).
    EXPECT_TRUE(lines.contains(QStringLiteral(
        "\t$(CPP) $(LINKOBJ) -o _lightcpp_link.exe $(LIBS)")));
    EXPECT_TRUE(lines.contains(QStringLiteral(
        "\tcmd /c move /y _lightcpp_link.exe \"\x9879\x76EE" "1.exe\"")));
    EXPECT_FALSE(lines.contains(
        QStringLiteral("\t$(CPP) $(LINKOBJ) -o $(BIN) $(LIBS)")));
}

TEST(DevCppProject, MakefileRoutesNonAsciiLibrariesThroughAnIntermediate)
{
    if (LightCpp::Text::localAnsiEncoding()
        != LightCpp::Text::TextEncoding::Gbk) {
        GTEST_SKIP() << "local ANSI code page is not GBK";
    }
    // ar mangles a non-ASCII archive name the same way ld does.
    QByteArray staticDev = "[Project]\nName=";
    staticDev += QByteArrayLiteral("\xCF\xEE\xC4\xBF" "1");
    staticDev += "\nType=2\nUnitCount=1\n[Unit1]\nFileName=lib.cpp\n";
    const auto staticLib = parseDevProject(kDevPath, staticDev);
    ASSERT_TRUE(staticLib.has_value());
    const QStringList staticLines = localMakefileLines(
        devCppMakefilePlan(*staticLib, BuildConfiguration::Debug,
                           testToolchain()).content);
    EXPECT_TRUE(staticLines.contains(
        QStringLiteral("\tar r _lightcpp_link.a $(LINKOBJ)")));
    EXPECT_TRUE(staticLines.contains(
        QStringLiteral("\tranlib _lightcpp_link.a")));
    EXPECT_TRUE(staticLines.contains(QStringLiteral(
        "\tcmd /c move /y _lightcpp_link.a \"\x9879\x76EE" "1.a\"")));

    // A DLL moves its import library and .def out of the stand-ins too.
    QByteArray dynamicDev = "[Project]\nName=";
    dynamicDev += QByteArrayLiteral("\xCF\xEE\xC4\xBF" "1");
    dynamicDev += "\nType=3\nUnitCount=1\n[Unit1]\nFileName=plugin.cpp\n";
    const auto dynLib = parseDevProject(kDevPath, dynamicDev);
    ASSERT_TRUE(dynLib.has_value());
    const QStringList dynamicLines = localMakefileLines(
        devCppMakefilePlan(*dynLib, BuildConfiguration::Debug,
                           testToolchain()).content);
    EXPECT_EQ(variableValue(dynamicLines, QStringLiteral("DEF")),
              QStringLiteral("lib\x9879\x76EE" "1.def"));
    EXPECT_TRUE(dynamicLines.contains(QStringLiteral(
        "\t$(CPP) -shared $(LINKOBJ) -o _lightcpp_link.exe $(LIBS) "
        "-Wl,--output-def,_lightcpp_link.def,--out-implib,_lightcpp_link.a"
        ",--add-stdcall-alias")));
    EXPECT_TRUE(dynamicLines.contains(QStringLiteral(
        "\tcmd /c move /y _lightcpp_link.exe \"\x9879\x76EE" "1.dll\"")));
    EXPECT_TRUE(dynamicLines.contains(QStringLiteral(
        "\tcmd /c move /y _lightcpp_link.def \"lib\x9879\x76EE" "1.def\"")));
    EXPECT_TRUE(dynamicLines.contains(QStringLiteral(
        "\tcmd /c move /y _lightcpp_link.a \"lib\x9879\x76EE" "1.a\"")));
}
