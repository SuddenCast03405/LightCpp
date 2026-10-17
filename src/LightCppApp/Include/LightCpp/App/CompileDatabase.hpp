// CompileDatabase — where the project's own compile_commands.json lives.
//
// clangd learns a project's include paths from a compilation database and from
// nowhere else, so finding the one CMake wrote is the difference between the
// project's own headers resolving and every include turning into a red
// squiggle. The directory is not fixed: CMake puts it in whatever build
// directory the user chose, and "cmake --preset" (which this IDE drives) puts
// it where the preset says — `build/<preset>` for the presets in this repo,
// `out/build/<preset>` for the layout CMake presets default to and VS uses.
//
// The search is breadth-first and stops at the first hit, so the shallowest
// database wins. Depth 3 is only walked beneath a directory that looks like a
// build root (`build`, `out`, `cmake-build-*`): walking every source directory
// three levels deep would stat thousands of directories on every language
// server start, and a compile_commands.json under `src/` is not a thing CMake
// produces.

#pragma once

#include <QString>

namespace LightCpp::App::CompileDatabase {

/// The directory holding `compile_commands.json` for the project rooted at
/// `rootPath`, or an empty string when the project has none. The result is an
/// absolute path; callers hand it to clangd as `--compile-commands-dir`.
[[nodiscard]] QString findProjectDirectory(const QString& rootPath);

}  // namespace LightCpp::App::CompileDatabase
