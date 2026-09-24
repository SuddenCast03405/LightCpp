#pragma once

#include <QString>
#include <QStringList>

#include <optional>

#include "LightCpp/Project/BuildConfiguration.hpp"

/// CMake project support: find the project, lay out its build tree, and build
/// the command lines. Nothing here touches a process — the window owns the
/// QProcess and feeds it these arguments, which keeps the argument shapes
/// testable without a build actually running.
namespace LightCpp::Project {

/// A CMake project located on disk. `sourceDir` holds the CMakeLists.txt and
/// may be an ancestor of the file being edited.
struct CMakeProject {
    QString sourceDir;

    [[nodiscard]] bool isValid() const noexcept { return !sourceDir.isEmpty(); }
};

/// Walk up from `startDir` (inclusive) looking for a CMakeLists.txt.
///
/// `maxDepth` bounds the climb: without it a stray CMakeLists.txt in C:\ would
/// turn the whole drive into "the project". Stops at a `.git` or `.dev`
/// directory too — a repository root is a better guess at where a project
/// ends than a fixed depth.
[[nodiscard]] std::optional<CMakeProject> findCMakeProject(const QString& startDir,
                                                           int maxDepth = 6);

/// Build tree for one configuration: `<source>/build/<config>`.
///
/// A `build/` directory inside the project is the layout every CMake project
/// already expects (and already lists in .gitignore); keeping the two
/// configurations in separate trees is what CMake itself recommends, since a
/// cache belongs to exactly one build type.
[[nodiscard]] QString buildDirectory(const CMakeProject& project,
                                     BuildConfiguration config);

/// Arguments for the configure step. `generator` empty means "let CMake pick",
/// which is what you want when the chosen one is not installed.
[[nodiscard]] QStringList configureArguments(const CMakeProject& project,
                                             BuildConfiguration config,
                                             const QString& generator = QStringLiteral("Ninja"),
                                             bool exportCompileCommands = true);

/// Arguments for `cmake --build`. `--config` is carried for the multi-config
/// generators (Visual Studio, Ninja Multi-Config), which ignore
/// CMAKE_BUILD_TYPE; the single-config ones ignore --config instead, so one
/// command line is correct for either.
[[nodiscard]] QStringList buildArguments(const CMakeProject& project,
                                         BuildConfiguration config);

/// True once the build tree holds a CMakeCache.txt, i.e. configure has run.
[[nodiscard]] bool isConfigured(const CMakeProject& project,
                                BuildConfiguration config);

/// Every executable currently in the build tree, newest first.
///
/// `CMakeFiles/` is skipped: it is full of compiler probes (cmTC_xxxx.exe)
/// that are not the program the user asked for.
[[nodiscard]] QStringList builtExecutables(const QString& buildTree);

/// The most recently built executable, or empty when the tree has none.
[[nodiscard]] QString newestExecutable(const QString& buildTree);

}  // namespace LightCpp::Project
