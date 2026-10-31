#include "LightCpp/App/ToolchainPaths.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace LightCpp::App::ToolchainPaths {
namespace {

/// Locate a toolchain directory from the executable search path. This
/// intentionally avoids machine-specific drive letters and install roots.
QString dirOnPath(const QString& tool)
{
    const QString found = QStandardPaths::findExecutable(tool);
    return found.isEmpty() ? QString() : QFileInfo(found).absolutePath();
}

/// A tool: the copy in the build bin directory first, then PATH.
QString toolPath(const QString& exe)
{
    const QString bin = mingwBin();
    if (hasFile(bin, exe)) {
        return QDir(bin).filePath(exe);
    }
    return QStandardPaths::findExecutable(QFileInfo(exe).completeBaseName());
}

}  // namespace

QString firstWithTool(const QStringList& candidates,
                      const std::function<bool(const QString&)>& hasTool)
{
    if (!hasTool) {
        return {};
    }
    for (const QString& candidate : candidates) {
        if (candidate.isEmpty() || !hasTool(candidate)) {
            continue;
        }
        return candidate;
    }
    return {};
}

QString mingwBin()
{
    static const QString resolved = dirOnPath(QStringLiteral("g++"));
    return resolved;
}

QString clangdPath()
{
    static const QString resolved = toolPath(QStringLiteral("clangd"));
    return resolved;
}

QString clangFormatPath()
{
    static const QString resolved = toolPath(QStringLiteral("clang-format"));
    return resolved;
}

QString gitPath()
{
    static const QString resolved = toolPath(QStringLiteral("git"));
    return resolved;
}

}  // namespace LightCpp::App::ToolchainPaths
