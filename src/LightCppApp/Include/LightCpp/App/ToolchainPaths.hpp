#pragma once

#include <QString>
#include <QStringList>

#include <functional>

/// Paths to the external tools this IDE drives.
///
/// One place rather than one per file: the compiler, the language server and
/// the formatter all come out of the same MSYS2 install, and a second copy of
/// that path is a second thing to keep in step when it moves.
///
/// These are **fallbacks**. A kit names the tools first — every build and run
/// site in `MainWindow` asks `activeKit()` before it asks this file — and a
/// setting names them second (`SettingsStore::clangdPath()` and friends, whose
/// fallback is whatever `MainWindow` passed to `setFallbackToolPaths()`).
/// What is left for this file is the case where nothing is configured: a fresh
/// install, or a project whose kit says nothing about this machine.
///
/// In that case the answer has to be *found*. The constants this file used to
/// export answered `E:/msys64/ucrt64/bin` unconditionally — on a machine where
/// that directory does not exist, every consumer downstream failed with a path
/// the user had never chosen ("Compiler failed to start. Check that
/// E:/msys64/ucrt64/bin/g++.exe exists."). Every candidate below is checked
/// for the tool it is supposed to contain, and "not found" is an answer.
namespace LightCpp::App::ToolchainPaths {

/// The first candidate the predicate accepts, else empty. Takes the predicate
/// instead of touching the filesystem so the *order* can be tested on a
/// machine with no MSYS2 at all; the real callers pass an existence check.
[[nodiscard]] QString firstWithTool(
    const QStringList& candidates,
    const std::function<bool(const QString&)>& hasTool);

/// The bin directory to build with when no kit and no setting names one: the
/// first known MSYS2 layout that really holds `g++.exe`, else the directory
/// `g++` is on PATH from, else empty.
///
/// Empty is a real answer and it is not silently worked around: the consumers
/// then hand bare tool names ("g++.exe") to `QProcess`, which resolves them
/// through the child's own PATH, and messages say the tool was not found
/// instead of naming a directory nobody configured.
///
/// Resolved once per process (the search touches the filesystem).
[[nodiscard]] QString mingwBin();

/// clangd / clang-format / git: the copy inside `mingwBin()` when it is there
/// (the same prefix as the compiler the IDE builds with, so the versions agree
/// with each other), else the one on PATH, else empty.
[[nodiscard]] QString clangdPath();
[[nodiscard]] QString clangFormatPath();
[[nodiscard]] QString gitPath();

}  // namespace LightCpp::App::ToolchainPaths
