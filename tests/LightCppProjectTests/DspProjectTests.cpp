// DspProjectTests — reading Visual Studio 6 `.dsp` project files and
// `.dsw` workspaces.
//
// The fixtures are written out as literal text in the exact shape VS6
// writes (MSG-tail header, CFG line, !IF blocks, # PROP/# ADD lines,
// SOURCE= entries), so each parser assumption — BASE lines skipped,
// quoted /D values, /out: overrides, the ` - ` split inside a CFG name —
// is pinned by an input that says which shape it is testing.

#include "LightCpp/Project/DspProject.hpp"

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "LightCpp/Text/Encoding.hpp"

namespace {

using LightCpp::Project::DspProject;
using LightCpp::Project::DspTargetKind;
using LightCpp::Project::DswWorkspace;
using LightCpp::Project::dspTargetLabel;
using LightCpp::Project::loadDspProject;
using LightCpp::Project::parseDspProject;
using LightCpp::Project::parseDswWorkspace;

const QString kDspPath = QStringLiteral("E:/proj/hello.dsp");
const QString kDswPath = QStringLiteral("E:/proj/hello.dsw");

/// The last parse error, streamed into ASSERTs.
QString lastError;

std::optional<DspProject> parseDsp(const char* content)
{
    lastError.clear();
    return parseDspProject(kDspPath, QByteArray(content), &lastError);
}

/// A VS6 console-application project, Release and Debug, the way the IDE
/// actually writes one — including the !MESSAGE block a naive line parser
/// would misread as configuration blocks.
const char* kDspConsole =
    "# Microsoft Developer Studio Project File - Name=\"hello\" - Package Owner=<4>\n"
    "# Microsoft Developer Studio Generated Build File, Format Version 6.00\n"
    "# ** DO NOT EDIT **\n"
    "\n"
    "# TARGTYPE \"Win32 (x86) Console Application\" 0x0103\n"
    "\n"
    "CFG=hello - Win32 Debug\n"
    "!MESSAGE This is not a valid makefile. To build this project using NMAKE,\n"
    "!MESSAGE use the Export Makefile command and run\n"
    "!MESSAGE \n"
    "!MESSAGE NMAKE /f \"hello.mak\".\n"
    "!MESSAGE \n"
    "!MESSAGE You can specify a configuration when running NMAKE\n"
    "!MESSAGE by defining the macro CFG on the command line. For example:\n"
    "!MESSAGE \n"
    "!MESSAGE NMAKE /f \"hello.mak\" CFG=\"hello - Win32 Debug\"\n"
    "!MESSAGE \n"
    "!MESSAGE Possible choices for configuration are:\n"
    "!MESSAGE \n"
    "!MESSAGE \"hello - Win32 Release\" (based on \"Win32 (x86) Console Application\")\n"
    "!MESSAGE \"hello - Win32 Debug\" (based on \"Win32 (x86) Console Application\")\n"
    "!MESSAGE \n"
    "\n"
    "# Begin Project\n"
    "# PROP AllowPerConfigDependencies 0\n"
    "# PROP Scc_ProjName \"\"\n"
    "# PROP Scc_LocalPath \"\"\n"
    "CPP=cl.exe\n"
    "RSC=rc.exe\n"
    "\n"
    "!IF  \"$(CFG)\" == \"hello - Win32 Release\"\n"
    "\n"
    "# PROP BASE Use_Debug_Libraries 0\n"
    "# PROP BASE Output_Dir \"Release\"\n"
    "# PROP BASE Intermediate_Dir \"Release\"\n"
    "# PROP BASE Target_Dir \"\"\n"
    "# PROP Use_Debug_Libraries 0\n"
    "# PROP Output_Dir \"Release\"\n"
    "# PROP Intermediate_Dir \"Release\"\n"
    "# PROP Target_Dir \"\"\n"
    "# ADD BASE CPP /nologo /W3 /GX /O2 /D \"WIN32\" /D \"NDEBUG\" /D \"_CONSOLE\" /YX /FD /c\n"
    "# ADD CPP /nologo /W3 /GX /O2 /I \"..\\common\" /D \"WIN32\" /D \"NDEBUG\" /D \"_CONSOLE\" /YX /FD /c\n"
    "# ADD BASE LINK32 kernel32.lib /nologo /subsystem:console /machine:I386\n"
    "# ADD LINK32 kernel32.lib /nologo /subsystem:console /machine:I386 /out:\"Release\\custom.exe\"\n"
    "\n"
    "!ELSEIF  \"$(CFG)\" == \"hello - Win32 Debug\"\n"
    "\n"
    "# PROP BASE Use_Debug_Libraries 1\n"
    "# PROP BASE Output_Dir \"Debug\"\n"
    "# PROP BASE Intermediate_Dir \"Debug\"\n"
    "# PROP Use_Debug_Libraries 1\n"
    "# PROP Output_Dir \"Debug\"\n"
    "# PROP Intermediate_Dir \"Debug\"\n"
    "# ADD BASE CPP /nologo /W3 /Gm /Zi /Od /D \"WIN32\" /D \"_DEBUG\" /D \"_CONSOLE\" /YX /FD /c\n"
    "# ADD CPP /nologo /W3 /Gm /Zi /Od /D \"WIN32\" /D \"_DEBUG\" /D \"_CONSOLE\" /YX /FD /c\n"
    "# ADD BASE LINK32 kernel32.lib /nologo /subsystem:console /debug /machine:I386\n"
    "# ADD LINK32 kernel32.lib /nologo /subsystem:console /debug /machine:I386\n"
    "\n"
    "!ENDIF \n"
    "\n"
    "# Begin Target\n"
    "\n"
    "# Name \"hello - Win32 Release\"\n"
    "# Name \"hello - Win32 Debug\"\n"
    "# Begin Group \"Source Files\"\n"
    "\n"
    "# PROP Default_Filter \"cpp;c;cxx\"\n"
    "SOURCE=.\\main.cpp\n"
    "SOURCE=.\\util\\helper.cpp\n"
    "# End Group\n"
    "# Begin Group \"Header Files\"\n"
    "\n"
    "# PROP Default_Filter \"h;hpp\"\n"
    "SOURCE=.\\hello.h\n"
    "# End Group\n"
    "# Begin Group \"Resource Files\"\n"
    "\n"
    "# PROP Default_Filter \"ico;cur;bmp;rc\"\n"
    "SOURCE=.\\app.rc\n"
    "# End Group\n"
    "# End Target\n"
    "# End Project\n";

}  // namespace

