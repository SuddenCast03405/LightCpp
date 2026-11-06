// EditorTools — implementation. See the header for the rules, especially the
// boundaries include sorting refuses to cross.

#include "LightCpp/Workbench/EditorTools.hpp"

#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace LightCpp::Workbench::Editor {
namespace {

const QRegularExpression& includePattern()
{
    // `#  include"a.h"` is legal C; so is a trailing comment. The path and the
    // comment are captured separately so both survive a sort.
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*#\s*include\s*([<"])([^>"]+)[>"]\s*(?://\s*(.*))?$)"));
    return pattern;
}

int leadingSpaces(const QString& line)
{
    int count = 0;
    for (const QChar ch : line) {
        if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t')) {
            break;
        }
        count += ch == QLatin1Char('\t') ? 4 : 1;
    }
    return count;
}

bool isBlank(const QString& line)
{
    return line.trimmed().isEmpty();
}

bool isCommentLine(const QString& line)
{
    const QString trimmed = line.trimmed();
    return trimmed.startsWith(QLatin1String("//"))
           || trimmed.startsWith(QLatin1String("/*"))
           || trimmed.startsWith(QLatin1String("*"))
           || trimmed.endsWith(QLatin1String("*/"));
}

/// A unit of a run: an include with the comment lines that belong to it.
struct Unit {
    QStringList lines;
    QString sortKey;
};

}  // namespace

bool isIncludeLine(const QString& line)
{
    return includePattern().match(line).hasMatch();
}

bool isConditionalDirective(const QString& line)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b)"));
    return pattern.match(line).hasMatch();
}

QList<IncludeDirective> parseIncludes(const QString& text)
{
    QList<IncludeDirective> includes;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const auto match = includePattern().match(lines.at(i));
        if (!match.hasMatch()) {
            continue;
        }
        IncludeDirective directive;
        directive.line = i;
        directive.spelling = lines.at(i);
        directive.system = match.captured(1) == QLatin1String("<");
        directive.path = match.captured(2).trimmed();
        directive.trailingComment = match.captured(3);
        includes.append(directive);
    }
    return includes;
}

IncludeSortResult sortIncludes(const QString& text, const IncludeSortOptions& options)
{
    IncludeSortResult result;
    result.text = text;

    // A file with no includes, or with a BOM/short file, has nothing to do.
    QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() < 2) {
        return result;
    }

    // The leading text before the first include is not part of any run.
    int index = 0;
    while (index < lines.size()) {
        if (!isIncludeLine(lines.at(index))) {
            ++index;
            continue;
        }
        // A run starts at an include that begins a "block": the previous line
        // is blank, is a conditional, or is not an include/comment.
        const int runStart = index;
        const int runIndent = leadingSpaces(lines.at(index));
        int runEnd = index;  // exclusive

        // Collect the units of the run.
        QList<Unit> units;
        int cursor = index;
        QStringList pendingComments;
        while (cursor < lines.size()) {
            const QString& line = lines.at(cursor);
            if (isIncludeLine(line)) {
                if (leadingSpaces(line) != runIndent) {
                    break;
                }
                Unit unit;
                if (options.keepCommentsWithNextInclude && !pendingComments.isEmpty()) {
                    unit.lines = pendingComments;
                    pendingComments.clear();
                }
                unit.lines.append(line);
                const auto match = includePattern().match(line);
                QString key = match.captured(2).trimmed();
                if (!options.caseSensitive) {
                    key = key.toLower();
                }
                unit.sortKey = key;
                units.append(unit);
                ++cursor;
                continue;
            }
            if (isBlank(line) || isConditionalDirective(line) || isCommentLine(line)) {
                // A blank line ends the run; a conditional always does.
                if (isBlank(line) || isConditionalDirective(line)) {
                    break;
                }
                pendingComments.append(line);
                ++cursor;
                continue;
            }
            break;  // ordinary code: the run is over
        }
        runEnd = cursor;
        // Comments left over at the end of the run stay where they are.
        const QStringList trailingComments = pendingComments;

        if (units.size() < 2) {
            index = qMax(runEnd, runStart + 1);
            continue;
        }

        // Sorting: split by bracket style when asked, and stable-sort each
        // group by path. `std::stable_sort` keeps two includes of the same
        // header in their original order, which matters for `#include`
        // inside `#if` ladders that this code cannot see.
        auto groupOf = [&](const Unit& unit) {
            if (options.order == IncludeOrder::Alphabetical
                || !options.separateGroups) {
                return 0;
            }
            const bool system = unit.lines.last().trimmed().contains(QLatin1Char('<'));
            if (options.order == IncludeOrder::SystemFirst) {
                return system ? 0 : 1;
            }
            return system ? 1 : 0;
        };
        std::stable_sort(units.begin(), units.end(), [&](const Unit& a, const Unit& b) {
            const int ga = groupOf(a);
            const int gb = groupOf(b);
            if (ga != gb) {
                return ga < gb;
            }
            return a.sortKey < b.sortKey;
        });

        // Rebuild the run in place. The old lines are replaced 1:1, so line
        // numbers after the run do not move — that is what lets several runs
        // be sorted in one pass.
        QStringList rebuilt;
        int previousGroup = -1;
        for (const Unit& unit : units) {
            const int group = groupOf(unit);
            if (options.separateGroups && previousGroup >= 0 && group != previousGroup) {
                rebuilt.append(QString());
            }
            previousGroup = group;
            rebuilt.append(unit.lines);
        }
        rebuilt.append(trailingComments);

        const int oldLength = runEnd - runStart;
        const int newLength = rebuilt.size();
        // Replace the slice even when lengths differ: the run is rebuilt from
        // its own lines, so the count only changes when a group separator was
        // added or a blank line was consumed.
        lines.erase(lines.begin() + runStart, lines.begin() + runEnd);
        for (int i = 0; i < newLength; ++i) {
            lines.insert(runStart + i, rebuilt.at(i));
        }

        result.sortedIncludes += units.size();
        ++result.groups;
        index = runStart + newLength + (newLength - oldLength > 0
                                            ? newLength - oldLength
                                            : 0);
    }

    result.text = lines.join(QLatin1Char('\n'));
    result.changed = result.text != text;
    return result;
}

