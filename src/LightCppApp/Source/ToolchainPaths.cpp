#include "LightCpp/App/ToolchainPaths.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace LightCpp::App::ToolchainPaths {
namespace {

/// Return a compiler directory only when it actually contains a host
/// compiler. This avoids treating an arbitrary PATH entry as an MSYS2 root.
QString compilerBinIfPresent(const QString& bin)
{
    if (bin.isEmpty()) {
        return {};
    }
    for (const QString& name : {QStringLiteral("g++"),
                                QStringLiteral("g++.exe"),
                                QStringLiteral("clang++"),
                                QStringLiteral("clang++.exe")}) {
        const QFileInfo info(QDir(bin).filePath(name));
        if (info.isFile() && info.isExecutable()) {
            return QDir(bin).absolutePath();
        }
    }
    return {};
}

/// Probe standard MSYS2 sub-environments under a discovered installation
/// root. The root itself comes from environment, bash on PATH, or mounted
/// drives; no drive letter or user-specific directory is embedded here.
QString msys2BinFromRoot(const QString& root)
{
    if (root.isEmpty()) {
        return {};
    }
    for (const QString& relative : {
             QStringLiteral("ucrt64/bin"), QStringLiteral("mingw64/bin"),
             QStringLiteral("clang64/bin"), QStringLiteral("mingw32/bin"),
             QStringLiteral("clang32/bin"), QStringLiteral("bin")}) {
        const QString found =
            compilerBinIfPresent(QDir(root).filePath(relative));
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

/// A tool: the copy in the compiler bin directory first, then PATH.
/// Check both Unix names and Windows .exe names without assuming the host.
QString toolPath(const QStringList& names)
{
    const QString bin = mingwBin();
    for (const QString& name : names) {
        if (!bin.isEmpty()) {
            const QString local = QDir(bin).filePath(name);
            const QFileInfo localInfo(local);
            if (localInfo.isFile() && localInfo.isExecutable()) {
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
        const QString prefix = qEnvironmentVariable("MINGW_PREFIX");
        if (!prefix.isEmpty()) {
            if (const QString bin = compilerBinIfPresent(prefix);
                !bin.isEmpty()) {
                return bin;
            }
            if (const QString bin = compilerBinIfPresent(
                    QDir(prefix).filePath(QStringLiteral("bin")));
                !bin.isEmpty()) {
                return bin;
            }
        }

        if (const QString bin = msys2BinFromRoot(
                qEnvironmentVariable("MSYS2_ROOT")); !bin.isEmpty()) {
            return bin;
        }

        // If the MSYS2 shell is on PATH but its UCRT64 compiler directory is
        // not, infer the installation root from <root>/usr/bin/bash.exe.
        QString bash = QStandardPaths::findExecutable(QStringLiteral("bash.exe"));
        if (bash.isEmpty()) {
            bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
        }
        if (!bash.isEmpty()) {
            QDir root(QFileInfo(bash).absolutePath());
            if (root.dirName().compare(QLatin1String("bin"),
                                       Qt::CaseInsensitive) == 0
                && root.cdUp()
                && root.dirName().compare(QLatin1String("usr"),
                                          Qt::CaseInsensitive) == 0
                && root.cdUp()) {
                if (const QString bin = msys2BinFromRoot(root.absolutePath());
                    !bin.isEmpty()) {
                    return bin;
                }
            }
        }

#ifdef Q_OS_WIN
        // Search the standard MSYS2 folder names on mounted drives rather
        // than assuming it was installed on C: or E:.
        for (const QFileInfo& drive : QDir::drives()) {
            for (const QString& name : {QStringLiteral("msys64"),
                                        QStringLiteral("msys32")}) {
                if (const QString bin = msys2BinFromRoot(
                        QDir(drive.absoluteFilePath()).filePath(name));
                    !bin.isEmpty()) {
                    return bin;
                }
            }
        }
#endif

        QString found = QStandardPaths::findExecutable(QStringLiteral("g++"));
        if (found.isEmpty()) {
            found = QStandardPaths::findExecutable(QStringLiteral("g++.exe"));
        }
        if (found.isEmpty()) {
            found = QStandardPaths::findExecutable(QStringLiteral("clang++"));
        }
        if (found.isEmpty()) {
            found = QStandardPaths::findExecutable(QStringLiteral("clang++.exe"));
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


namespace {

/// The first candidate the PATH (or the toolchain directory) provides, using
/// QStandardPaths rather than a hard-coded prefix.
QString findTool(const QStringList& names)
{
    const QString local = mingwBin();
    if (!local.isEmpty()) {
        for (const QString& name : names) {
            const QString candidate = QDir(local).filePath(name);
            const QFileInfo info(candidate);
            if (info.isFile() && info.isExecutable()) {
                return info.absoluteFilePath();
            }
        }
    }
    for (const QString& name : names) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

}  // namespace

QString cppcheckPath()
{
    return findTool({QStringLiteral("cppcheck")});
}

QString clangTidyPath()
{
    return findTool({QStringLiteral("clang-tidy"),
                     QStringLiteral("clang-tidy-19"), QStringLiteral("clang-tidy-18"),
                     QStringLiteral("clang-tidy-17"), QStringLiteral("clang-tidy-16"),
                     QStringLiteral("clang-tidy-15"), QStringLiteral("clang-tidy-14")});
}

QString scanBuildPath()
{
    return findTool({QStringLiteral("scan-build"),
                     QStringLiteral("scan-build-19"), QStringLiteral("scan-build-18"),
                     QStringLiteral("scan-build-17"), QStringLiteral("scan-build-16"),
                     QStringLiteral("scan-build-15"), QStringLiteral("scan-build-14")});
}

QString pvsStudioPath()
{
    // Windows spells it PVS-Studio_Cmd.exe, Linux pvs-studio-analyzer. Both
    // are accepted because a project may well be analysed on either.
    return findTool({QStringLiteral("pvs-studio-analyzer"),
                     QStringLiteral("PVS-Studio_Cmd.exe"),
                     QStringLiteral("pvs-studio")});
}

QString coverityPath()
{
    // `cov-analyze` is the analyser; `cov-build` only produces the
    // intermediate directory. The panel drives cov-build itself, so this
    // resolves the build wrapper and derives the rest from its directory.
    return findTool({QStringLiteral("cov-build"),
                     QStringLiteral("cov-analyze"),
                     QStringLiteral("coverity")});
}

}  // namespace LightCpp::App::ToolchainPaths