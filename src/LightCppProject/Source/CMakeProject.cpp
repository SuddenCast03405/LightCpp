#include "LightCpp/Project/CMakeProject.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QVector>

#include <algorithm>

namespace LightCpp::Project {
namespace {

const QString kCmakeLists = QStringLiteral("CMakeLists.txt");

/// CMake's own marker that a build tree has been configured.
const QString kCacheFile = QStringLiteral("CMakeCache.txt");

bool hasExecutableSuffix(const QString& fileName)
{
    // This IDE builds for Windows, so .exe is the whole story. Checked
    // case-insensitively because the suffix can come from anywhere.
    return fileName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive);
}

bool isInsideCMakeFiles(const QString& path)
{
    // Compiler feature probes land here (cmTC_1a2b3c.exe) and are never the
    // program the user asked for.
    return path.contains(QStringLiteral("/CMakeFiles/"), Qt::CaseInsensitive);
}

}  // namespace

std::optional<CMakeProject> findCMakeProject(const QString& startDir, int maxDepth)
{
    if (startDir.isEmpty()) {
        return std::nullopt;
    }
    QDir dir = QFileInfo(startDir).isDir()
                   ? QDir(startDir)
                   : QFileInfo(startDir).absoluteDir();
    if (!dir.exists()) {
        return std::nullopt;
    }

    for (int depth = 0; depth <= maxDepth; ++depth) {
        if (QFileInfo(dir.filePath(kCmakeLists)).isFile()) {
            return CMakeProject{dir.absolutePath()};
        }
        // A repository or a Dev-C++ project marks the top of the project far
        // more reliably than a depth count, so stop climbing there.
        if (QFileInfo(dir.filePath(QStringLiteral(".git"))).exists()) {
            return std::nullopt;
        }
        if (!dir.cdUp()) {
            break;  // hit the filesystem root
        }
    }
    return std::nullopt;
}

QString buildDirectory(const CMakeProject& project, BuildConfiguration config)
{
    return QDir(QDir(project.sourceDir).filePath(QStringLiteral("build")))
        .filePath(toString(config));
}

QStringList configureArguments(const CMakeProject& project, BuildConfiguration config,
                               const QString& generator, bool exportCompileCommands)
{
    // Forward slashes throughout: CMake takes them on every platform, and a
    // command line that reads the same everywhere is one less thing to get
    // wrong when it gets printed into the build log.
    QStringList args{QStringLiteral("-S"), project.sourceDir,
                     QStringLiteral("-B"),
                     buildDirectory(project, config)};
    if (!generator.isEmpty()) {
        // Ninja is single-config, so CMAKE_BUILD_TYPE actually selects the
        // flags; with a multi-config generator it is ignored and --config on
        // the build step takes over.
        args << QStringLiteral("-G") << generator;
    }
    args << QStringLiteral("-DCMAKE_BUILD_TYPE=%1").arg(toString(config));
    if (exportCompileCommands) {
        // Free to produce and the standard way to let external tooling (and
        // the user's own clangd setup) see the real command lines. It lands in
        // the build tree, never in the source tree.
        args << QStringLiteral("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
    }
    return args;
}

QStringList buildArguments(const CMakeProject& project, BuildConfiguration config)
{
    return {QStringLiteral("--build"),
            buildDirectory(project, config),
            QStringLiteral("--config"), toString(config)};
}

bool isConfigured(const CMakeProject& project, BuildConfiguration config)
{
    return QFileInfo(QDir(buildDirectory(project, config)).filePath(kCacheFile)).isFile();
}

QStringList builtExecutables(const QString& buildTree)
{
    QStringList found;
    if (buildTree.isEmpty() || !QFileInfo(buildTree).isDir()) {
        return found;
    }
    QDirIterator it(buildTree, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (!hasExecutableSuffix(it.fileName()) || isInsideCMakeFiles(path)) {
            continue;
        }
        found.append(path);
    }
    std::sort(found.begin(), found.end(), [](const QString& a, const QString& b) {
        return QFileInfo(a).lastModified() > QFileInfo(b).lastModified();
    });
    return found;
}

QString newestExecutable(const QString& buildTree)
{
    const QStringList found = builtExecutables(buildTree);
    return found.isEmpty() ? QString() : found.first();
}

QString preferredExecutable(const QString& buildTree)
{
    const QStringList found = builtExecutables(buildTree);
    if (found.size() <= 1) {
        return found.isEmpty() ? QString() : found.first();
    }

    // CMAKE_PROJECT_NAME:<TYPE>=<value> in the cache — the top-level
    // project() call, whose name conventionally matches the main target.
    QString projectName;
    QFile cache(QDir(buildTree).filePath(kCacheFile));
    if (cache.open(QIODevice::ReadOnly)) {
        const QByteArray content = cache.readAll();
        if (cache.error() == QFileDevice::NoError) {
            for (const QByteArray& line : content.split('\n')) {
                if (line.startsWith("CMAKE_PROJECT_NAME:")) {
                    const qsizetype equals = line.indexOf('=');
                    if (equals > 0) {
                        projectName = QString::fromUtf8(
                            line.mid(equals + 1).trimmed());
                    }
                    break;
                }
            }
        }
        cache.close();
    }

    if (!projectName.isEmpty()) {
        for (const QString& path : found) {
            if (QFileInfo(path).completeBaseName().compare(
                    projectName, Qt::CaseInsensitive) == 0) {
                return path;
            }
        }
    }
    return found.first();  // newest — the old behaviour
}

}  // namespace LightCpp::Project
