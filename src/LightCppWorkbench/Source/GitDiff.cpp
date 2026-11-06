// GitDiff — implementation. See the header for why each piece exists.

#include "LightCpp/Workbench/GitDiff.hpp"

#include <QDate>

#include <algorithm>

namespace LightCpp::Workbench::Git {

namespace {

/// One line of a hunk, with its numbers resolved. A private shape: the model
/// in GitModel keeps the hunk's lines as raw "+"/"-" strings, which is right
/// for storage and useless for alignment.
struct DiffLine {
    DiffLineKind kind = DiffLineKind::Context;
    int oldLine = 0;
    int newLine = 0;
    QString text;
};

/// One span covering `[start, start + length)`.
InlineSpan span(int start, int length)
{
    InlineSpan result;
    result.start = start;
    result.length = length;
    return result;
}

/// LCS over the differing middles. The table is `(n+1) * (m+1)` ints; the
/// lines this runs on are short by construction (the common prefix and suffix
/// were already trimmed), so the memory is bounded by the size of the change
/// rather than by the size of the line.
void lcsSpans(const QString& before, int beforeStart, int beforeEnd,
              const QString& after, int afterStart, int afterEnd,
              QList<InlineSpan>* beforeSpans, QList<InlineSpan>* afterSpans)
{
    const int n = beforeEnd - beforeStart;
    const int m = afterEnd - afterStart;
    if (n == 0 && m == 0) {
        return;
    }
    if (n == 0) {
        afterSpans->append(span(afterStart, m));
        return;
    }
    if (m == 0) {
        beforeSpans->append(span(beforeStart, n));
        return;
    }
    // A guard on the pathological case: two unrelated thousand-character
    // lines would need a million cells. Past that, report the whole middle as
    // changed — which is both cheap and true.
    constexpr int kMaxCells = 400000;
    if (qsizetype(n) * qsizetype(m) > kMaxCells) {
        beforeSpans->append(span(beforeStart, n));
        afterSpans->append(span(afterStart, m));
        return;
    }

    QList<int> table((n + 1) * (m + 1), 0);
    const auto at = [&](int i, int j) -> int& { return table[i * (m + 1) + j]; };
    for (int i = n - 1; i >= 0; --i) {
        for (int j = m - 1; j >= 0; --j) {
            if (before.at(beforeStart + i) == after.at(afterStart + j)) {
                at(i, j) = at(i + 1, j + 1) + 1;
            } else {
                at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
            }
        }
    }

    // Walk the table, accumulating runs of characters that are *not* part of
    // the common subsequence. Runs become spans, which is what keeps a
    // highlight from being one span per character.
    int i = 0;
    int j = 0;
    int runStartBefore = -1;
    int runStartAfter = -1;
    const auto flushBefore = [&]() {
        if (runStartBefore >= 0) {
            beforeSpans->append(span(beforeStart + runStartBefore, i - runStartBefore));
            runStartBefore = -1;
        }
    };
    const auto flushAfter = [&]() {
        if (runStartAfter >= 0) {
            afterSpans->append(span(afterStart + runStartAfter, j - runStartAfter));
            runStartAfter = -1;
        }
    };
    while (i < n && j < m) {
        if (before.at(beforeStart + i) == after.at(afterStart + j)) {
            flushBefore();
            flushAfter();
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            if (runStartBefore < 0) {
                runStartBefore = i;
            }
            ++i;
        } else {
            if (runStartAfter < 0) {
                runStartAfter = j;
            }
            ++j;
        }
    }
    if (i < n) {
        if (runStartBefore < 0) {
            runStartBefore = i;
        }
        i = n;
    }
    if (j < m) {
        if (runStartAfter < 0) {
            runStartAfter = j;
        }
        j = m;
    }
    flushBefore();
    flushAfter();
}

}  // namespace

void inlineDiff(const QString& before, const QString& after,
                QList<InlineSpan>* beforeSpans, QList<InlineSpan>* afterSpans)
{
    QList<InlineSpan> left;
    QList<InlineSpan> right;

    const int beforeLength = int(before.size());
    const int afterLength = int(after.size());

    int prefix = 0;
    while (prefix < beforeLength && prefix < afterLength
           && before.at(prefix) == after.at(prefix)) {
        ++prefix;
    }
    int suffix = 0;
    while (suffix < beforeLength - prefix && suffix < afterLength - prefix
           && before.at(beforeLength - 1 - suffix) == after.at(afterLength - 1 - suffix)) {
        ++suffix;
    }

    lcsSpans(before, prefix, beforeLength - suffix, after, prefix, afterLength - suffix,
             &left, &right);

    if (beforeSpans != nullptr) {
        *beforeSpans = mergeSpans(left);
    }
    if (afterSpans != nullptr) {
        *afterSpans = mergeSpans(right);
    }
}

QList<InlineSpan> mergeSpans(const QList<InlineSpan>& spans, int gap)
{
    QList<InlineSpan> sorted;
    sorted.reserve(spans.size());
    for (const InlineSpan& s : spans) {
        if (s.length > 0) {
            sorted.append(s);
        }
    }
    if (sorted.size() < 2) {
        return sorted;
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const InlineSpan& a, const InlineSpan& b) { return a.start < b.start; });

    QList<InlineSpan> merged;
    merged.append(sorted.first());
    for (qsizetype i = 1; i < sorted.size(); ++i) {
        InlineSpan& last = merged.last();
        const int lastEnd = last.start + last.length;
        if (sorted.at(i).start - lastEnd <= gap) {
            const int newEnd = std::max(lastEnd, sorted.at(i).start + sorted.at(i).length);
            last.length = newEnd - last.start;
        } else {
            merged.append(sorted.at(i));
        }
    }
    return merged;
}

