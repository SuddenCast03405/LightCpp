#include "LightCpp/Workbench/ExternalTools.hpp"

#include <QDir>
#include <QFileInfo>

namespace LightCpp::Workbench::ExternalTools {

QStringList placeholderNames()
{
    return {QStringLiteral("${file}"), QStringLiteral("${fileDir}"),
            QStringLiteral("${fileName}"), QStringLiteral("${projectRoot}"),
            QStringLiteral("${buildDir}"), QStringLiteral("${selection}")};
}

bool needsPosixQuoting(const QString& value)
{
    if (value.isEmpty()) {
        return true;
    }
    for (const QChar ch : value) {
        // The safe set is deliberately narrow. A path with a space is the
        // common case; a path with a quote, a `$` or a glob character is
        // the case that quietly runs the wrong command.
        const bool safe = ch.isLetterOrNumber() || ch == QLatin1Char('/')
                          || ch == QLatin1Char('.') || ch == QLatin1Char('_')
                          || ch == QLatin1Char('-') || ch == QLatin1Char('+')
                          || ch == QLatin1Char(':') || ch == QLatin1Char('\\');
        if (!safe) {
            return true;
        }
    }
    return false;
}

bool needsWindowsQuoting(const QString& value)
{
    if (value.isEmpty()) {
        return true;
    }
    for (const QChar ch : value) {
        const bool safe = ch.isLetterOrNumber() || ch == QLatin1Char('/')
                          || ch == QLatin1Char('\\') || ch == QLatin1Char('.')
                          || ch == QLatin1Char('_') || ch == QLatin1Char('-')
                          || ch == QLatin1Char('+') || ch == QLatin1Char(':');
        if (!safe) {
            return true;
        }
    }
    return false;
}

QString quoteForShell(const QString& value, Shell shell)
{
    if (shell == Shell::Windows) {
        if (!needsWindowsQuoting(value)) {
            return value;
        }
        QString escaped = value;
        // cmd.exe has no escape character inside double quotes; a literal
        // quote is written as two. `%` expands variables even inside quotes,
        // so it has to be doubled as well or `%TEMP%` in a path becomes a
        // directory that does not exist.
        escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        escaped.replace(QLatin1Char('%'), QStringLiteral("%%"));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    }
    if (!needsPosixQuoting(value)) {
        return value;
    }
    QString escaped = value;
    // The only character that cannot appear inside single quotes is the
    // single quote itself; the canonical way out is to close the quote,
    // emit an escaped one, and reopen.
    escaped.replace(QLatin1String("'"), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString directoryOf(const QString& filePath)
{
    if (filePath.isEmpty()) {
        return {};
    }
    // QFileInfo::path() of a bare name is ".", which is a working directory
    // the caller did not ask for; return empty instead.
    const QFileInfo info(filePath);
    const QString directory = info.path();
    return directory == QLatin1String(".") ? QString() : directory;
}

QString fileNameOf(const QString& filePath)
{
    return filePath.isEmpty() ? QString() : QFileInfo(filePath).fileName();
}

QString expand(const QString& templateText, const Context& context, Shell shell,
               QStringList* missing)
{
    if (templateText.isEmpty()) {
        return {};
    }

    // The values are resolved once, not per occurrence: a template that uses
    // ${file} three times must get the same answer three times even if the
    // editor changed between them (which it cannot here, but the invariant
    // is cheap).
    struct Pair {
        QString name;
        QString value;
        bool quote;
    };
    const QList<Pair> values{
        {QStringLiteral("${file}"), context.file, true},
        {QStringLiteral("${fileDir}"), directoryOf(context.file), true},
        {QStringLiteral("${fileName}"), fileNameOf(context.file), true},
        {QStringLiteral("${projectRoot}"), context.projectRoot, true},
        {QStringLiteral("${buildDir}"), context.buildDir, true},
        // A selection is usually several arguments' worth of text; quoting it
        // would collapse it into one. It is passed through as the user typed
        // it, which is what a shell fragment means anyway.
        {QStringLiteral("${selection}"), context.selection, false},
    };

    QString result = templateText;
    for (const Pair& pair : values) {
        if (!result.contains(pair.name)) {
            continue;
        }
        const QString substituted =
            pair.value.isEmpty() ? QString()
                                 : (pair.quote ? quoteForShell(pair.value, shell)
                                               : pair.value);
        if (pair.value.isEmpty() && missing != nullptr && !missing->contains(pair.name)) {
            missing->append(pair.name);
        }
        result.replace(pair.name, substituted);
    }
    return result;
}

QString makeId(const QString& name, const QStringList& existing)
{
    QString base = name.toLower().trimmed();
    QString cleaned;
    cleaned.reserve(base.size());
    bool lastWasDash = false;
    for (const QChar ch : base) {
        if (ch.isLetterOrNumber()) {
            cleaned.append(ch);
            lastWasDash = false;
        } else if (!lastWasDash && !cleaned.isEmpty()) {
            cleaned.append(QLatin1Char('-'));
            lastWasDash = true;
        }
    }
    while (cleaned.endsWith(QLatin1Char('-'))) {
        cleaned.chop(1);
    }
    if (cleaned.isEmpty()) {
        cleaned = QStringLiteral("tool");
    }
    if (!existing.contains(cleaned)) {
        return cleaned;
    }
    for (int suffix = 2;; ++suffix) {
        const QString candidate = QStringLiteral("%1-%2").arg(cleaned).arg(suffix);
        if (!existing.contains(candidate)) {
            return candidate;
        }
    }
}

QString describe(const QString& command, const QString& arguments, int maxLength)
{
    if (command.trimmed().isEmpty()) {
        return {};
    }
    QString text = command.trimmed();
    if (!arguments.trimmed().isEmpty()) {
        text += QLatin1Char(' ') + arguments.trimmed();
    }
    // Collapse runs of whitespace: a multi-line template would otherwise
    // push the menu entry to twice the height of its neighbours.
    text = text.simplified();
    if (maxLength > 0 && text.size() > maxLength) {
        text = text.left(maxLength - 3) + QStringLiteral("...");
    }
    return text;
}

QString workingDirectoryFor(const QString& templateText, const Context& context,
                            Shell shell)
{
    if (!templateText.trimmed().isEmpty()) {
        // Expanded, then unquoted: a working directory is handed to QProcess
        // as a path, not to a shell, so quotes would become part of the name
        // and the directory would not exist.
        QString unquoted = expand(templateText, context, shell);
        if (unquoted.size() >= 2 && unquoted.startsWith(QLatin1Char('\''))
            && unquoted.endsWith(QLatin1Char('\''))) {
            unquoted = unquoted.mid(1, unquoted.size() - 2);
        } else if (unquoted.size() >= 2 && unquoted.startsWith(QLatin1Char('"'))
                   && unquoted.endsWith(QLatin1Char('"'))) {
            unquoted = unquoted.mid(1, unquoted.size() - 2);
        }
        if (!unquoted.isEmpty()) {
            return unquoted;
        }
    }
    if (!context.projectRoot.isEmpty()) {
        return context.projectRoot;
    }
    return directoryOf(context.file);
}

}  // namespace LightCpp::Workbench::ExternalTools
