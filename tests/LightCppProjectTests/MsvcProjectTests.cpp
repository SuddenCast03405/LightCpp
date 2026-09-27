// MsvcProjectTests — reading Microsoft C/C++ project files (.vcxproj and
// .vcproj).
//
// The fixtures are written out as literal XML in the exact shapes Visual
// Studio produces, so each parser assumption (conditioned PropertyGroups
// after the ProjectConfigurations group, the `==` in Condition matching,
// VCLinkerTool OutputFile as the output authority, semicolon lists) is
// pinned by an input that says which shape it is testing.

#include "LightCpp/Project/MsvcProject.hpp"
#include "LightCpp/Project/ProjectText.hpp"

#include <gtest/gtest.h>

#include "LightCpp/Text/Encoding.hpp"

namespace {

using LightCpp::Project::MsvcConfiguration;
using LightCpp::Project::MsvcProject;
using LightCpp::Project::MsvcTargetKind;
using LightCpp::Project::expandMsvcMacros;
using LightCpp::Project::msvcTargetLabel;
using LightCpp::Project::parseMsvcProject;

const QString kVcxprojPath = QStringLiteral("E:/proj/hello.vcxproj");
const QString kVcprojPath = QStringLiteral("E:/proj/hello.vcproj");

/// parseMsvcProject fills `error` only on failure; keeping the last one
/// lets the ASSERTs stream the actual parse error instead of nothing.
QString lastError;

std::optional<MsvcProject> parseVcxproj(const char* content)
{
    lastError.clear();
    return parseMsvcProject(kVcxprojPath, QByteArray(content), &lastError);
}

std::optional<MsvcProject> parseVcproj(const char* content)
{
    lastError.clear();
    return parseMsvcProject(kVcprojPath, QByteArray(content), &lastError);
}

/// A VS2022-shaped .vcxproj: the ProjectConfigurations group first, then
/// Globals, then one conditioned PropertyGroup per configuration, then the
/// file groups. OutDir references $(SolutionDir)$(Configuration) and the
/// TargetName is left to the default $(ProjectName).
const char* kVcxprojModern =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Project DefaultTargets=\"Build\" ToolsVersion=\"17.0\" "
    "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
    "  <ItemGroup Label=\"ProjectConfigurations\">\n"
    "    <ProjectConfiguration Include=\"Debug|Win32\">\n"
    "      <Configuration>Debug</Configuration>\n"
    "      <Platform>Win32</Platform>\n"
    "    </ProjectConfiguration>\n"
    "    <ProjectConfiguration Include=\"Release|x64\">\n"
    "      <Configuration>Release</Configuration>\n"
    "      <Platform>x64</Platform>\n"
    "    </ProjectConfiguration>\n"
    "  </ItemGroup>\n"
    "  <PropertyGroup Label=\"Globals\">\n"
    "    <ProjectGuid>{11111111-2222-3333-4444-555555555555}</ProjectGuid>\n"
    "    <RootNamespace>hello</RootNamespace>\n"
    "  </PropertyGroup>\n"
    "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n"
    "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|Win32'\" "
    "Label=\"Configuration\">\n"
    "    <ConfigurationType>Application</ConfigurationType>\n"
    "    <UseDebugLibraries>true</UseDebugLibraries>\n"
    "  </PropertyGroup>\n"
    "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\" "
    "Label=\"Configuration\">\n"
    "    <ConfigurationType>DynamicLibrary</ConfigurationType>\n"
    "  </PropertyGroup>\n"
    "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|Win32'\">\n"
    "    <OutDir>$(SolutionDir)$(Configuration)\\</OutDir>\n"
    "  </PropertyGroup>\n"
    "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n"
    "    <OutDir>$(SolutionDir)x64\\$(Configuration)\\</OutDir>\n"
    "    <TargetName>hello_native</TargetName>\n"
    "  </PropertyGroup>\n"
    "  <ItemGroup>\n"
    "    <ClCompile Include=\"main.cpp\" />\n"
    "    <ClCompile Include=\"..\\shared\\helper.cpp\" />\n"
    "    <ClInclude Include=\"hello.h\" />\n"
    "    <ResourceCompile Include=\"res\\app.rc\" />\n"
    "    <None Include=\"notes.txt\" />\n"
    "  </ItemGroup>\n"
    "</Project>\n";

/// A VS2008-shaped .vcproj: numeric ConfigurationType, OutputDirectory on
/// the Configuration element, VCLinkerTool OutputFile carrying the output,
/// and a nested Filter tree for the files.
const char* kVcproj2008 =
    "<?xml version=\"1.0\" encoding=\"Windows-1252\"?>\n"
    "<VisualStudioProject\n"
    "  ProjectType=\"Visual C++\"\n"
    "  Version=\"9.00\"\n"
    "  Name=\"hello\">\n"
    "  <Configurations>\n"
    "    <Configuration\n"
    "      Name=\"Debug|Win32\"\n"
    "      OutputDirectory=\"$(SolutionDir)$(ConfigurationName)\"\n"
    "      IntermediateDirectory=\"$(ConfigurationName)\"\n"
    "      ConfigurationType=\"1\">\n"
    "      <Tool Name=\"VCCLCompilerTool\"\n"
    "            PreprocessorDefinitions=\"WIN32;_DEBUG;_CONSOLE\"\n"
    "            AdditionalIncludeDirectories=\".\\include;..\\common\" />\n"
    "      <Tool Name=\"VCLinkerTool\"\n"
    "            OutputFile=\"$(OutDir)\\$(ProjectName).exe\" />\n"
    "    </Configuration>\n"
    "    <Configuration\n"
    "      Name=\"Release|Win32\"\n"
    "      OutputDirectory=\"$(ConfigurationName)\"\n"
    "      ConfigurationType=\"2\">\n"
    "      <Tool Name=\"VCCLCompilerTool\" />\n"
    "    </Configuration>\n"
    "  </Configurations>\n"
    "  <Files>\n"
    "    <Filter Name=\"Source Files\" Filter=\"cpp;cxx\">\n"
    "      <File RelativePath=\".\\main.cpp\" />\n"
    "    </Filter>\n"
    "    <Filter Name=\"Header Files\" Filter=\"h\">\n"
    "      <File RelativePath=\".\\hello.h\" />\n"
    "    </Filter>\n"
    "  </Files>\n"
    "</VisualStudioProject>\n";

}  // namespace