// ---------------------------------------------------------------------------
// Line diff
// ---------------------------------------------------------------------------

QList<LineMark> diffLines(const QStringList& baseline, const QStringList& current)
{
    QList<LineMark> marks;
    marks.reserve(current.size());
    for (int i = 0; i < current.size(); ++i) {
        LineMark mark;
        mark.line = i;
        mark.change = LineChange::Unchanged;
        marks.append(mark);
    }

    // Trim the common prefix and suffix first: for the usual edit (a few lines
    // in a big file) this alone reduces the problem to almost nothing.
    int prefix = 0;
    while (prefix < baseline.size() && prefix < current.size()
           && baseline.at(prefix) == current.at(prefix)) {
        ++prefix;
    }
    int suffix = 0;
    while (suffix < baseline.size() - prefix && suffix < current.size() - prefix
           && baseline.at(baseline.size() - 1 - suffix)
                  == current.at(current.size() - 1 - suffix)) {
        ++suffix;
    }

    const int oldCount = baseline.size() - prefix - suffix;
    const int newCount = current.size() - prefix - suffix;
    if (oldCount == 0 && newCount == 0) {
        return marks;
    }
    if (oldCount == 0) {
        for (int i = prefix; i < prefix + newCount; ++i) {
            marks[i].change = LineChange::Added;
        }
        return marks;
    }

    // LCS over the middle. The cap is a hard guard: 4M cells is a few tens of
    // milliseconds, and past it the file was replaced rather than edited.
    constexpr qint64 kMaxCells = 4'000'000;
    if (static_cast<qint64>(oldCount) * static_cast<qint64>(newCount) > kMaxCells) {
        for (int i = prefix; i < prefix + newCount; ++i) {
            marks[i].change = LineChange::Modified;
        }
        return marks;
    }

    // dp[i][j] = LCS length of baseline[prefix+i..] and current[prefix+j..]
    QList<QList<int>> dp(oldCount + 1, QList<int>(newCount + 1, 0));
    for (int i = oldCount - 1; i >= 0; --i) {
        for (int j = newCount - 1; j >= 0; --j) {
            if (baseline.at(prefix + i) == current.at(prefix + j)) {
                dp[i][j] = dp[i + 1][j + 1] + 1;
            } else {
                dp[i][j] = qMax(dp[i + 1][j], dp[i][j + 1]);
            }
        }
    }

    // Walk the LCS once. A deletion with no current line to mark is
    // remembered; the insertion that follows it is the same edit, so it is
    // reported as Modified rather than Added — that is what a one-line edit
    // looks like in every other editor's gutter.
    int i = 0;
    int j = 0;
    int pendingDeletions = 0;
    while (i < oldCount && j < newCount) {
        if (baseline.at(prefix + i) == current.at(prefix + j)) {
            ++i;
            ++j;
            continue;
        }
        if (dp[i + 1][j] >= dp[i][j + 1]) {
            ++pendingDeletions;
            ++i;
            continue;
        }
        marks[prefix + j].change = pendingDeletions > 0
            ? LineChange::Modified
            : LineChange::Added;
        if (pendingDeletions > 0) {
            --pendingDeletions;
        }
        ++j;
    }
    // Trailing deletions pair with the insertions that are still to come.
    while (i < oldCount) {
        ++pendingDeletions;
        ++i;
    }
    while (j < newCount) {
        marks[prefix + j].change = pendingDeletions > 0
            ? LineChange::Modified
            : LineChange::Added;
        if (pendingDeletions > 0) {
            --pendingDeletions;
        }
        ++j;
    }
    return marks;
}

QList<LineMark> diffText(const QString& baseline, const QString& current)
{
    QString left = baseline;
    QString right = current;
    // A trailing newline is a file convention, not a line of its own.
    if (left.endsWith(QLatin1Char('\n'))) {
        left.chop(1);
    }
    if (right.endsWith(QLatin1Char('\n'))) {
        right.chop(1);
    }
    return diffLines(left.split(QLatin1Char('\n')), right.split(QLatin1Char('\n')));
}

int changedLineCount(const QList<LineMark>& marks)
{
    int count = 0;
    for (const LineMark& mark : marks) {
        if (mark.change != LineChange::Unchanged) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

QString normalizeIncludePath(const QString& path)
{
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (normalized.startsWith(QLatin1String("./"))) {
        normalized.remove(0, 2);
    }
#ifdef Q_OS_WIN
    normalized = normalized.toLower();
#endif
    return normalized;
}

bool isHeaderPath(const QString& path)
{
    static const QStringList suffixes = {
        QStringLiteral("h"), QStringLiteral("hpp"), QStringLiteral("hh"),
        QStringLiteral("hxx"), QStringLiteral("h++"), QStringLiteral("inl"),
        QStringLiteral("ipp"), QStringLiteral("tpp"), QStringLiteral("ixx"),
        QStringLiteral("cppm"),
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

}  // namespace LightCpp::Workbench::Editor
