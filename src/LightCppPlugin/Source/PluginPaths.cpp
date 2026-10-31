// PluginPaths — see the header for why the resolution lives here rather
// than in each caller.

#include "LightCpp/Plugin/PluginPaths.hpp"

#include <QDir>
#include <QFileInfo>
#include <QLibrary>

namespace LightCpp::Plugin {

namespace {

/// True when `directory` exists and holds at least one plugin library.
/// The name filter is case-insensitive on Windows, which is what makes
/// the real `libdevcppplugin.dll` match `*Plugin*.dll`.
[[nodiscard]] bool holdsPluginLibraries(const QDir& directory)
{
    if (!directory.exists()) {
        return false;
    }
    // Qt's library predicate understands the native suffix on each
    // platform (.dll, .so, .dylib). Do not bake Windows' DLL suffix into
    // discovery or Linux/macOS plugins silently disappear.
    for (const QString& fileName : directory.entryList(QDir::Files)) {
        if (fileName.contains(QStringLiteral("Plugin"), Qt::CaseInsensitive)
            && QLibrary::isLibrary(directory.filePath(fileName))) {
            return true;
        }
    }
    return false;
}

}  // namespace

QString defaultPluginsDirectory(const QString& executableDir)
{
    return QDir(executableDir).filePath(QStringLiteral("plugins"));
}

QString resolvePluginsDirectory(const QString& executableDir, int levelsUp)
{
    const QString nextToExecutable = defaultPluginsDirectory(executableDir);
    if (QFileInfo::exists(nextToExecutable)) {
        return nextToExecutable;
    }
    QDir probe(executableDir);
    for (int up = 0; up < levelsUp; ++up) {
        if (!probe.cdUp()) {
            break;
        }
        const QString candidate =
            probe.filePath(QStringLiteral("plugins"));
        if (holdsPluginLibraries(QDir(candidate))) {
            return candidate;
        }
    }
    return nextToExecutable;
}

}  // namespace LightCpp::Plugin