// --- .vcxproj --------------------------------------------------------------

TEST(MsvcProject, ParsesConfigurationsFromModernVcxproj)
{
    const auto project = parseVcxproj(kVcxprojModern);
    ASSERT_TRUE(project.has_value()) << lastError.toStdString();

    EXPECT_EQ(project->name, QStringLiteral("hello"));  // from RootNamespace
    ASSERT_EQ(project->configurations.size(), 2);
    EXPECT_FALSE(project->isLegacyVcproj);

    const MsvcConfiguration& debug = project->configurations[0];
    EXPECT_EQ(debug.name, QStringLiteral("Debug"));
    EXPECT_EQ(debug.platform, QStringLiteral("Win32"));
    EXPECT_EQ(debug.kind, MsvcTargetKind::Application);
    EXPECT_EQ(debug.outDir, QStringLiteral("$(SolutionDir)$(Configuration)\\"));

    const MsvcConfiguration& release = project->configurations[1];
    EXPECT_EQ(release.name, QStringLiteral("Release"));
    EXPECT_EQ(release.platform, QStringLiteral("x64"));
    EXPECT_EQ(release.kind, MsvcTargetKind::DynamicLibrary);
    EXPECT_EQ(release.targetName, QStringLiteral("hello_native"));
}

TEST(MsvcProject, ClassifiesFilesIntoSlots)
{
    const auto project = parseVcxproj(kVcxprojModern);
    ASSERT_TRUE(project.has_value());

    EXPECT_EQ(project->sourceFiles,
              (QStringList{QStringLiteral("E:/proj/main.cpp"),
                           QStringLiteral("E:/shared/helper.cpp")}));
    EXPECT_EQ(project->headerFiles,
              (QStringList{QStringLiteral("E:/proj/hello.h")}));
    EXPECT_EQ(project->resourceFiles,
              (QStringList{QStringLiteral("E:/proj/res/app.rc")}));
    EXPECT_EQ(project->otherFiles,
              (QStringList{QStringLiteral("E:/proj/notes.txt")}));
}

