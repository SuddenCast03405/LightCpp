// CppTokenScan — see the header for why this is one pass and not three.

#include "LightCpp/App/CppTokenScan.hpp"

namespace LightCpp::App::CppTokenScan {
namespace {

/// Index of the `*` of the `*/` that closes a block comment, or -1.
int closingBlockComment(const QString& text, int from)
{
    const int size = static_cast<int>(text.size());
    for (int i = from; i + 1 < size; ++i) {
        if (text[i] == u'*' && text[i + 1] == u'/') {
            return i;
        }
    }
    return -1;
}

/// Index of the quote that closes the literal opened before `from`, or -1.
/// An escape eats the character after it, so `"a\"b"` is one literal and
/// `'\''` is one char literal.
int closingQuote(const QString& text, int from, QChar quote)
{
    const int size = static_cast<int>(text.size());
    for (int i = from; i < size; ++i) {
        const QChar c = text[i];
        if (c == u'\\') {
            ++i;
            continue;
        }
        if (c == quote) {
            return i;
        }
    }
    return -1;
}

/// A `'` is a character literal unless it is a C++14 digit separator
/// (`1'000'000`, `0xFF'FF`): a separator always follows a digit. No
/// character literal does — `'a'` follows nothing or punctuation, and the
/// prefixed forms (`L'a'`, `u8'a'`) follow a letter.
bool startsCharLiteral(const QString& text, int index)
{
    return index == 0 || !text[index - 1].isDigit();
}

}  // namespace

Line scan(const QString& text, bool startsInsideBlockComment)
{
    Line line;
    const int size = static_cast<int>(text.size());
    int i = 0;

    if (startsInsideBlockComment) {
        const int close = closingBlockComment(text, 0);
        if (close < 0) {
            // Still inside it. A blank line contributes no span — there is
            // nothing to paint — but the state must survive it.
            if (size > 0) {
                line.spans.append(Span{0, size, Kind::BlockComment});
            }
            line.endsInsideBlockComment = true;
            return line;
        }
        line.spans.append(Span{0, close + 2, Kind::BlockComment});
        i = close + 2;
    }

    while (i < size) {
        const QChar c = text[i];

        if (c == u'/' && i + 1 < size) {
            if (text[i + 1] == u'/') {
                line.spans.append(Span{i, size - i, Kind::LineComment});
                break;
            }
            if (text[i + 1] == u'*') {
                const int close = closingBlockComment(text, i + 2);
                if (close < 0) {
                    line.spans.append(Span{i, size - i, Kind::BlockComment});
                    line.endsInsideBlockComment = true;
                    break;
                }
                line.spans.append(Span{i, close + 2 - i, Kind::BlockComment});
                i = close + 2;
                continue;
            }
            ++i;
            continue;
        }

        if (c == u'"' || (c == u'\'' && startsCharLiteral(text, i))) {
            const int close = closingQuote(text, i + 1, c);
            if (close < 0) {
                // Unclosed on this line: not a literal, just a stray quote.
                ++i;
                continue;
            }
            line.spans.append(Span{i, close + 1 - i, Kind::Literal});
            i = close + 1;
            continue;
        }

        ++i;
    }

    return line;
}

bool overlapsAny(const QList<Span>& spans, int start, int length)
{
    const int end = start + length;
    for (const Span& span : spans) {
        if (span.start >= end) {
            return false;  // sorted by start: nothing further along can overlap
        }
        if (span.end() > start) {
            return true;
        }
    }
    return false;
}

}  // namespace LightCpp::App::CppTokenScan
