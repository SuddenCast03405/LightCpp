// BuildOptions — the build settings the IDE has to *construct* rather than
// pass through (lightcpp.1.md §三: cross-compilation toolchain, ccache,
// targets, install/package, the compile-command view, and the defines /
// includes / libraries / flags page).
//
// The gap this closes is specific. A CMake build has three layers of settings
// that a GUI must not confuse:
//
//   * what the toolchain file says (compiler, sysroot, target system) — the
//     only place cross-compilation can be expressed;
//   * what the cache variables say (-DCMAKE_CXX_STANDARD, -DVCPKG_TARGET_…);
//   * what a single translation unit's command line says (the flags in
//     compile_commands.json), which is what "show me the compile command"
//     must display — a project's real flags, not the IDE's guess.
//
// Everything is a pure function, so the spellings are testable, and the
// toolchain-file generator is checked against a real cross build.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::Build {

// ---------------------------------------------------------------------------
// Compile options (defines / includes / libraries / flags)
// ---------------------------------------------------------------------------

/// The four lists of the "Compiler options" page, in the order MSVC and the
/// GNU drivers both accept them.
struct CompileOptions {
    QStringList defines;       ///< `NAME` or `NAME=VALUE`, without -D
    QStringList includePaths;  ///< directories, without -I
    QStringList libraries;     ///< `fmt` or `/path/to/libfmt.a`, without -l
    QStringList libraryPaths;  ///< directories, without -L
    QStringList flags;         ///< everything else, verbatim
    QString cStandard;         ///< "c17"; empty = the project's own
    QString cppStandard;       ///< "c++20"; empty = the project's own

    [[nodiscard]] bool isEmpty() const;
    /// GCC/Clang spelling.
    [[nodiscard]] QStringList toArguments() const;
    /// MSVC spelling (`/D`, `/I`, `/LIBPATH:`, `/std:`).
    [[nodiscard]] QStringList toMsvcArguments() const;
    /// CMake cache variables that express the same thing, for a configure
    /// step rather than a compiler invocation.
    [[nodiscard]] QStringList toCMakeArguments() const;
};

/// Recover the four lists from a compiler command line (the `command` member
/// of one compile_commands.json entry). Used by the "show compile command"
/// view so it can highlight the parts instead of showing one long string.
///
/// The parser understands the quoting rules of CMake's generator output and
/// of a shell; an argument it cannot place goes into `flags` rather than being
/// dropped.
[[nodiscard]] CompileOptions parseCompileCommand(const QStringList& arguments);

/// Split a command string into arguments, honouring single and double quotes
/// and backslash escapes — the form compile_commands.json stores.
[[nodiscard]] QStringList splitCommandLine(const QString& command);

// ---------------------------------------------------------------------------
// Cross compilation
// ---------------------------------------------------------------------------

/// A CMake toolchain file's content, as data.
struct CrossToolchain {
    QString name;              ///< display name, e.g. "aarch64-linux-gnu"
    QString systemName;        ///< CMAKE_SYSTEM_NAME, e.g. "Linux"
    QString systemVersion;     ///< CMAKE_SYSTEM_VERSION, optional
    QString processor;         ///< CMAKE_SYSTEM_PROCESSOR, e.g. "aarch64"
    QString cCompiler;         ///< absolute path or a name on PATH
    QString cxxCompiler;
    QString ar;
    QString ranlib;
    QString strip;
    QString sysroot;
    /// "ONLY" (default), "NEVER", or empty for CMake's default.
    QString findRootPathMode;

    /// A toolchain file needs at least a system name and a C/C++ compiler;
    /// anything less and CMake quietly falls back to the host compiler, which
    /// looks like a successful configure and produces host binaries.
    [[nodiscard]] bool isValid() const;
    /// The problems, in the order the user should fix them.
    [[nodiscard]] QStringList problems() const;
    /// The `.cmake` file content.
    [[nodiscard]] QString toCMakeToolchainFile() const;
};

/// The cross toolchains found next to `compilerPath` (its own directory, then
/// `../bin`), inferred from the `triplet-` prefix a cross compiler carries
/// (`aarch64-linux-gnu-g++`). Returns a candidate per prefix.
[[nodiscard]] QList<CrossToolchain> inferToolchains(const QString& compilerPath);

/// Fill in the pieces a user typically leaves alone (ar/ranlib/strip from the
/// same prefix, sysroot from the compiler's own default when it names one).
[[nodiscard]] CrossToolchain completeToolchain(CrossToolchain toolchain);

/// `-DCMAKE_TOOLCHAIN_FILE=<path>` plus the cache variables a cross build
/// needs regardless of the file.
[[nodiscard]] QStringList toolchainArguments(const QString& toolchainFile,
                                             const CrossToolchain& toolchain);

