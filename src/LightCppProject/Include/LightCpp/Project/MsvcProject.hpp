#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace LightCpp::Project {

/// What a Microsoft C/C++ project produces, across the three file generations
/// this model parses. The .vcxproj spellings are "Application",
/// "DynamicLibrary", "StaticLibrary", "Makefile"; the .vcproj ones are the
/// numbers 1/2/3/0; a .dsp's TARGTYPE collapses console and GUI applications
/// into the same "an .exe comes out" bucket.
enum class MsvcTargetKind {
    Application,
    DynamicLibrary,
    StaticLibrary,
    Makefile,
};

/// One build configuration of an MSVC project, e.g. "Debug|Win32".
struct MsvcConfiguration {
    QString name;      // "Debug"
    QString platform;  // "Win32" / "x64"

    MsvcTargetKind kind = MsvcTargetKind::Application;

    /// Raw (unexpanded) property values, exactly as the file states them.
    /// .vcxproj: the conditioned PropertyGroup's OutDir/TargetName.
    /// .vcproj: the Configuration's OutputDirectory and the VCLinkerTool's
    /// OutputFile (empty when absent). .dsp mapping happens in DspProject.
    QString outDir;
    QString targetName;
    QString outputFile;

    /// Preprocessor definitions (/D) and include directories (/I) of this
    /// configuration, in file order.
    QStringList defines;
    QStringList includeDirs;
};

/// A parsed Microsoft C/C++ project — both XML generations:
///
///   * .vcxproj — the MSBuild format (VS2010 and every later release). Root
///     element `<Project>`, per-configuration properties in PropertyGroups
///     carrying a `Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'"`.
///   * .vcproj — the VS2005/2008 format. Root element
///     `<VisualStudioProject>`, a `<Configurations>` section, configuration
///     type as a digit on the Configuration element.
///
/// What is *kept* is what building and running need: configurations, the
/// source/header/resource split, and the output path with the project's own
/// macros expanded. Property sheets, imports and everything else a
/// hand-rolled parser cannot honour are simply not modelled — msbuild does
/// the real build, this model only has to predict where the result lands.
struct MsvcProject {
    QString filePath;    // the project file itself, absolute
    QString projectDir;  // its directory

    QString name;  // ProjectName / VisualStudioProject@Name / file stem

    /// false for the .vcxproj MSBuild format, true for the VS2005-2008 one.
    bool isLegacyVcproj = false;

    QList<MsvcConfiguration> configurations;

    /// Absolute paths, deduplicated across the whole project. Resource
    /// files are .rc scripts (compiled by rc.exe); anything neither
    /// compilable nor a header lands in otherFiles — including sources the
    /// file excludes from every configuration via ExcludedFromBuild, which
    /// stay visible in the tree without inflating the compile count.
    QStringList sourceFiles;
    QStringList headerFiles;
    QStringList resourceFiles;
    QStringList otherFiles;

    [[nodiscard]] bool isValid() const noexcept
    {
        return !configurations.isEmpty();
    }

    /// The configuration with the given build name ("Debug"), preferring
    /// Win32 over x64 over any other platform. nullptr when absent.
    [[nodiscard]] const MsvcConfiguration* configurationFor(
        const QString& name) const;

    /// The configuration a plain "build this" opens with: Debug when present,
    /// else Release, else the first one.
    [[nodiscard]] const MsvcConfiguration* defaultConfiguration() const;

    /// The produced file, macros expanded, as an absolute path. Empty for a
    /// Makefile project (what it produces is whatever the makefile says).
    /// `solutionDir` overrides $(SolutionDir) for projects opened through a
    /// .sln; empty keeps the single-project default (the project directory).
    [[nodiscard]] QString outputPathFor(
        const MsvcConfiguration& config, const QString& solutionDir = {}) const;
};

/// Expands the MSVC macros this model tracks, iteratively until stable:
/// $(ProjectDir), $(ProjectName), $(Configuration)/$(ConfigurationName),
/// $(Platform), $(SolutionDir) (the project directory unless `solutionDir`
/// is given — this IDE builds single projects, so the solution root is the
/// project root by default), $(OutDir), $(TargetName), $(TargetExt),
/// $(TargetFileName). Unknown macros expand to empty, which is what msbuild
/// itself does with them.
[[nodiscard]] QString expandMsvcMacros(const QString& value,
                                       const MsvcProject& project,
                                       const MsvcConfiguration& config,
                                       const QString& solutionDir = {});

/// What the target kind produces, for the build banner: "Application",
/// "Dynamic Library", "Static Library", "Makefile".
[[nodiscard]] QString msvcTargetLabel(MsvcTargetKind kind);

/// Parses in-memory `.vcxproj`/`.vcproj` content. Dispatches on the file's
/// own root element, not the extension, so a misnamed file still parses
/// correctly.
[[nodiscard]] std::optional<MsvcProject> parseMsvcProject(
    const QString& filePath, const QByteArray& content, QString* error = nullptr);

/// Reads and parses a `.vcxproj`/`.vcproj` file from disk.
[[nodiscard]] std::optional<MsvcProject> loadMsvcProject(
    const QString& filePath, QString* error = nullptr);

}  // namespace LightCpp::Project
