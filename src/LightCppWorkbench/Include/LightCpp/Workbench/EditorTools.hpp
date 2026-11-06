// EditorTools — text transforms that belong to the editor, not to a widget.
//
// Three jobs lightcpp.1.md §六 names and that are all "rewrite a buffer
// without losing the user's structure":
//
//   * Include sorting. The naive version (sort every `#include` line) breaks
//     code: it moves an include out of the `#ifdef _WIN32` it belongs to, and
//     it separates `#include "config.h"` from the comment that explains it.
//     This implementation only sorts *runs* that are contiguous at the same
//     conditional depth, and only ever moves whole lines with their trailing
//     comment.
//   * Line diff for the gutter's change marks (§八 "行内修改标记"). A buffer is
//     compared against the text as it was read from disk; the result is one
//     status per line, which is what a gutter paints.
//   * Include-path normalisation for "add include" and for the include
//     completion fallback.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::Editor {

// ---------------------------------------------------------------------------
// Include sorting
// ---------------------------------------------------------------------------

/// One `#include` line, as parsed.
struct IncludeDirective {
    int line = 0;            ///< 0-based line index
    QString spelling;        ///< the whole line, without the newline
    QString path;            ///< `foo/bar.h`
    bool system = false;     ///< <...> rather than "..."
    QString trailingComment; ///< `// why`, kept attached to the line
};

/// Parse the includes of a translation unit.
[[nodiscard]] QList<IncludeDirective> parseIncludes(const QString& text);

/// How to order two includes.
enum class IncludeOrder {
    /// `"a.h"` before `<b.h>`, each group alphabetised — the LLVM/clang-format
    /// default (`SortIncludes: true` with `IncludeBlocks: Regroup`).
    SystemLast,
    /// `<a.h>` before `"b.h"`.
    SystemFirst,
    /// Pure alphabetical, ignoring the bracket style.
    Alphabetical,
};

/// Options for sortIncludes().
struct IncludeSortOptions {
    IncludeOrder order = IncludeOrder::SystemLast;
    bool caseSensitive = false;
    /// Keep a blank line between the `"..."` and `<...>` groups. When false
    /// the groups are merged into one block.
    bool separateGroups = true;
    /// Sort the body of a contiguous run even when the run is interrupted by
    /// a comment line. A comment line that directly precedes an include with
    /// no blank line between them is treated as belonging to it, so the pair
    /// moves together — that is the behaviour clang-format has and the one
    /// users expect for `// platform headers` banners.
    bool keepCommentsWithNextInclude = true;
};

/// The outcome of a sort: the new text plus what changed, so the caller can
/// report "12 includes sorted in 3 groups" and skip a no-op edit.
struct IncludeSortResult {
    QString text;
    int sortedIncludes = 0;
    int groups = 0;
    bool changed = false;
};

/// Sort the includes of `text`.
///
/// Runs are delimited by: a blank line, a non-include non-comment line, a
/// preprocessor conditional (`#if`/`#ifdef`/`#else`/`#elif`/`#endif`), or a
/// change of indentation (an include inside a namespace is left alone).
/// Sorting never crosses one of those boundaries, because moving an include
/// across `#endif` changes what the file compiles to.
[[nodiscard]] IncludeSortResult sortIncludes(
    const QString& text, const IncludeSortOptions& options = {});

/// True when the line is an `#include` (after leading whitespace).
[[nodiscard]] bool isIncludeLine(const QString& line);

/// True when the line is a preprocessor conditional that bounds an include
/// run.
[[nodiscard]] bool isConditionalDirective(const QString& line);

// ---------------------------------------------------------------------------
// Line diff (gutter change marks)
// ---------------------------------------------------------------------------

/// What happened to one line of the *current* buffer relative to the baseline.
enum class LineChange {
    Unchanged = 0,
    Added,     ///< no counterpart in the baseline
    Modified,  ///< paired with a baseline line, different text
};

/// One gutter mark. `line` is 0-based in the current buffer.
struct LineMark {
    int line = 0;
    LineChange change = LineChange::Unchanged;
};

/// Per-line marks for `current` against `baseline`.
///
/// The algorithm is a common-prefix/suffix trim followed by an LCS over the
/// differing middle, capped at 4 million cell comparisons. Past the cap the
/// middle is reported as modified rather than diffed: a wrong-but-bounded
/// answer beats a frozen editor on a file that was replaced wholesale.
[[nodiscard]] QList<LineMark> diffLines(const QStringList& baseline,
                                        const QStringList& current);

/// Convenience over whole texts; the trailing newline is not significant.
[[nodiscard]] QList<LineMark> diffText(const QString& baseline,
                                       const QString& current);

/// The number of changed lines (Added + Modified), for a status bar.
[[nodiscard]] int changedLineCount(const QList<LineMark>& marks);

// ---------------------------------------------------------------------------
// Includes: path helpers
// ---------------------------------------------------------------------------

/// Normalise an include path for comparison: forward slashes, no leading
/// `./`, case-folded on Windows only.
[[nodiscard]] QString normalizeIncludePath(const QString& path);

/// True when `path` names a header the editor can open (.h/.hpp/.hh/.hxx/
/// .inl/.ipp and the C++ module extensions).
[[nodiscard]] bool isHeaderPath(const QString& path);

}  // namespace LightCpp::Workbench::Editor
