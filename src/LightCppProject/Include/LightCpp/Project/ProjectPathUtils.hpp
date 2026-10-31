#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace LightCpp::Project::Paths {

/// Project files written on Windows keep backslashes even when they are read
/// on POSIX. Qt's fromNativeSeparators() is a no-op on POSIX, so normalise
/// both separator styles explicitly and use forward slashes internally.
inline QString normalizeSeparators(QString path)
{
    path.replace(QLatin1Char('\\\\'), QLatin1Char('/'));
    return path;
}

inline bool isWindowsDriveAbsolutePath(const QString& path)
{
    return path.size() >= 3 && path.at(0).isLetter()
           && path.at(1) == QLatin1Char(':')
           && path.at(2) == QLatin1Char('/');
}

inline bool isAbsolutePath(const QString& path)
{
    return QDir::isAbsolutePath(path) || isWindowsDriveAbsolutePath(path);
}

/// Makes a project file path absolute without turning a foreign Windows drive
/// path (for example E:/work/app.vcxproj) into <cwd>/E:/work/... on POSIX.
inline QString absoluteFilePath(const QString& path)
{
    const QString normalized = normalizeSeparators(path);
    if (normalized.isEmpty()) {
        return {};
    }
    if (isAbsolutePath(normalized)) {
        return QDir::cleanPath(normalized);
    }
    return QFileInfo(normalized).absoluteFilePath();
}

inline QString directoryForFile(const QString& filePath)
{
    return QFileInfo(absoluteFilePath(filePath)).path();
}

/// Resolves a project-relative reference while preserving Windows drive paths
/// on POSIX. QDir on POSIX does not regard E:/... as absolute, so join those
/// paths lexically rather than asking QDir to prepend the current directory.
inline QString resolveReference(const QString& projectDir,
                                const QString& reference)
{
    const QString normalized = normalizeSeparators(reference.trimmed());
    if (normalized.isEmpty()) {
        return {};
    }
    if (isAbsolutePath(normalized)) {
        return QDir::cleanPath(normalized);
    }

    const QString base = normalizeSeparators(projectDir);
    if (isWindowsDriveAbsolutePath(base)) {
        return QDir::cleanPath(base + QLatin1Char('/') + normalized);
    }
    return QDir::cleanPath(QDir(base).absoluteFilePath(normalized));
}

/// Returns a path relative to baseDir. Windows drive paths are compared using
/// Windows' case-insensitive semantics even when the parser runs on POSIX.
inline QString relativePath(const QString& baseDir, const QString& targetPath)
{
    const QString base = QDir::cleanPath(normalizeSeparators(baseDir));
    const QString target = QDir::cleanPath(normalizeSeparators(targetPath));

    if (isWindowsDriveAbsolutePath(base)
        && isWindowsDriveAbsolutePath(target)) {
        if (base.left(2).compare(target.left(2), Qt::CaseInsensitive) != 0) {
            return target;
        }

        const QStringList baseParts = base.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const QStringList targetParts =
            target.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        qsizetype common = 0;
        while (common < baseParts.size() && common < targetParts.size()
               && baseParts.at(common).compare(targetParts.at(common),
                                                Qt::CaseInsensitive) == 0) {
            ++common;
        }

        QStringList parts;
        for (qsizetype i = common; i < baseParts.size(); ++i) {
            parts.append(QStringLiteral(".."));
        }
        for (qsizetype i = common; i < targetParts.size(); ++i) {
            parts.append(targetParts.at(i));
        }
        return parts.isEmpty() ? QStringLiteral(".")
                               : QDir::cleanPath(parts.join(QLatin1Char('/')));
    }

    if (isWindowsDriveAbsolutePath(base) != isWindowsDriveAbsolutePath(target)
        && (isAbsolutePath(base) || isAbsolutePath(target))) {
        return target;
    }
    return QDir(base).relativeFilePath(target);
}

}  // namespace LightCpp::Project::Paths
