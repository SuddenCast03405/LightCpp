#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "LightCpp/Project/BuildConfiguration.hpp"

/// Bloodshed Dev-C++ project files (`*.dev`) — the INI-shaped descriptor that
/// the classic Dev-C++ IDE (and its wxDev-C++/Embarcadero descendants) writes.
///
/// Two conventions in this format are easy to get wrong and are handled
/// centrally here:
///
///   * Multi-value keys separate their entries with `_@@_`, not with spaces —
///     `Linker=D:\Qt\lib\libQtCore4.a_@@_D:\Qt\lib\libqtmain.a`. That is the
///     whole point of the marker: it lets an entry contain spaces.
///   * The unit list lives in `[UnitN]` sections, not only in the `UnitN=` keys
///     of `[Project]` (which is what *templates* use). Both are read.
///
/// Dev-C++ is a MinGW IDE, so a project from it is always built with the GNU
/// toolchain — through a generated makefile, the way the original IDE does it
/// (see devCppMakefilePlan()).
namespace LightCpp::Project {

/// Recognised `Type=` values that do not produce a program.
constexpr int kDevCppStaticLibrary = 2;
constexpr int kDevCppDynamicLibrary = 3;

/// One translation unit. `compile` / `link` mirror `Compile=` / `Link=`, which
/// Dev-C++ lets you clear to keep a file in the project tree without building
/// it (data files, headers). `compileCpp` mirrors the per-unit `CompileCpp=`
/// override (absent → the project's IsCpp); it picks $(CPP)/$(CXXFLAGS) versus
/// $(CC)/$(CFLAGS) in the generated makefile, exactly like the original IDE.
/// `overrideBuildCmd`/`buildCmd` mirror `OverrideBuildCmd=`/`BuildCmd=`: when
/// set, the makefile rule runs that command instead of the default one.
struct DevCppUnit {
    QString fileName;
    bool compile = true;
    bool link = true;
    bool compileCpp = true;
    bool overrideBuildCmd = false;
    QString buildCmd;
};

struct DevCppProject {
    QString filePath;    // the .dev file itself, absolute
    QString projectDir;  // its directory
    QString name;        // [Project] Name, falling back to the file stem
    bool isCpp = true;   // IsCpp=1 → C++; 0 → C
    int type = 0;        // [Project] Type (see kDevCpp* above)

    QStringList compilerFlags;   // [Project] CppCompiler / Compiler
    QStringList linkerFlags;     // [Project] Linker
    QStringList includeDirs;     // [Project] Includes
    QStringList libraryDirs;     // [Project] Libs
    QStringList defines;         // [Project] PreprocDefines

    QString exeOutput;           // [Project] ExeOutput — a relative directory
    QString objectOutput;        // [Project] ObjectOutput
    bool overrideOutput = false; // [Project] OverrideOutput
    QString overrideOutputName;  // [Project] OverrideOutputName

    bool useCustomMakefile = false;
    QString customMakefile;

    /// [Project] PrivateResource — an .rc script living inside the project
    /// that windres compiles and the build links in. Dev-C++ itself writes
    /// one (`<name>Private.rc`) whenever a project icon is set, so it wins
    /// over the Icon= field when both are present.
    QString privateResource;
    /// [Project] MakeIncludes — extra makefiles `include`d by the generated
    /// one (the hook Dev-C++ offers for all-before/all-after rules).
    QStringList makeIncludes;

    QString iconPath;            // [Project] Icon, resolved; empty when unset
    QStringList runArguments;    // [Project] CommandLine — passed when running

    QList<DevCppUnit> units;

    /// True when the file parsed and at least one unit is a compilable
    /// translation unit — a project listing only headers has nothing to build.
    [[nodiscard]] bool isValid() const noexcept;

    /// False for a static library: there is no program to run afterwards.
    [[nodiscard]] bool producesExecutable() const noexcept
    {
        return type != kDevCppStaticLibrary && type != kDevCppDynamicLibrary;
    }

    /// What the project produces, for the build banner.
    ///
    /// 0 and 1 are both reported as an application: sources disagree about
    /// which of them means "console" and which "GUI", and nothing here depends
    /// on it — a GUI subsystem comes from the `-mwindows` the user already put
    /// in Linker=, not from the type number.
    [[nodiscard]] QString targetLabel() const;

    /// Absolute paths of the units that take part in the build.
    [[nodiscard]] QStringList buildUnitPaths() const;

    /// Directory the linked output goes to (ExeOutput resolved, else the
    /// project directory).
    [[nodiscard]] QString outputDirectory() const;

    /// The program or library this project produces.
    [[nodiscard]] QString outputPath() const;