TEST(DspProject, ParsesConfigurationsFromVs6Dsp)
{
    const auto project = parseDsp(kDspConsole);
    ASSERT_TRUE(project.has_value()) << lastError.toStdString();

    EXPECT_EQ(project->name, QStringLiteral("hello"));
    ASSERT_EQ(project->configurations.size(), 2);

    // !MESSAGE lines carry quoted CFG-shaped strings too — only !IF and
    // !ELSEIF may open a configuration, and the two blocks here are Release
    // then Debug, in file order.
    EXPECT_EQ(project->configurations[0].fullName,
              QStringLiteral("hello - Win32 Release"));
    EXPECT_EQ(project->configurations[0].name, QStringLiteral("Release"));
    EXPECT_EQ(project->configurations[0].platform, QStringLiteral("Win32"));
    EXPECT_EQ(project->configurations[0].kind, DspTargetKind::Application);
    EXPECT_EQ(project->configurations[1].fullName,
              QStringLiteral("hello - Win32 Debug"));

    // Output_Dir, with the BASE variants ignored.
    EXPECT_EQ(project->configurations[0].outDir, QStringLiteral("Release"));
    EXPECT_EQ(project->configurations[1].outDir, QStringLiteral("Debug"));
}

TEST(DspProject, ReadsDefinesAndIncludesFromAddCpp)
{
    const auto project = parseDsp(kDspConsole);
    ASSERT_TRUE(project.has_value());

    // The Release block: quoted /D values and a quoted /I directory. The
    // BASE line's flags must not leak in.
    EXPECT_EQ(project->configurations[0].defines,
              (QStringList{QStringLiteral("WIN32"), QStringLiteral("NDEBUG"),
                           QStringLiteral("_CONSOLE")}));
    EXPECT_EQ(project->configurations[0].includeDirs,
              (QStringList{QStringLiteral("../common")}));
    // Debug defines NDEBUG's sibling.
    EXPECT_TRUE(project->configurations[1].defines.contains(
        QStringLiteral("_DEBUG")));
}

TEST(DspProject, LinkOutOverridesDefaultOutputName)
{
    const auto project = parseDsp(kDspConsole);
    ASSERT_TRUE(project.has_value());

    // Release redirects with /out:"Release\custom.exe" — the override wins
    // over the <dir>\<name>.exe default.
    EXPECT_EQ(project->outputPathFor(project->configurations[0]),
              QStringLiteral("E:/proj/Release/custom.exe"));
    // Debug falls back to the default shape.
    EXPECT_EQ(project->outputPathFor(project->configurations[1]),
              QStringLiteral("E:/proj/Debug/hello.exe"));
}

TEST(DspProject, ClassifiesFilesByExtension)
{
    const auto project = parseDsp(kDspConsole);
    ASSERT_TRUE(project.has_value());

    EXPECT_EQ(project->sourceFiles,
              (QStringList{QStringLiteral("E:/proj/main.cpp"),
                           QStringLiteral("E:/proj/util/helper.cpp")}));
    EXPECT_EQ(project->headerFiles,
              (QStringList{QStringLiteral("E:/proj/hello.h")}));
    EXPECT_EQ(project->resourceFiles,
              (QStringList{QStringLiteral("E:/proj/app.rc")}));
}

