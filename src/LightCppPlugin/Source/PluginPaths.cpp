// PluginPaths — see the header for why the resolution lives here rather
// than in each caller.

#include "LightCpp/Plugin/PluginPaths.hpp"

#include <QDir>
#include <QFileInfo>

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
    return !directory.entryList({QStringLiteral("*Plugin*.dll")},
                                QDir::Files)
                .isEmpty();
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