TEST(MsvcProject, ExpandsOutputPathThroughSolutionDir)
{
    const auto project = parseVcxproj(kVcxprojModern);
    ASSERT_TRUE(project.has_value());

    // $(SolutionDir) is the project dir for a single project, with the
    // trailing separator msbuild's own SolutionDir carries.
    EXPECT_EQ(project->outputPathFor(project->configurations[0]),
              QStringLiteral("E:/proj/Debug/hello.exe"));
    // Release defines its own TargetName and lands under the x64 tree.
    EXPECT_EQ(project->outputPathFor(project->configurations[1]),
              QStringLiteral("E:/proj/x64/Release/hello_native.dll"));
}

TEST(MsvcProject, ConditionedPropertyGroupsMatchByConfiguration)
{
    // The ProjectConfigurations group deliberately comes *after* the
    // conditioned PropertyGroups: the parser must not depend on order.
    const char* reordered =
        "<?xml version=\"1.0\"?>\n"
        "<Project>\n"
        "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|Win32'\">\n"
        "    <OutDir>$(SolutionDir)bin\\</OutDir>\n"
        "  </PropertyGroup>\n"
        "  <ItemGroup Label=\"ProjectConfigurations\">\n"
        "    <ProjectConfiguration Include=\"Debug|Win32\">\n"
        "      <Configuration>Debug</Configuration>\n"
        "      <Platform>Win32</Platform>\n"
        "    </ProjectConfiguration>\n"
        "  </ItemGroup>\n"
        "</Project>\n";
    const auto project = parseVcxproj(reordered);
    ASSERT_TRUE(project.has_value());
    ASSERT_EQ(project->configurations.size(), 1);
    EXPECT_EQ(project->configurations[0].outDir,
              QStringLiteral("$(SolutionDir)bin\\"));
    EXPECT_EQ(project->outputPathFor(project->configurations[0]),
              QStringLiteral("E:/proj/bin/hello.exe"));
}

TEST(MsvcProject, ConditionMatchingIgnoresOtherConfigurations)
{
    const auto project = parseVcxproj(kVcxprojModern);
    ASSERT_TRUE(project.has_value());
    // The Debug|Win32 conditioned group must not have leaked its OutDir into
    // Release|x64, and vice versa.
    EXPECT_EQ(project->configurations[0].outDir,
              QStringLiteral("$(SolutionDir)$(Configuration)\\"));
    EXPECT_EQ(project->configurations[1].outDir,
              QStringLiteral("$(SolutionDir)x64\\$(Configuration)\\"));
}

TEST(MsvcProject, ConfigurationPrefersWin32OverX64)
{
    const auto project = parseVcxproj(kVcxprojModern);
    ASSERT_TRUE(project.has_value());

    // "Debug" only exists as Win32, "Release" only as x64 — each resolves.
    EXPECT_EQ(project->configurationFor(QStringLiteral("Debug")),
              &project->configurations[0]);
    EXPECT_EQ(project->configurationFor(QStringLiteral("release")),
              &project->configurations[1]);  // case-insensitive name

    // Both platforms present: Win32 wins.
    MsvcProject both;
    both.filePath = kVcxprojPath;
    both.projectDir = QStringLiteral("E:/proj");
    both.name = QStringLiteral("hello");
    MsvcConfiguration debugWin;
    debugWin.name = QStringLiteral("Debug");
    debugWin.platform = QStringLiteral("Win32");
    MsvcConfiguration debugX64;
    debugX64.name = QStringLiteral("Debug");
    debugX64.platform = QStringLiteral("x64");
    both.configurations.append(debugWin);
    both.configurations.append(debugX64);
    EXPECT_EQ(both.configurationFor(QStringLiteral("Debug")), &both.configurations[0]);
    EXPECT_EQ(both.defaultConfiguration(), &both.configurations[0]);
}

