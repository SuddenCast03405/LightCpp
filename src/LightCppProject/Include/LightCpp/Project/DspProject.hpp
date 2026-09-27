#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace LightCpp::Project {

/// What a Visual Studio 6 (.dsp) project produces. The TARGTYPE numbers
/// distinguish console from GUI applications, but both end in an .exe, and
/// nothing here depends on the difference — so both collapse into
/// Application, exactly like the .vcxproj model treats them.
enum class DspTargetKind {
    Application,
    DynamicLibrary,
    StaticLibrary,
};

/// One build configuration of a VS6 project, e.g. "hello - Win32 Debug".
struct DspConfiguration {
    /// The full CFG name, the identifier msdev /make takes:
    /// "<project> - Win32 <name>".
    QString fullName;
    QString name;      // "Debug"
    QString platform;  // "Win32"

    DspTargetKind kind = DspTargetKind::Application;

    /// Raw `# PROP Output_Dir` value (usually "Debug" or "Release").
    QString outDir;
    /// An /out:"..." argument of the `# ADD LINK32` line, when the project
    /// overrides the default output name.
    QString outputFile;

    QStringList defines;      // /D of the `# ADD CPP` line
    QStringList includeDirs;  // /I of the `# ADD CPP` line
};

/// A parsed Visual Studio 6 project file (.dsp).
///
/// The format is a text file with a makefile-shaped middle: `!IF "$(CFG)"`
/// blocks per configuration, `# PROP`/`# ADD CPP`/`# ADD LINK32` lines, and
/// `# Begin Group` sections holding the file list as `SOURCE=` lines. What
/// is kept is what building and running need — configurations, the file
/// split, the output path — not the flags themselves; msdev does the real
/// build, this model only has to predict where the result lands and what
/// it is called.
struct DspProject {
    QString filePath;    // the .dsp file itself, absolute
    QString projectDir;  // its directory

    QString name;  // from the header line, falling back to the file stem

    QList<DspConfiguration> configurations;

    /// Absolute paths. The VS6 .rc/.bmp/.ico resources a "Resource Files"
    /// group holds land here; anything else non-compilable is "other".
    QStringList sourceFiles;
    QStringList headerFiles;
    QStringList resourceFiles;
    QStringList otherFiles;

    [[nodiscard]] bool isValid() const noexcept
    {
        return !configurations.isEmpty();
    }

    /// The configuration with the given name ("Debug"), case-insensitive.
    /// nullptr when absent.
    [[nodiscard]] const DspConfiguration* configurationFor(
        const QString& name) const;

    /// The configuration msdev would open with: the one the CFG= line
    /// names, else Debug, else Release, else the first.
    [[nodiscard]] const DspConfiguration* defaultConfiguration() const;

    /// The produced file, as an absolute path. Empty for the (rare) utility
    /// project shapes that produce nothing.
    [[nodiscard]] QString outputPathFor(const DspConfiguration& config) const;
};

/// One project listed in a .dsw workspace.
struct DswProjectEntry {
    QString name;  // the quoted name in the Project: line
    /// The .dsp path, resolved against the .dsw's directory.
    QString dspPath;
};

/// A parsed Visual Studio 6 workspace file (.dsw) — a list of .dsp paths
/// plus package boilerplate this model does not need.
struct DswWorkspace {
    QString filePath;    // the .dsw file itself, absolute
    QString projectDir;  // its directory

    QList<DswProjectEntry> projects;

    [[nodiscard]] bool isValid() const noexcept { return !projects.isEmpty(); }

    /// The entry to open when the user names the workspace: the one whose
    /// .dsp stem matches the .dsw's own name, else the first. (A workspace
    /// usually lives next to the project it was created with.)
    [[nodiscard]] const DswProjectEntry* defaultProject() const;
};

/// Parses in-memory `.dsp` content.
[[nodiscard]] std::optional<DspProject> parseDspProject(
    const QString& filePath, const QByteArray& content, QString* error = nullptr);

/// Reads and parses a `.dsp` file from disk.
[[nodiscard]] std::optional<DspProject> loadDspProject(
    const QString& filePath, QString* error = nullptr);

/// Parses in-memory `.dsw` content, resolving the listed .dsp paths against
/// the workspace's directory.
[[nodiscard]] std::optional<DswWorkspace> parseDswWorkspace(
    const QString& filePath, const QByteArray& content, QString* error = nullptr);

/// Reads and parses a `.dsw` file from disk.
[[nodiscard]] std::optional<DswWorkspace> loadDswWorkspace(
    const QString& filePath, QString* error = nullptr);

/// What the target kind produces, for the build banner: "Application",
/// "Dynamic Library", "Static Library".
[[nodiscard]] QString dspTargetLabel(DspTargetKind kind);

}  // namespace LightCpp::Project