    /// The makefile configured via UseCustomMakefile=1, or empty when the
    /// build should use the generated one (see devCppMakefilePlan()).
    [[nodiscard]] QString makefilePath() const;

    /// Where the compiled icon object is parked (kept out of the source tree).
    [[nodiscard]] QString iconObjectPath() const;

    /// The generated resource script (see iconResourceScript).
    [[nodiscard]] QString iconResourcePath() const;

    /// The resource script fed to windres. Dev-C++ stores the icon as a bare
    /// .ico path, and windres wants a script, so one is generated next to the
    /// object file rather than written into the project.
    [[nodiscard]] QByteArray iconResourceScript() const;
};

/// Parses `.dev` content already in memory.
[[nodiscard]] std::optional<DevCppProject> parseDevProject(
    const QString& filePath, const QByteArray& content, QString* error = nullptr);

/// Reads and parses a `.dev` file from disk.
[[nodiscard]] std::optional<DevCppProject> loadDevProject(
    const QString& filePath, QString* error = nullptr);

/// Compiles and links every unit in one compiler invocation.
///
/// Kept for the "syntax-check shaped" callers and the tests: the project
/// *build* now goes through a generated makefile (see devCppMakefilePlan()),
/// which is what the original IDE does — one g++ line over the whole unit
/// list cannot express per-unit flags, object output directories, or the
/// incremental behaviour make gives for free. `iconObject` is appended to the
/// inputs when it is not empty.
[[nodiscard]] QStringList gnuBuildArguments(const DevCppProject& project,
                                            BuildConfiguration config,
                                            const QString& compiler,
                                            const QString& outputPath,
                                            const QString& iconObject = QString());

/// `windres` command line turning the project icon into a linkable object.
[[nodiscard]] QStringList windresArguments(const DevCppProject& project,
                                           const QString& objectFile);

/// `mingw32-make` command line for UseCustomMakefile=1.
[[nodiscard]] QStringList makeArguments(const DevCppProject& project);

/// The GNU tools the generated makefile names. The IDE fills these from the
/// active kit; empty entries degrade to bare names ("g++", "windres.exe").
struct DevCppToolchain {
    QString gxx;      // C++ compiler (CPP=)
    QString gcc;      // C compiler (CC=); empty → derived from `gxx`
    QString windres;  // resource compiler; empty → derived from `gxx`
};

/// What building a .dev project through make needs on disk.
struct DevCppMakefilePlan {
    /// <projectDir>/Makefile.win — the same name and place Dev-C++ uses.
    QString path;
    /// The makefile bytes to write. Dev-C++'s makefile is read by
    /// mingw32-make byte-for-byte, so non-ASCII paths are encoded in the
    /// machine's ANSI code page (GBK on zh-CN), exactly like the original.
    /// A non-ASCII *output* name additionally routes the link/archive rule
    /// through an ASCII intermediate plus `cmd /c move`: recent MSYS2
    /// binutils decode ANSI bytes as UTF-8 and would land the file as
    /// U+FFFD mush.
    QByteArray content;
    /// When the project carries an Icon= and no PrivateResource=, the .rc
    /// script windres compiles is generated in temp — this is its content,
    /// and the caller must write it to iconResourcePath() before make runs.
    QByteArray resourceScript;
};

/// Builds the Dev-C++ style makefile for `project` — the same variable
/// layout (CPP/CC/WINDRES/RES/OBJ/LINKOBJ/LIBS/INCS/BIN/CXXFLAGS/CFLAGS),
/// the same relative paths anchored at the project directory, and the same
/// per-unit rules the original IDE's Compiler.pas writes, so the file it
/// produces is interchangeable with a Dev-C++ one.
[[nodiscard]] DevCppMakefilePlan devCppMakefilePlan(
    const DevCppProject& project, BuildConfiguration config,
    const DevCppToolchain& tools);

/// Writes the makefile (and the generated icon script, when present) to
/// disk. Returns false with `error` filled when a write fails.
[[nodiscard]] bool writeDevCppMakefile(const DevCppMakefilePlan& plan,
                                       const DevCppProject& project,
                                       QString* error = nullptr);

/// Splits a `_@@_`-separated list, dropping empty entries.
[[nodiscard]] QStringList splitDevList(const QString& value);

/// Splits a flag string into individual arguments.
///
/// `_@@_` wins when present, because that is the format's own way of keeping a
/// value with spaces in one piece. Only when there is no separator at all does
/// it fall back to splitting on whitespace (honouring double quotes), which is
/// what hand-written files from the 4.x era look like.
[[nodiscard]] QStringList splitFlagString(const QString& value);

}  // namespace LightCpp::Project
