#include "LightCpp/Project/GitStatus.hpp"

#include <QRegularExpression>
#include <QStringList>

namespace LightCpp::Project {
namespace {

/// git wraps a path in double quotes and escapes it when it contains anything
/// unusual. Undo the common escapes so the path can be shown.
///
/// Octal escapes (git writes UTF-8 bytes as `\303\251`) are deliberately left
/// alone: the path here is only ever displayed, never handed back to git, so
/// a slightly odd-looking name in a rare case is a better trade than
/// hand-rolling a byte-level decoder.
QString unquotePath(const QString& raw)
{
    const QString text = raw.trimmed();
    if (text.size() < 2 || !text.startsWith(u'"') || !text.endsWith(u'"')) {
        return text;
    }
    const QString inner = text.mid(1, text.size() - 2);
    QString out;
    out.reserve(inner.size());
    for (qsizetype i = 0; i < inner.size(); ++i) {
        const QChar ch = inner.at(i);
        if (ch != u'\\' || i + 1 >= inner.size()) {
            out.append(ch);
            continue;
        }
        const QChar escaped = inner.at(++i);
        switch (escaped.unicode()) {
        case 'n':  out.append(u'\n'); break;
        case 't':  out.append(u'\t'); break;
        case 'r':  out.append(u'\r'); break;
        case '"':  out.append(u'"');  break;
        case '\\': out.append(u'\\'); break;
        default:   out.append(escaped); break;
        }
    }
    return out;
}

/// Reads "ahead N" / "behind M" out of the bracketed part of the `## ` header.
///
/// The counting words are the one bit of the branch header git does not
/// formally promise to keep in English, so an unrecognised label leaves the
/// count at zero rather than guessing at which number is which. Nothing else
/// in the IDE depends on these numbers.
void parseTrackingCounts(const QString& counters, GitStatusSummary& summary)
{
    static const QRegularExpression aheadRe(
        QStringLiteral("ahead\\s+(\\d+)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression behindRe(
        QStringLiteral("behind\\s+(\\d+)"), QRegularExpression::CaseInsensitiveOption);
    if (const auto m = aheadRe.match(counters); m.hasMatch()) {
        summary.ahead = m.captured(1).toInt();
    }
    if (const auto m = behindRe.match(counters); m.hasMatch()) {
        summary.behind = m.captured(1).toInt();
    }
}

void parseBranchHeader(const QString& header, GitStatusSummary& summary)
{
    QString refs = header;
    const qsizetype bracket = refs.indexOf(QStringLiteral(" ["));
    if (bracket >= 0) {
        parseTrackingCounts(refs.mid(bracket + 2), summary);
        refs = refs.left(bracket);
    }
    if (refs.startsWith(QStringLiteral("HEAD (no branch)"))) {
        summary.detached = true;
        return;
    }
    // A repository with no commits yet still names the branch it is on.
    static const QStringList unbornPrefixes = {
        QStringLiteral("No commits yet on "),
        QStringLiteral("Initial commit on "),
    };
    for (const QString& prefix : unbornPrefixes) {
        if (refs.startsWith(prefix)) {
            summary.branch = refs.mid(prefix.size());
            return;
        }
    }
    const qsizetype dots = refs.indexOf(QStringLiteral("..."));
    if (dots >= 0) {
        summary.branch = refs.left(dots);
        summary.upstream = refs.mid(dots + 3);
    } else {
        summary.branch = refs;
    }
}

/// True for the characters git's `--porcelain=v1` format allows in either
/// status column. Anything else means the line is not porcelain output at all
/// — a stray informational line, a truncated read — and reporting it as a
/// changed file would invent work for the user to review.
///
/// `T` is a typechange (a symlink replaced by a file, say) and `U` an
/// unmerged path; both are uncommon but real.
bool isStatusLetter(QChar ch)
{
    static const QString letters = QStringLiteral(" MADRCUT?!");
    return letters.contains(ch);
}

}  // namespace

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
int GitStatusSummary::stagedCount() const
{
    int count = 0;
    for (const GitEntry& e : entries) {
        if (e.hasStagedChanges()) {
            ++count;
        }
    }
    return count;
}

int GitStatusSummary::unstagedCount() const
{
    int count = 0;
    for (const GitEntry& e : entries) {
        if (e.hasUnstagedChanges()) {
            ++count;
        }
    }
    return count;
}

int GitStatusSummary::untrackedCount() const
{
    int count = 0;
    for (const GitEntry& e : entries) {
        if (e.isUntracked()) {
            ++count;
        }
    }
    return count;
}

QString GitStatusSummary::label() const
{
    QString text = detached ? QStringLiteral("(detached HEAD)") : branch;
    if (ahead > 0) {
        text += QStringLiteral(" \u2191%1").arg(ahead);   // ↑
    }
    if (behind > 0) {
        text += QStringLiteral(" \u2193%1").arg(behind);  // ↓
    }
    if (!entries.isEmpty()) {
        text += QStringLiteral(" \u25CF");                // ● — dirty tree
    }
    return text.trimmed();
}

QString describeGitStatus(const GitStatusSummary& summary)
{
    if (summary.entries.isEmpty()) {
        return QStringLiteral("working tree clean");
    }
    QStringList parts;
    if (const int staged = summary.stagedCount(); staged > 0) {
        parts << QStringLiteral("%1 staged").arg(staged);
    }
    if (const int unstaged = summary.unstagedCount(); unstaged > 0) {
        parts << QStringLiteral("%1 not staged").arg(unstaged);
    }
    if (const int untracked = summary.untrackedCount(); untracked > 0) {
        parts << QStringLiteral("%1 untracked").arg(untracked);
    }
    return parts.join(QStringLiteral(", "));
}

GitStatusSummary parseGitStatus(const QByteArray& porcelainOutput)
{
    GitStatusSummary summary;
    const QString text = QString::fromUtf8(porcelainOutput);
    const QStringList lines = text.split(u'\n');

    bool headerSeen = false;
    for (const QString& rawLine : lines) {
        const QString line = rawLine.endsWith(u'\r')
                                 ? rawLine.left(rawLine.size() - 1)
                                 : rawLine;
        if (line.isEmpty()) {
            continue;
        }
        if (!headerSeen && line.startsWith(QStringLiteral("## "))) {
            headerSeen = true;
            parseBranchHeader(line.mid(3), summary);
            continue;
        }
        // "XY path" — two status columns, a space, then the path.
        if (line.size() < 3 || !isStatusLetter(line.at(0))
            || !isStatusLetter(line.at(1))) {
            continue;
        }
        GitEntry entry;
        entry.indexStatus = line.at(0);
        entry.workTreeStatus = line.at(1);
        QString rest = line.mid(3);
        if (const qsizetype arrow = rest.indexOf(QStringLiteral(" -> ")); arrow >= 0) {
            entry.originalPath = unquotePath(rest.left(arrow));
            rest = rest.mid(arrow + 4);
        }
        entry.path = unquotePath(rest);
        summary.entries.append(entry);
    }
    return summary;
}

}  // namespace LightCpp::Project
