#include "LightCpp/Project/FolderContents.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace LightCpp::Project {
namespace {

const QString kCmakeLists = QStringLiteral("CMakeLists.txt");

/// The `.dev` file to prefer when a directory holds several.
///
/// A directory with more than one project descriptor is rare, and picking by
/// name is at least predictable — a directory listing order would make which
/// project opens depend on the filesystem.
std::optional<ProjectFile> firstDescriptorFile(const QDir& dir)
{
    // Every non-CMake format the project plugins currently claim. CMake is
    // checked separately, before this, so it keeps priority.
    static const QStringList suffixes = {
        QStringLiteral("vcxproj"), QStringLiteral("vcproj"),
        QStringLiteral("dsw"),     QStringLiteral("dsp"),
        QStringLiteral("dev"),
    };
    const QStringList names =
        dir.entryList(QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    for (const QString& name : names) {
        if (suffixes.contains(QFileInfo(name).suffix().toLower())) {
            return ProjectFile{dir.absoluteFilePath(name), false};
        }
    }
    return std::nullopt;
}

}  // namespace

bool isDirectory(const QString& path)
{
    return !path.isEmpty() && QFileInfo(path).isDir();
}

std::optional<ProjectFile> projectFileIn(const QString& dir)
{
    if (!isDirectory(dir)) {
        return std::nullopt;
    }
    const QDir directory(dir);

    // Case-insensitive, like openProjectFile(): the file is CMakeLists.txt on
    // every platform CMake runs on, but Windows will hand back whatever the
    // user actually typed.
    const QStringList entries = directory.entryList(QDir::Files | QDir::Readable,
                                                   QDir::Name);
    for (const QString& name : entries) {
        if (name.compare(kCmakeLists, Qt::CaseInsensitive) == 0) {
            return ProjectFile{directory.absoluteFilePath(name), true};
        }
    }

    return firstDescriptorFile(directory);
}

}  // namespace LightCpp::Project