TEST(MsvcProject, DefaultConfigurationPrefersDebugThenRelease)
{
    MsvcProject project;
    project.filePath = kVcxprojPath;
    project.projectDir = QStringLiteral("E:/proj");
    MsvcConfiguration release;
    release.name = QStringLiteral("Release");
    release.platform = QStringLiteral("Win32");
    release.kind = MsvcTargetKind::StaticLibrary;
    MsvcConfiguration debug;
    debug.name = QStringLiteral("Debug");
    debug.platform = QStringLiteral("x64");
    project.configurations.append(release);
    project.configurations.append(debug);
    EXPECT_EQ(project.defaultConfiguration(), &project.configurations[1]);

    MsvcProject onlyRelease;
    onlyRelease.filePath = kVcxprojPath;
    onlyRelease.projectDir = QStringLiteral("E:/proj");
    onlyRelease.configurations.append(release);
    EXPECT_EQ(onlyRelease.defaultConfiguration(), &onlyRelease.configurations[0]);
}

TEST(MsvcProject, StaticLibraryProducesALib)
{
    const char* content =
        "<?xml version=\"1.0\"?>\n"
        "<Project>\n"
        "  <ItemGroup Label=\"ProjectConfigurations\">\n"
        "    <ProjectConfiguration Include=\"Debug|Win32\">\n"
        "      <Configuration>Debug</Configuration>\n"
        "      <Platform>Win32</Platform>\n"
        "    </ProjectConfiguration>\n"
        "  </ItemGroup>\n"
        "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|Win32'\">\n"
        "    <ConfigurationType>StaticLibrary</ConfigurationType>\n"
        "    <OutDir>lib\\</OutDir>\n"
        "    <TargetName>$(ProjectName)s</TargetName>\n"
        "  </PropertyGroup>\n"
        "</Project>\n";
    const auto project = parseVcxproj(content);
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->outputPathFor(project->configurations[0]),
              QStringLiteral("E:/proj/lib/hellos.lib"));
}

TEST(MsvcProject, MakefileProjectHasNoOutputPath)
{
    const char* content =
        "<?xml version=\"1.0\"?>\n"
        "<Project>\n"
        "  <ItemGroup Label=\"ProjectConfigurations\">\n"
        "    <ProjectConfiguration Include=\"Debug|Win32\">\n"
        "      <Configuration>Debug</Configuration>\n"
        "      <Platform>Win32</Platform>\n"
        "    </ProjectConfiguration>\n"
        "  </ItemGroup>\n"
        "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|Win32'\">\n"
        "    <ConfigurationType>Makefile</ConfigurationType>\n"
        "  </PropertyGroup>\n"
        "</Project>\n";
    const auto project = parseVcxproj(content);
    ASSERT_TRUE(project.has_value());
    EXPECT_TRUE(project->outputPathFor(project->configurations[0]).isEmpty());
}

TEST(MsvcProject, UnknownMacrosExpandToEmpty)
{
    const auto project = parseVcxproj(kVcxprojModern);
    ASSERT_TRUE(project.has_value());

    const QString expanded = expandMsvcMacros(
        QStringLiteral("$(NotAKnownMacro)$(OutDir)$(ProjectName).exe"),
        *project, project->configurations[0]);
    // $(NotAKnownMacro) becomes empty exactly like msbuild does, $(OutDir)
    // expands through its own definition, and the loop bottoms out.
    EXPECT_EQ(expanded, QStringLiteral("E:/proj/Debug/hello.exe"));
}

// --- .vcproj --------------------------------------------------------------

