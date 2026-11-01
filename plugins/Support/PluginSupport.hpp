// PluginSupport — the small, header-only toolbox every first-party
// project-backend plugin shares: locating a tool on PATH, reading a
// descriptor as UTF-8, splitting lines, counting source files, and spelling
// the host's executable suffix. It is deliberately not a library target: the
// helpers are inline, so each plugin DLL stays self-contained.

#pragma once

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace LightCpp::Plugins::Support {

/// The file name a host tool has on this platform ("go" → "go.exe").
[[nodiscard]] inline QString hostExecutableName(const QString& base)
{
#ifdef Q_OS_WIN
    return base.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)
               ? base
               : base + QStringLiteral(".exe");
#else
    return base;
#endif
}

/// The executable suffix a compiled target has on this platform.
[[nodiscard]] inline QString hostExecutableSuffix()
{
#ifdef Q_OS_WIN
    return QStringLiteral(".exe");
#else
    return {};
#endif
}

/// First of `names` found on PATH, or empty. On Windows a bare name is retried
/// with .exe, because QStandardPaths does not always apply PATHEXT itself.
[[nodiscard]] inline QString findTool(const QStringList& names)
{
    for (const QString& name : names) {
        const QString trimmed = name.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const QString found = QStandardPaths::findExecutable(trimmed);
        if (!found.isEmpty()) {
            return found;
        }
#ifdef Q_OS_WIN
        if (!trimmed.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
            const QString withSuffix =
                QStandardPaths::findExecutable(trimmed + QStringLiteral(".exe"));
            if (!withSuffix.isEmpty()) {
                return withSuffix;
            }
        }
#endif
    }
    return {};
}

/// Reads a descriptor as UTF-8. BOMs are dropped; a read error is reported
/// through `error` and leaves the returned text empty.
[[nodiscard]] inline QString readTextFile(const QString& path, QString* error = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot read %1: %2")
                         .arg(QDir::toNativeSeparators(path),
                              file.errorString());
        }
        return {};
    }
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot read %1: %2")
                         .arg(QDir::toNativeSeparators(path),
                              file.errorString());
        }
        return {};
    }
    QString text = QString::fromUtf8(data);
    if (text.startsWith(QChar(0xFEFF))) {
        text.remove(0, 1);
    }
    return text;
}

/// Splits on LF and drops the CR that Windows line endings leave behind.
[[nodiscard]] inline QStringList splitLines(const QString& text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
    }
    return lines;
}

/// Removes a trailing `#` comment that is not inside single or double quotes.
/// TOML (.toml) and qmake (.pro) both use this marker.
[[nodiscard]] inline QString stripHashComment(const QString& line)
{
    QChar quote;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\')) {
                ++i;
            } else if (c == quote) {
                quote = QChar();
            }
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            continue;
        }
        if (c == QLatin1Char('#')) {
            return line.left(i);
        }
    }
    return line;
}

/// Unquotes one TOML/qmake scalar, honouring single and double quotes and the
/// common backslash escapes inside double quotes.
[[nodiscard]] inline QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() < 2) {
        return value;
    }
    const QChar first = value.front();
    if ((first != QLatin1Char('"') && first != QLatin1Char('\''))
        || value.back() != first) {
        return value;
    }
    QString inner = value.mid(1, value.size() - 2);
    if (first == QLatin1Char('"')) {
        inner.replace(QLatin1String("\\n"), QStringLiteral("\n"));
        inner.replace(QLatin1String("\\t"), QStringLiteral("\t"));
        inner.replace(QLatin1String("\\\""), QStringLiteral("\""));
        inner.replace(QLatin1String("\\\\"), QStringLiteral("\\"));
    }
    return inner;
}

/// True when every `.`-separated path segment in `relativePath` is an
/// intermediate directory the callers never count sources in.
[[nodiscard]] inline bool isSkippedDirectoryName(const QString& name)
{
    static const QStringList skipped = {
        QStringLiteral(".git"),       QStringLiteral(".svn"),
        QStringLiteral("__pycache__"), QStringLiteral("node_modules"),
        QStringLiteral("target"),     QStringLiteral("zig-cache"),
        QStringLiteral(".zig-cache"), QStringLiteral("zig-out"),
        QStringLiteral("build"),      QStringLiteral("dist"),
        QStringLiteral("vendor"),     QStringLiteral(".venv"),
        QStringLiteral("venv"),
    };
    return skipped.contains(name, Qt::CaseInsensitive);
}

/// Counts files whose suffix (case-insensitively) is in `suffixes`, below
/// `root`, skipping the usual build/cache/VCS directories.
[[nodiscard]] inline int countFilesWithSuffix(const QString& root,
                                             const QStringList& suffixes,
                                             bool recursive = true)
{
    const QDir base(root);
    if (!base.exists()) {
        return 0;
    }
    const QDirIterator::IteratorFlags flags =
        recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    int count = 0;
    QDirIterator it(root, QDir::Files, flags);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = base.relativeFilePath(path);
        const QStringList parts =
            relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        bool skipped = false;
        for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
            if (isSkippedDirectoryName(parts.at(i))) {
                skipped = true;
                break;
            }
        }
        if (skipped) {
            continue;
        }
        if (suffixes.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive)) {
            ++count;
        }
    }
    return count;
}

/// The last path segment of a module name ("example.com/hello" → "hello").
[[nodiscard]] inline QString lastPathSegment(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? path.mid(slash + 1) : path;
}

/// A package/script identifier usable in a file name: lowercase, spaces and
/// dots folded to underscores.
[[nodiscard]] inline QString normalizedName(const QString& name,
                                           const QString& fallback)
{
    QString result = name.trimmed().toLower();
    for (QChar& c : result) {
        if (c.isSpace() || c == QLatin1Char('.') || c == QLatin1Char('-')) {
            c = QLatin1Char('_');
        }
    }
    return result.isEmpty() ? fallback : result;
}

}  // namespace LightCpp::Plugins::Support