/// Write `toolchain` to `path`, creating the parent directory. Returns false
/// and fills `error` when the file cannot be written.
[[nodiscard]] bool writeToolchainFile(const QString& path,
                                      const CrossToolchain& toolchain,
                                      QString* error = nullptr);

// ---------------------------------------------------------------------------
// Compiler launchers (ccache / sccache)
// ---------------------------------------------------------------------------

/// One found launcher.
struct CompilerLauncher {
    QString program;   ///< "ccache", "sccache"
    QString path;      ///< absolute path when found
    bool found = false;

    [[nodiscard]] bool isValid() const { return found && !path.isEmpty(); }
};

/// Look for `ccache` and `sccache` in `searchPaths` (then on PATH when the
/// list is empty). Returns both, in that order, with `found` set per entry.
[[nodiscard]] QList<CompilerLauncher> detectLaunchers(const QStringList& searchPaths);

/// The CMake cache arguments that route the compiler through `launcher`:
/// `-DCMAKE_C_COMPILER_LAUNCHER=…` and `-DCMAKE_CXX_COMPILER_LAUNCHER=…`.
[[nodiscard]] QStringList launcherArguments(const CompilerLauncher& launcher);

/// `CCACHE_DIR`/`SCCACHE_DIR` and the size limit, as environment additions.
struct LauncherEnvironment {
    QString cacheDirectory;
    QString maxSize;      ///< "5G", "500M"; empty = leave the tool's default
};
[[nodiscard]] QList<QPair<QString, QString>> launcherEnvironment(
    const CompilerLauncher& launcher, const LauncherEnvironment& settings);

/// `ccache --show-stats` / `sccache --show-stats` arguments.
[[nodiscard]] QStringList launcherStatsArguments(const CompilerLauncher& launcher);

/// One line from the stats output: "<label>: <value>", or empty when the
/// output does not contain the hit-rate line.
[[nodiscard]] QString parseLauncherHitRate(const QString& output);

// ---------------------------------------------------------------------------
// Targets and build commands
// ---------------------------------------------------------------------------

/// Parse `cmake --build <dir> --target help`.
///
/// CMake prints a sentence per generator. Make says
/// "The following are some of the valid targets for this Makefile:" then
/// "... all (the default if no target is provided)"; Ninja prints
/// "... target"; MSBuild prints "The following targets are available:" then
/// lines with the target name. The parser takes every line that starts with
/// "... " or is listed under that header, and drops the generator's own
/// bookkeeping targets.
[[nodiscard]] QStringList parseTargetList(const QString& helpText);

/// Targets that are not worth a menu entry: they are bookkeeping, not builds.
[[nodiscard]] bool isBookkeepingTarget(const QString& target);

/// `--target <name>` for CMake, `/t:<name>` for MSBuild — as an argument list
/// for the configured generator.
[[nodiscard]] QStringList buildTargetArguments(const QString& target,
                                               bool useMsvc);
/// `--parallel [n]`.
[[nodiscard]] QStringList parallelArguments(bool parallel, int jobs);
/// `--config <cfg>` for a multi-config generator; empty for single-config.
[[nodiscard]] QStringList configArguments(const QString& config, bool multiConfig);
/// `--target install` — the build step.
[[nodiscard]] QStringList installArguments(const QString& prefix);

/// `-DCMAKE_INSTALL_PREFIX=…` — the configure step. CMake reads the prefix
/// only at configure time; passing it to `cmake --build` is silently ignored.
[[nodiscard]] QStringList installPrefixArguments(const QString& prefix);
/// `cpack -G <generator> -B <dir>` with the configurations to package.
[[nodiscard]] QStringList packageArguments(const QStringList& generators,
                                           const QString& outputDirectory);

/// A build log's file name: `<stem>-<timestamp>.log`, safe on every platform.
[[nodiscard]] QString buildLogFileName(const QString& stem,
                                       const QString& timestamp);

/// The command line a compile_commands.json entry holds for `filePath`,
/// matched by basename (the entry's `file` is absolute and so is the editor's
/// path; a stale database can still name the same file by a different case on
/// Windows).
struct CompileCommandEntry {
    QString file;
    QString directory;
    QString command;
    QStringList arguments;
    bool valid = false;
};

[[nodiscard]] CompileCommandEntry compileCommandFor(const QString& compileCommandsJson,
                                                    const QString& filePath);

/// The command line of `entry` as one string, whatever form the database used
/// (`command` or `arguments`).
[[nodiscard]] QString compileCommandText(const CompileCommandEntry& entry);

}  // namespace LightCpp::Workbench::Build
