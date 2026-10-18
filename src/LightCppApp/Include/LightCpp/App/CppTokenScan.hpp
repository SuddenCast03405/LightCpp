// CppTokenScan — which parts of one line are comments and literals.
//
// The highlighter used to answer that question three separate times, each in
// its own pass and each blind to the other two:
//
//   * the string rules ran over the whole line and painted green;
//   * a `/*` search took the first `/*` anywhere, including one inside a
//     string — `s = "/*";` therefore opened a block comment that swallowed
//     every following line to the end of the file;
//   * the `//` search walked the line and skipped positions it knew to be
//     inside a literal, but the literals it knew about came from a regex that
//     cross-matches (`'(?:[^'\\]|\\.)*'` happily spans `"don't"`), and blocked
//     nothing else: a `//` after an unclosed string still turned the line grey.
//
// The visible result of the first of those is what the report called "string
// literals coloured twice": colours are merged per character, and the string
// format only sets a foreground. A keyword inside a literal was painted bold
// red and then recoloured green, which keeps the *width* — `"if (x)"` showed a
// bold green `if`. Skipping the word passes inside literals fixes that at the
// root; repainting over them cannot.
//
// So the line is scanned once, left to right, and the answer is a list of
// spans. Everything downstream — the keyword/type passes, the number and
// identifier rules, the comment colours — asks this one list whether a range
// is free before painting it. The keyword and type *patterns* are untouched:
// one merged alternation per class with a hash lookup is what keeps large
// files from stuttering, and that structure must stay.

#pragma once

#include <QList>
#include <QString>

namespace LightCpp::App::CppTokenScan {

enum class Kind {
    Literal,       ///< "..." or '...', including the quotes.
    LineComment,   ///< `//` to the end of the line.
    BlockComment,  ///< `/* ... */`, or the tail of one that runs past the line.
};

struct Span {
    int start = 0;
    int length = 0;
    Kind kind = Kind::Literal;

    [[nodiscard]] int end() const { return start + length; }
};

struct Line {
    /// Non-overlapping, sorted by start.
    QList<Span> spans;
    /// The line ended inside a `/* ... */`: the next line starts in a comment.
    bool endsInsideBlockComment = false;
};

/// Scan one line. `startsInsideBlockComment` is the previous line's
/// `endsInsideBlockComment`, which is how a block comment crosses lines —
/// the only state the highlighter carries between blocks.
///
/// A quote with no closing partner on the same line is NOT a literal. C++
/// literals cannot contain a raw newline, so an unclosed quote is either a
/// mistake or an apostrophe/`'` digit separator; swallowing the rest of the
/// line as green (and thereby hiding a real `//` in it) would be worse than
/// leaving that one character uncoloured.
[[nodiscard]] Line scan(const QString& text, bool startsInsideBlockComment);

/// True when [start, start+length) touches any span in `spans` — a match
/// that starts before a literal but reaches into it (`1"a"` and its like)
/// touches it just as much as one that starts inside.
///
/// A line carries a handful of spans (a comment, one or two literals), so a
/// plain walk beats any index here; what matters is that the word passes ask
/// at all.
[[nodiscard]] bool overlapsAny(const QList<Span>& spans, int start, int length);

}  // namespace LightCpp::App::CppTokenScan