TEST(DspProject, DllAndStaticLibraryTargTypes)
{
    const char* dllDsp =
        "# TARGTYPE \"Win32 (x86) Dynamic-Link Library\" 0x0102\n"
        "CFG=lib - Win32 Debug\n"
        "!IF  \"$(CFG)\" == \"lib - Win32 Debug\"\n"
        "# PROP Output_Dir \"Debug\"\n"
        "!ENDIF\n";
    const auto dll = parseDsp(dllDsp);
    ASSERT_TRUE(dll.has_value());
    EXPECT_EQ(dll->configurations[0].kind, DspTargetKind::DynamicLibrary);
    EXPECT_EQ(dll->outputPathFor(dll->configurations[0]),
              QStringLiteral("E:/proj/Debug/lib.dll"));

    const char* libDsp =
        "# TARGTYPE \"Win32 (x86) Static Library\" 0x0104\n"
        "CFG=lib - Win32 Debug\n"
        "!IF  \"$(CFG)\" == \"lib - Win32 Debug\"\n"
        "# PROP Output_Dir \"Debug\"\n"
        "!ENDIF\n";
    const auto lib = parseDsp(libDsp);
    ASSERT_TRUE(lib.has_value());
    EXPECT_EQ(lib->configurations[0].kind, DspTargetKind::StaticLibrary);
    EXPECT_EQ(lib->outputPathFor(lib->configurations[0]),
              QStringLiteral("E:/proj/Debug/lib.lib"));
}

TEST(DspProject, DefaultConfigurationPrefersDebug)
{
    const auto project = parseDsp(kDspConsole);
    ASSERT_TRUE(project.has_value());
    // The file lists Release first; Debug wins by name, not by position.
    EXPECT_EQ(project->defaultConfiguration()->name,
              QStringLiteral("Debug"));
}

TEST(DspProject, GbkEncodedDspDecodesItsName)
{
    // Only meaningful where the local ANSI page really is GBK — the fixture
    // is the bytes VS6 writes on a zh-CN machine for a project called 项目1.
    if (LightCpp::Text::localAnsiEncoding()
        != LightCpp::Text::TextEncoding::Gbk) {
        GTEST_SKIP() << "local ANSI code page is not GBK";
    }
    QByteArray content =
        "# Microsoft Developer Studio Project File - Name=\"";
    content += QByteArrayLiteral("\xCF\xEE\xC4\xBF" "1");
    content += "\" - Package Owner=<4>\n"
               "# TARGTYPE \"Win32 (x86) Console Application\" 0x0103\n"
               "CFG=x - Win32 Debug\n"
               "!IF  \"$(CFG)\" == \"x - Win32 Debug\"\n"
               "# PROP Output_Dir \"Debug\"\n"
               "!ENDIF\n"
               "SOURCE=.\\main.cpp\n";

    lastError.clear();
    const auto project = parseDspProject(kDspPath, content, &lastError);
    ASSERT_TRUE(project.has_value()) << lastError.toStdString();
    // FromUtf8 would have produced U+FFFD mush here.
    EXPECT_EQ(project->name, QStringLiteral("\x9879\x76EE" "1"));
    EXPECT_EQ(project->outputPathFor(project->configurations[0]),
              QStringLiteral("E:/proj/Debug/") + QStringLiteral("\x9879\x76EE" "1")
                  + QStringLiteral(".exe"));
}

TEST(DspProject, RejectsFileWithoutConfigurations)
{
    lastError.clear();
    const auto project =
        parseDspProject(kDspPath, QByteArray("# a comment\nSOURCE=.\n"),
                        &lastError);
    EXPECT_FALSE(project.has_value());
    EXPECT_FALSE(lastError.isEmpty());
}

// --- .dsw -----------------------------------------------------------------

