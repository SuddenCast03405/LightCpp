#pragma once

#include <QString>
#include <QStringList>

#include <functional>

/// Paths to the external tools this IDE drives.
///
/// One place rather than one per file: the compiler, language server and
/// formatter are resolved from the active toolchain or the host environment.
///
/// These are **fallbacks**. A kit names the tools first — every build and run
/// site in `MainWindow` asks `activeKit()` before it asks this file — and a
/// setting names them second (`SettingsStore::clangdPath()` and friends, whose
/// fallback is whatever `MainWindow` passed to `setFallbackToolPaths()`).
/// What is left for this file is the case where nothing is configured: a fresh
/// install, or a project whose kit says nothing about this machine.
///
/// In that case the answer has to be *found*. Windows installations are
/// discovered through MINGW_PREFIX/MSYS2_ROOT, an MSYS2 bash on PATH, or the
/// standard MSYS2 directory names on mounted drives; POSIX hosts use the
/// compiler on PATH. No drive letter, user directory or install prefix is
/// assumed.

namespace LightCpp::App::ToolchainPaths {

/// The first candidate the predicate accepts, else empty. Takes the predicate
/// instead of touching the filesystem so the *order* can be tested on a
/// machine with no MSYS2 at all; the real callers pass an existence check.
[[nodiscard]] QString firstWithTool(
    const QStringList& candidates,
    const std::function<bool(const QString&)>& hasTool);

/// The host compiler directory to build with when no kit or setting names
/// one. On Windows this can be a discovered MSYS2 prefix; on POSIX it is the
/// directory containing the compiler found on PATH.
///
/// Empty is a real answer and is not silently worked around: consumers hand
/// bare host tool names ("g++", "clang++", etc.) to QProcess, which resolves
/// them through the child's own PATH instead of inventing a machine-specific
/// location.
///
/// Resolved once per process (the search touches the filesystem).
[[nodiscard]] QString mingwBin();

/// clangd / clang-format / git: the copy inside `mingwBin()` when present,
/// else the one on PATH, else empty.
[[nodiscard]] QString clangdPath();
[[nodiscard]] QString clangFormatPath();
[[nodiscard]] QString gitPath();
/// The static analysis tools of §七: cppcheck, clang-tidy, scan-build. Empty
/// when the tool is not installed — the analysis panel skips it rather than
/// offering a button that cannot work.
[[nodiscard]] QString cppcheckPath();
[[nodiscard]] QString clangTidyPath();
[[nodiscard]] QString scanBuildPath();

}  // namespace LightCpp::App::ToolchainPaths
