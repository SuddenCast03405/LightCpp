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
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return path;
}

inline bool isWindowsDriveAbsolutePath(const QString& path)
{
    return path.size() >= 3 && path.at(0).isLetter()
           && path.at(1) == QLatin1Char(':')
           && path.at(2) == QLatin1Char('/');
}

inline bool isWindowsUncAbsolutePath(const QString& path)
{
    const QString normalized = normalizeSeparators(path);
    if (!normalized.startsWith(QStringLiteral("//"))
        || normalized.startsWith(QStringLiteral("///"))
        || normalized.startsWith(QStringLiteral("//?/"))
        || normalized.startsWith(QStringLiteral("//./"))) {
        return false;
    }
    const QStringList parts =
        normalized.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.size() >= 2 && parts.at(0) != QStringLiteral(".")
           && parts.at(0) != QStringLiteral("..")
           && parts.at(1) != QStringLiteral(".")
           && parts.at(1) != QStringLiteral("..");
}

inline QString cleanProjectPath(const QString& path)
{
    const QString normalized = normalizeSeparators(path);
    if (!isWindowsUncAbsolutePath(normalized)) {
        return QDir::cleanPath(normalized);
    }

    const QStringList components =
        normalized.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList cleaned{components.at(0), components.at(1)};
    for (qsizetype i = 2; i < components.size(); ++i) {
        const QString& part = components.at(i);
        if (part.isEmpty() || part == QStringLiteral(".")) {
            continue;
        }
        if (part == QStringLiteral("..")) {
            if (cleaned.size() > 2) {
                cleaned.removeLast();
            }
            continue;
        }
        cleaned.append(part);
    }
    return QStringLiteral("//") + cleaned.join(QLatin1Char('/'));
}

inline bool isAbsolutePath(const QString& path)
{
    const QString normalized = normalizeSeparators(path);
    return QDir::isAbsolutePath(normalized)
           || isWindowsDriveAbsolutePath(normalized)
           || isWindowsUncAbsolutePath(normalized);
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
        return cleanProjectPath(normalized);
    }
    return QFileInfo(normalized).absoluteFilePath();
}

inline QString directoryForFile(const QString& filePath)
{
    const QString absolute = absoluteFilePath(filePath);
    if (isWindowsUncAbsolutePath(absolute)) {
        const qsizetype slash = absolute.lastIndexOf(QLatin1Char('/'));
        if (slash >= 2) {
            return absolute.left(slash);
        }
    }
    return QFileInfo(absolute).path();
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
        return cleanProjectPath(normalized);
    }

    const QString base = normalizeSeparators(projectDir);
    if (isWindowsDriveAbsolutePath(base) || isWindowsUncAbsolutePath(base)) {
        return cleanProjectPath(base + QLatin1Char('/') + normalized);
    }
    return cleanProjectPath(QDir(base).absoluteFilePath(normalized));
}

/// Returns a path relative to baseDir. Windows drive paths are compared using
/// Windows' case-insensitive semantics even when the parser runs on POSIX.
inline QString relativePath(const QString& baseDir, const QString& targetPath)
{
    const bool baseUnc = isWindowsUncAbsolutePath(baseDir);
    const bool targetUnc = isWindowsUncAbsolutePath(targetPath);
    const QString base = cleanProjectPath(normalizeSeparators(baseDir));
    const QString target = cleanProjectPath(normalizeSeparators(targetPath));

    if (baseUnc && targetUnc) {
        const QStringList baseParts =
            base.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const QStringList targetParts =
            target.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (baseParts.at(0).compare(targetParts.at(0), Qt::CaseInsensitive) != 0
            || baseParts.at(1).compare(targetParts.at(1),
                                       Qt::CaseInsensitive) != 0) {
            return target;
        }

        qsizetype common = 2;
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
                               : parts.join(QLatin1Char('/'));
    }

    if (baseUnc != targetUnc
        && (isAbsolutePath(base) || isAbsolutePath(target))) {
        return target;
    }

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
