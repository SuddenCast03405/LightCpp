#pragma once

#include <QString>

#include <optional>

/// Recognising what a directory is, before deciding what to do with it.
///
/// Opening a folder is not one operation but three, and which one the user
/// meant is decided by what is *in* the directory:
///
///   * a `CMakeLists.txt` or a `*.dev` makes it a project — the IDE can build
///     and run it, so the folder should be opened as that project;
///   * otherwise it is just a folder, and the Explorer shows its contents.
///
/// The distinction is worth its own module because getting it wrong is silent:
/// a folder that really is a project but is opened as "just a folder" looks
/// perfectly fine — the tree fills in and nothing complains — while Build and
/// Run stay greyed out for no visible reason.
namespace LightCpp::Project {

/// A project descriptor file found in a directory.
struct ProjectFile {
    QString filePath;  // absolute path to the descriptor itself
    bool isCMake = false;

    [[nodiscard]] bool isValid() const noexcept { return !filePath.isEmpty(); }
};

/// True when `path` names a directory.
[[nodiscard]] bool isDirectory(const QString& path);

/// The project descriptor sitting *directly* in `dir`.
///
/// Only one level is examined on purpose. `findCMakeProject()` already knows
/// how to climb, and that is the right question when the user pointed at a
/// source file buried inside the project — but the user who is opening a
/// *folder* has named the root they want, and quietly climbing out of it would
/// open something they did not ask for.
///
/// A CMakeLists.txt wins over any other descriptor when both are present;
/// among the remaining formats (vcxproj > vcproj > dsw > dsp > dev) the
/// format decides first and the file name only breaks ties within one
/// format — matching openProjectFile()'s own precedence.
[[nodiscard]] std::optional<ProjectFile> projectFileIn(const QString& dir);

}  // namespace LightCpp::Project