TEST(DswWorkspace, ListsMemberProjectsWithResolvedPaths)
{
    const char* content =
        "Microsoft Developer Studio Workspace File, Format Version 6.00\n"
        "# WARNING: DO NOT EDIT OR DELETE THIS WORKSPACE FILE!\n"
        "\n"
        "###############################################################################\n"
        "\n"
        "Project: \"alpha\"=\".\\alpha\\alpha.dsp\" - Package Owner=<4>\n"
        "Package=<5>\n"
        "{{{\n"
        "}}}\n"
        "\n"
        "###############################################################################\n"
        "\n"
        "Project: \"other\"=.\\sub\\other.dsp - Package Owner=<4>\n"
        "Package=<5>\n"
        "{{{\n"
        "}}}\n"
        "Package=<4>\n"
        "{{{\n"
        "}}}\n"
        "\n"
        "###############################################################################\n"
        "\n"
        "Global:\n"
        "\n"
        "Package=<5>\n"
        "{{{\n"
        "}}}\n"
        "Package=<3>\n"
        "{{{\n"
        "}}}\n"
        "###############################################################################\n"
        "\n";
    lastError.clear();
    const auto workspace =
        parseDswWorkspace(kDswPath, QByteArray(content), &lastError);
    ASSERT_TRUE(workspace.has_value()) << lastError.toStdString();
    ASSERT_EQ(workspace->projects.size(), 2);
    EXPECT_EQ(workspace->projects[0].name, QStringLiteral("alpha"));
    EXPECT_EQ(workspace->projects[0].dspPath,
              QStringLiteral("E:/proj/alpha/alpha.dsp"));  // quoted in file
    EXPECT_EQ(workspace->projects[1].name, QStringLiteral("other"));
    EXPECT_EQ(workspace->projects[1].dspPath,
              QStringLiteral("E:/proj/sub/other.dsp"));
    // No member matches the workspace's own name: the first one opens.
    EXPECT_EQ(workspace->defaultProject(), &workspace->projects[0]);
}

TEST(DswWorkspace, DefaultProjectPrefersWorkspaceName)
{
    const char* content =
        "Project: \"first\"=.\\first.dsp - Package Owner=<4>\n"
        "Project: \"hello\"=..\\hello\\hello.dsp - Package Owner=<4>\n";
    lastError.clear();
    const auto workspace =
        parseDswWorkspace(kDswPath, QByteArray(content), &lastError);
    ASSERT_TRUE(workspace.has_value());
    // "hello.dsp" matches the workspace's own stem even though it is
    // listed second and lives one directory up.
    EXPECT_EQ(workspace->defaultProject()->name, QStringLiteral("hello"));
    EXPECT_EQ(workspace->defaultProject()->dspPath,
              QStringLiteral("E:/hello/hello.dsp"));
}

// --- labels ---------------------------------------------------------------

TEST(DspProject, TargetLabels)
{
    EXPECT_EQ(dspTargetLabel(DspTargetKind::Application),
              QStringLiteral("Application"));
    EXPECT_EQ(dspTargetLabel(DspTargetKind::DynamicLibrary),
              QStringLiteral("Dynamic Library"));
    EXPECT_EQ(dspTargetLabel(DspTargetKind::StaticLibrary),
              QStringLiteral("Static Library"));
}

TEST(DspProject, LoadFromDisk)
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("t.dsp"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("# TARGTYPE \"Win32 (x86) Console Application\" 0x0103\n"
               "CFG=t - Win32 Debug\n"
               "!IF  \"$(CFG)\" == \"t - Win32 Debug\"\n"
               "# PROP Output_Dir \"Debug\"\n"
               "!ENDIF\n");
    file.close();

    lastError.clear();
    const auto project = loadDspProject(path, &lastError);
    ASSERT_TRUE(project.has_value()) << lastError.toStdString();
    EXPECT_EQ(project->name, QStringLiteral("t"));
    EXPECT_EQ(project->projectDir, QDir(dir.path()).absolutePath());
}

// --- target-section conditionals ----------------------------------------

TEST(DspProject, TargetSectionConditionalsMakeNoGhostConfiguration)
{
    // A hand-rolled .dsp that wraps SOURCE= lines in !IF inside
    // "# Begin Target": those conditionals belong to the file list, not to
    // the configuration model. The real configuration block sits before
    // "# Begin Target" and must still parse.
    const auto project = parseDsp(
        "# Microsoft Developer Studio Project File - Name=\"hello\" - "
        "Package Owner=<4>\n"
        "# TARGTYPE \"Win32 (x86) Console Application\" 0x0103\n"
        "CFG=hello - Win32 Debug\n"
        "!IF  \"$(CFG)\" == \"hello - Win32 Debug\"\n"
        "# PROP Output_Dir \"Debug\"\n"
        "# ADD CPP /D \"WIN32\"\n"
        "!ENDIF \n"
        "# Begin Target\n"
        "# Name \"hello - Win32 Debug\"\n"
        "!IF  \"$(CFG)\" == \"hello - Win32 Debug\"\n"
        "SOURCE=.\\main.cpp\n"
        "!ELSE\n"
        "SOURCE=.\\stub.cpp\n"
        "!ENDIF \n"
        "# End Target\n");
    ASSERT_TRUE(project.has_value()) << lastError.toStdString();
    // Exactly the one real configuration - the in-target !IF used to add
    // "hello - Win32 Debug" a second time as a ghost.
    ASSERT_EQ(project->configurations.size(), 1);
    EXPECT_EQ(project->configurations.first().name,
              QStringLiteral("Debug"));
    // Both conditional variants of the file list are still collected.
    EXPECT_EQ(project->sourceFiles.size(), 2);
}

