#include "LightCpp/App/CompileDatabase.hpp"

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QStringList>

namespace LightCpp::App::CompileDatabase {
namespace {

constexpr const char* kDatabaseName = "compile_commands.json";

bool holdsDatabase(const QString& dir)
{
    return QFileInfo::exists(QDir(dir).filePath(QLatin1String(kDatabaseName)));
}

/// Direct child directories, sorted, so the answer does not depend on the
/// order the filesystem happens to return.
QStringList directoriesOf(const QString& dir)
{
    return QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

/// Names CMake (and the people using it) give a build root. Only these are
/// walked to depth 3.
bool looksLikeBuildRoot(const QString& name)
{
    return name.compare(QLatin1String("build"), Qt::CaseInsensitive) == 0
           || name.compare(QLatin1String("out"), Qt::CaseInsensitive) == 0
           || name.startsWith(QLatin1String("cmake-build-"), Qt::CaseInsensitive);
}

}  // namespace

QString findProjectDirectory(const QString& rootPath)
{
    if (rootPath.isEmpty()) {
        return {};
    }
    const QDir root(rootPath);
    if (!root.exists()) {
        return {};
    }

    const QString base = root.absolutePath();
    if (holdsDatabase(base)) {
        return base;
    }

    // Pass 1 and 2, over every branch: "build", "cmake-build-debug" and
    // "build/<preset>" / "build/Debug" are all within this much.
    const QStringList level1 = directoriesOf(base);
    for (const QString& entry : level1) {
        const QString path = QDir(base).filePath(entry);
        if (holdsDatabase(path)) {
            return path;
        }
    }

    QList<QPair<QString, QString>> level2;  // path, its parent's name
    for (const QString& entry : level1) {
        const QString parent = QDir(base).filePath(entry);
        for (const QString& child : directoriesOf(parent)) {
            const QString path = QDir(parent).filePath(child);
            if (holdsDatabase(path)) {
                return path;
            }
            level2.append({path, entry});
        }
    }

    // Pass 3, only beneath a build root: `out/build/<preset>` — the layout
    // CMake presets default to — and `build/<os>/<config>`.
    for (const auto& [path, parent] : level2) {
        if (!looksLikeBuildRoot(parent) && !looksLikeBuildRoot(QFileInfo(path).fileName())) {
            continue;
        }
        for (const QString& child : directoriesOf(path)) {
            const QString deeper = QDir(path).filePath(child);
            if (holdsDatabase(deeper)) {
                return deeper;
            }
        }
    }
    return {};
}

}  // namespace LightCpp::App::CompileDatabase