TEST(MsvcProject, ParsesLegacyVcproj)
{
    const auto project = parseVcproj(kVcproj2008);
    ASSERT_TRUE(project.has_value()) << lastError.toStdString();

    EXPECT_EQ(project->name, QStringLiteral("hello"));  // VisualStudioProject@Name
    EXPECT_TRUE(project->isLegacyVcproj);
    ASSERT_EQ(project->configurations.size(), 2);

    const MsvcConfiguration& debug = project->configurations[0];
    EXPECT_EQ(debug.name, QStringLiteral("Debug"));
    EXPECT_EQ(debug.platform, QStringLiteral("Win32"));
    EXPECT_EQ(debug.kind, MsvcTargetKind::Application);
    EXPECT_EQ(debug.defines,
              (QStringList{QStringLiteral("WIN32"), QStringLiteral("_DEBUG"),
                           QStringLiteral("_CONSOLE")}));
    EXPECT_EQ(debug.includeDirs,
              (QStringList{QStringLiteral("./include"), QStringLiteral("../common")}));

    // ConfigurationType="2" is a DLL.
    EXPECT_EQ(project->configurations[1].kind, MsvcTargetKind::DynamicLibrary);

    // The linker's OutputFile is the output authority.
    EXPECT_EQ(project->outputPathFor(debug),
              QStringLiteral("E:/proj/Debug/hello.exe"));
}

TEST(MsvcProject, LegacyVcprojFileNesting)
{
    const auto project = parseVcproj(kVcproj2008);
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->sourceFiles,
              (QStringList{QStringLiteral("E:/proj/main.cpp")}));
    EXPECT_EQ(project->headerFiles,
              (QStringList{QStringLiteral("E:/proj/hello.h")}));
}

// --- errors ---------------------------------------------------------------

TEST(MsvcProject, RejectsNonProjectXml)
{
    QString error;
    const auto project =
        parseMsvcProject(kVcxprojPath,
                         QByteArray("<catalog><book/></catalog>"), &error);
    EXPECT_FALSE(project.has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(MsvcProject, RejectsMalformedXml)
{
    QString error;
    const auto project = parseMsvcProject(
        kVcxprojPath, QByteArray("<Project><unclosed>"), &error);
    EXPECT_FALSE(project.has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(MsvcProject, RejectsProjectWithoutConfigurations)
{
    QString error;
    const auto project = parseMsvcProject(
        kVcxprojPath, QByteArray("<Project><ItemGroup/></Project>"), &error);
    EXPECT_FALSE(project.has_value());
    EXPECT_FALSE(error.isEmpty());
}

// --- target labels --------------------------------------------------------

TEST(MsvcProject, TargetLabels)
{
    EXPECT_EQ(msvcTargetLabel(MsvcTargetKind::Application),
              QStringLiteral("Application"));
    EXPECT_EQ(msvcTargetLabel(MsvcTargetKind::DynamicLibrary),
              QStringLiteral("Dynamic Library"));
    EXPECT_EQ(msvcTargetLabel(MsvcTargetKind::StaticLibrary),
              QStringLiteral("Static Library"));
    EXPECT_EQ(msvcTargetLabel(MsvcTargetKind::Makefile),
              QStringLiteral("Makefile"));
}

// --- encoding -------------------------------------------------------------

TEST(MsvcProject, GbkEncodedVcprojDecodesItsName)
{
    // Only meaningful where the local ANSI page really is GBK — the fixture
    // is the bytes VS2008 writes on a zh-CN machine when the project is
    // called 项目1 (Name="项目1").
    if (LightCpp::Text::localAnsiEncoding()
        != LightCpp::Text::TextEncoding::Gbk) {
        GTEST_SKIP() << "local ANSI code page is not GBK";
    }
    QByteArray content =
        "<?xml version=\"1.0\" encoding=\"gb2312\"?>\n"
        "<VisualStudioProject Name=\"";
    content += QByteArrayLiteral("\xCF\xEE\xC4\xBF" "1");
    content += "\">\n"
               "  <Configurations>\n"
               "    <Configuration Name=\"Debug|Win32\" ConfigurationType=\"1\"/>\n"
               "  </Configurations>\n"
               "</VisualStudioProject>\n";

    const auto project = parseMsvcProject(kVcprojPath, content);
    ASSERT_TRUE(project.has_value()) << lastError.toStdString();
    // FromUtf8 would have produced U+FFFD mush here.
    EXPECT_EQ(project->name, QStringLiteral("\x9879\x76EE" "1"));
}