QList<SideBySideRow> alignSides(const DiffHunk& hunk)
{
    QList<SideBySideRow> rows;
    QList<DiffLine> pendingRemoved;
    QList<DiffLine> pendingAdded;

    int oldNumber = hunk.oldStart;
    int newNumber = hunk.newStart;

    const auto flush = [&]() {
        const qsizetype count = std::max(pendingRemoved.size(), pendingAdded.size());
        for (qsizetype i = 0; i < count; ++i) {
            SideBySideRow row;
            if (i < pendingRemoved.size()) {
                row.oldLine = pendingRemoved.at(i).oldLine;
                row.left = pendingRemoved.at(i).text;
                row.leftChanged = true;
            }
            if (i < pendingAdded.size()) {
                row.newLine = pendingAdded.at(i).newLine;
                row.right = pendingAdded.at(i).text;
                row.rightChanged = true;
            }
            // A row with a line on only one side is a half row, not filler:
            // it is the natural shape of a pure insertion or a pure deletion.
            // Marking it filler would draw a blank separator in the middle of
            // a change, which is exactly the artefact this padding exists to
            // avoid.
            row.filler = false;
            rows.append(row);
        }
        pendingRemoved.clear();
        pendingAdded.clear();
    };

    for (const QString& raw : hunk.lines) {
        const QChar marker = raw.isEmpty() ? QLatin1Char(' ') : raw.at(0);
        const QString text = raw.size() > 1 ? raw.mid(1) : QString();
        DiffLine line;
        line.text = text;
        if (marker == QLatin1Char('-')) {
            line.kind = DiffLineKind::Removed;
            line.oldLine = oldNumber++;
            pendingRemoved.append(line);
        } else if (marker == QLatin1Char('+')) {
            line.kind = DiffLineKind::Added;
            line.newLine = newNumber++;
            pendingAdded.append(line);
        } else if (marker == QLatin1Char('\\')) {
            // "\ No newline at end of file" belongs to the line above it; it
            // is not a row, and counting it would shift every number below.
            continue;
        } else {
            flush();
            SideBySideRow row;
            row.oldLine = oldNumber++;
            row.newLine = newNumber++;
            row.left = text;
            row.right = text;
            rows.append(row);
        }
    }
    flush();
    return rows;
}

QList<SideBySideRow> alignDiff(const QList<DiffFile>& files)
{
    QList<SideBySideRow> rows;
    bool first = true;
    for (const DiffFile& file : files) {
        for (const DiffHunk& hunk : file.hunks) {
            if (!first) {
                SideBySideRow gap;
                gap.gap = true;
                gap.filler = true;
                rows.append(gap);
            }
            first = false;
            rows.append(alignSides(hunk));
        }
    }
    return rows;
}

// ---------------------------------------------------------------------------
// BlameLookup
// ---------------------------------------------------------------------------

BlameLookup::BlameLookup(QList<BlameLine> lines)
    : lines_(std::move(lines))
{
}

const BlameLine* BlameLookup::line(int lineNumber) const
{
    if (lineNumber <= 0 || lineNumber > int(lines_.size())) {
        return nullptr;
    }
    return &lines_.at(lineNumber - 1);
}

QString BlameLookup::gutterText(int lineNumber, int authorWidth) const
{
    const BlameLine* entry = line(lineNumber);
    if (entry == nullptr) {
        return QStringLiteral("uncommitted");
    }
    if (entry->uncommitted) {
        return QStringLiteral("uncommitted");
    }
    QString author = entry->author;
    if (authorWidth > 1 && author.size() > authorWidth) {
        author = author.left(authorWidth - 1) + QChar(0x2026);
    }
    // The date is stored as "2024-01-01 12:00:00 +0000"; a gutter wants the
    // day. Taking the first ten characters rather than parsing the ISO date
    // keeps a malformed date visible instead of silently blanking the column.
    QString day = entry->date.left(10);
    if (day.size() != 10) {
        day = entry->date;
    }
    if (day.isEmpty()) {
        return author;
    }
    return QStringLiteral("%1  %2").arg(author, day);
}

int BlameLookup::colourIndex(int lineNumber) const
{
    const BlameLine* entry = line(lineNumber);
    if (entry == nullptr || entry->uncommitted) {
        return -1;
    }
    // FNV-1a over the hash, folded into a small palette. A stable hash means
    // the same commit keeps its colour across sessions and across files,
    // which is what makes a blame gutter readable at a glance.
    quint32 hash = 2166136261u;
    for (const QChar ch : entry->sha) {
        hash ^= quint32(ch.unicode());
        hash *= 16777619u;
    }
    return int(hash % 12u);
}

QString BlameLookup::shortSha(int lineNumber) const
{
    const BlameLine* entry = line(lineNumber);
    if (entry == nullptr || entry->uncommitted) {
        return QStringLiteral("uncommitted");
    }
    return shortHash(entry->sha);
}

QString BlameLookup::summary(int lineNumber) const
{
    const BlameLine* entry = line(lineNumber);
    return entry != nullptr ? entry->summary : QString();
}

QString shortHash(const QString& sha)
{
    const QString trimmed = sha.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    if (trimmed.size() <= 7) {
        return trimmed;
    }
    return trimmed.left(7);
}

}  // namespace LightCpp::Workbench::Git
