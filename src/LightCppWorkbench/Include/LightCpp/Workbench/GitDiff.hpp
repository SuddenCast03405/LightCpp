// GitDiff — reading a diff well enough to *show* it, and enough blame to
// annotate a file (lightcpp.1.md §八: "行内修改标记", "blame", "diff 查看").
//
// Two things live here, and they are the same problem seen twice:
//
//   * A unified diff is line-oriented, but a line that changed by one
//     character — a typo fix, a renamed variable — reads as "whole line
//     replaced" in a line diff. `inlineDiff` narrows it to the span that
//     actually changed, which is what a gutter marker and an inline
//     highlight both need.
//   * A side-by-side view needs the rows aligned before it can draw
//     anything, and that alignment is not the same question as the inline
//     spans. `alignSides` answers it.
//
// The diff *text* itself is parsed by `Git::parseUnifiedDiff` in GitModel —
// this file takes that model and adds what a renderer needs. Everything is
// pure text, so all of it is testable without a repository.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "LightCpp/Workbench/GitModel.hpp"

namespace LightCpp::Workbench::Git {

// ---------------------------------------------------------------------------
// Inline (character-level) diff
// ---------------------------------------------------------------------------

/// One span of a line that differs from its counterpart. Columns are 0-based
/// UTF-16 offsets, matching QTextCursor so a highlight needs no conversion.
struct InlineSpan {
    int start = 0;
    int length = 0;

    [[nodiscard]] bool operator==(const InlineSpan& other) const
    {
        return start == other.start && length == other.length;
    }
};

/// The spans that differ between `before` and `after`, one list per side.
///
/// The algorithm is the classic prefix/suffix trim followed by a longest
/// common subsequence over what is left. The trim is what keeps a typo fix
/// cheap: two 400-character lines that share 390 characters only run the LCS
/// over the ten that differ — and a diff viewer meets that case on every
/// scroll.
///
/// An unchanged pair yields two empty lists, which is the caller's signal to
/// draw a context row rather than a change row.
void inlineDiff(const QString& before, const QString& after,
                QList<InlineSpan>* beforeSpans, QList<InlineSpan>* afterSpans);

/// What a hunk line is. Only the three kinds a renderer draws: the
/// "\ No newline at end of file" marker is metadata about the line above, not
/// a line.
enum class DiffLineKind {
    Context,
    Removed,
    Added,
};

/// Merge spans that are within `gap` characters of each other, so a highlight
/// does not flicker between nearly-touching runs, and sort them.
[[nodiscard]] QList<InlineSpan> mergeSpans(const QList<InlineSpan>& spans, int gap = 2);

// ---------------------------------------------------------------------------
// Side-by-side alignment
// ---------------------------------------------------------------------------

/// One row of a two-column view. `left` and `right` may both be set (a context
/// row) or exactly one (a change), or neither (a filler/separator row).
struct SideBySideRow {
    int oldLine = 0;   ///< 1-based; 0 when this side has no line
    int newLine = 0;
    QString left;
    QString right;
    bool leftChanged = false;
    bool rightChanged = false;
    /// A row that exists only to keep the columns from sliding: it carries no
    /// line on either side and gets no gutter number. A row with a line on
    /// only one side is a half row, not filler.
    bool filler = false;
    /// The "⋯" separator between hunks, produced by `alignDiff`. Drawn as a
    /// row, never as a line.
    bool gap = false;
};

/// Pair the removals with the additions inside one hunk.
///
/// git writes all the `-` lines of a run and then all the `+` lines. Zipping
/// them row by row is the alignment every side-by-side viewer uses, and it is
/// the one a human reads fastest: the first removed line sits next to the
/// first added one. The shorter block is padded with filler rows so the right
/// column does not slide up under a block of removals.
[[nodiscard]] QList<SideBySideRow> alignSides(const DiffHunk& hunk);

/// Align every hunk of a file and put a gap row between them. A file with one
/// hunk produces no gap row.
[[nodiscard]] QList<SideBySideRow> alignDiff(const QList<DiffFile>& files);

// ---------------------------------------------------------------------------
// Blame lookup for the gutter
// ---------------------------------------------------------------------------

/// Answers "which commit owns this line?" from a `--porcelain` blame.
///
/// The lookup is forgiving on purpose: a buffer edited since the blame was
/// taken has more lines than the blame knows about, and the honest answer for
/// those is "uncommitted" rather than the last known author. A GUI shows the
/// same thing for a modified-but-uncommitted region, so the two cases read
/// identically instead of one of them being a crash.
class BlameLookup {
public:
    BlameLookup() = default;
    explicit BlameLookup(QList<BlameLine> lines);

    [[nodiscard]] bool isEmpty() const { return lines_.isEmpty(); }
    [[nodiscard]] qsizetype size() const { return lines_.size(); }

    /// The entry for a 1-based line, or nullptr when the line is past the end
    /// of the blame or before the start of the file.
    [[nodiscard]] const BlameLine* line(int lineNumber) const;

    /// `author  2024-01-02` — the gutter's two columns collapsed into the one
    /// string a narrow margin can carry. An author longer than `authorWidth`
    /// is elided with an ellipsis rather than pushing the date out of view.
    [[nodiscard]] QString gutterText(int lineNumber, int authorWidth = 14) const;

    /// A stable colour index for the commit that owns `lineNumber`, so two
    /// adjacent blocks from different authors are visibly different. -1 when
    /// the line has no blame.
    [[nodiscard]] int colourIndex(int lineNumber) const;

    /// The commit hash of the owning line, shortened, or the literal
    /// "uncommitted" for a line the blame does not cover.
    [[nodiscard]] QString shortSha(int lineNumber) const;

    /// The commit subject, for a tooltip. Empty when unknown.
    [[nodiscard]] QString summary(int lineNumber) const;

private:
    QList<BlameLine> lines_;
};

/// Shorten a hash the way git does (7 characters). The caller passes the
/// result of `git rev-parse --short` when it has one; this is the fallback for
/// a hash that came out of a status or a worktree listing.
[[nodiscard]] QString shortHash(const QString& sha);

}  // namespace LightCpp::Workbench::Git
