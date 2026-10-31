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

/// A tool: the copy in the compiler bin directory first, then PATH.
/// Check both Unix names and Windows .exe names without assuming the host.
QString toolPath(const QStringList& names)
{
    const QString bin = mingwBin();
    for (const QString& name : names) {
        if (!bin.isEmpty()) {
            const QString local = QDir(bin).filePath(name);
            if (QFileInfo::exists(local)) {
                return local;
            }
        }
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
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
    static const QString resolved = [] {
        QString found = QStandardPaths::findExecutable(QStringLiteral("g++"));
        if (found.isEmpty()) {
            found = QStandardPaths::findExecutable(QStringLiteral("g++.exe"));
        }
        return found.isEmpty() ? QString() : QFileInfo(found).absolutePath();
    }();
    return resolved;
}

QString clangdPath()
{
    static const QString resolved = toolPath(
        {QStringLiteral("clangd"), QStringLiteral("clangd.exe")});
    return resolved;
}

QString clangFormatPath()
{
    static const QString resolved = toolPath(
        {QStringLiteral("clang-format"), QStringLiteral("clang-format.exe")});
    return resolved;
}

QString gitPath()
{
    static const QString resolved = toolPath(
        {QStringLiteral("git"), QStringLiteral("git.exe")});
    return resolved;
}

}  // namespace LightCpp::App::ToolchainPaths
