// CppTokenScanTests — one left-to-right answer to "what is a comment here".
//
// The highlighter used to answer that three times over (string regex, a bare
// `indexOf("/*")`, and a `//` walk that trusted the string regex), so the three
// answers disagreed and the *last* one painted. These tests pin the single
// answer, one disagreement at a time:
//
//   * `"http://x"`             — the URL must not open a line comment;
//   * `"/*"`                   — must not open a block comment that never ends;
//   * `// "done"`              — the quotes inside a comment are not a literal;
//   * `"don't"`                — the apostrophe must not join a later quote
//                                (that cross-match is what the old
//                                `'(?:[^'\\]|\\.)*'` rule did);
//   * `1'000'000`              — a digit separator is not a char literal;
//   * `"/* */ x`               — the block-comment state crosses lines.
//
// The expected spans are hand-counted from the line in the test, never taken
// from the scanner: a test that asks the code under test where the literal is
// only proves the code agrees with itself.

#include "LightCpp/App/CppTokenScan.hpp"

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

using LightCpp::App::CppTokenScan::Kind;
using LightCpp::App::CppTokenScan::Line;
using LightCpp::App::CppTokenScan::overlapsAny;
using LightCpp::App::CppTokenScan::scan;
using LightCpp::App::CppTokenScan::Span;

namespace {

/// "lit@16+10,//@8+7" — kind@start+length, in line order. Comparing this
/// string rather than a QList<Span> means a failure prints the whole layout
/// of the line instead of "two lists differ".
QString layoutOf(const Line& line)
{
    QStringList parts;
    parts.reserve(line.spans.size());
    for (const Span& span : line.spans) {
        QString kind;
        switch (span.kind) {
        case Kind::Literal:
            kind = QStringLiteral("lit");
            break;
        case Kind::LineComment:
            kind = QStringLiteral("//");
            break;
        case Kind::BlockComment:
            kind = QStringLiteral("/*");
            break;
        }
        parts.append(
            QStringLiteral("%1@%2+%3").arg(kind).arg(span.start).arg(span.length));
    }
    return parts.join(QLatin1Char(','));
}

}  // namespace

TEST(CppTokenScan, AUrlInsideAStringIsNotAComment)
{
    //            0         1         2
    //            0123456789012345678901234567
    const QString line = QStringLiteral(R"(const char* u = "http://x";)");
    ASSERT_EQ(line.size(), 27);

    const Line scanned = scan(line, false);
    EXPECT_EQ(layoutOf(scanned), QStringLiteral("lit@16+10"));
    EXPECT_FALSE(scanned.endsInsideBlockComment);
}

TEST(CppTokenScan, TheQuotesInsideACommentAreNotALiteral)
{
    //            012345678901234
    const QString line = QStringLiteral(R"(x = 1; // a "b")");
    ASSERT_EQ(line.size(), 15);

    // One span, and it starts at the first slash the compiler would too.
    EXPECT_EQ(layoutOf(scan(line, false)), QStringLiteral("//@7+8"));
}

TEST(CppTokenScan, ABlockCommentMarkerInsideAStringDoesNotOpenOne)
{
    //            0123456789012345
    const QString line = QStringLiteral(R"(s = "/*"; y = 2;)");
    ASSERT_EQ(line.size(), 16);

    // The old code found the `/*` at 5 and, with no `*/` after it, made the
    // rest of the file a comment. The literal is the whole answer.
    const Line scanned = scan(line, false);
    EXPECT_EQ(layoutOf(scanned), QStringLiteral("lit@4+4"));
    EXPECT_FALSE(scanned.endsInsideBlockComment);
}

TEST(CppTokenScan, ANeverClosedBlockCommentSwallowsTheLineAndTheNext)
{
    //            01234567890123
    const QString line = QStringLiteral("int a; /* note");
    ASSERT_EQ(line.size(), 14);

    const Line scanned = scan(line, false);
    EXPECT_EQ(layoutOf(scanned), QStringLiteral("/*@7+7"));
    EXPECT_TRUE(scanned.endsInsideBlockComment);

    // Handing that state back in is how the comment crosses the newline.
    const Line next = scan(QStringLiteral("still comment"), true);
    EXPECT_EQ(layoutOf(next), QStringLiteral("/*@0+13"));
    EXPECT_TRUE(next.endsInsideBlockComment);
}

TEST(CppTokenScan, TheLineThatClosesABlockCommentHasCodeAfterIt)
{
    //            0123456789012
    const QString line = QStringLiteral("*/ int x = 1;");
    ASSERT_EQ(line.size(), 13);

    const Line scanned = scan(line, true);
    EXPECT_EQ(layoutOf(scanned), QStringLiteral("/*@0+2"));
    EXPECT_FALSE(scanned.endsInsideBlockComment);
}

TEST(CppTokenScan, AClosedBlockCommentLeavesTheRestOfTheLineAlone)
{
    //            012345678901234
    const QString line = QStringLiteral(R"(/* a */ f("x");)");
    ASSERT_EQ(line.size(), 15);

    // Two spans, in order: the comment, then the literal three characters
    // later (f, the parenthesis, the quote). The old single `indexOf` pair
    // would have produced the first one only.
    EXPECT_EQ(layoutOf(scan(line, false)), QStringLiteral("/*@0+7,lit@10+3"));
}

TEST(CppTokenScan, AnEscapedQuoteDoesNotEndTheLiteral)
{
    //            0123456789012
    const QString line = QStringLiteral(R"(puts("a\"b");)");
    ASSERT_EQ(line.size(), 13);

    EXPECT_EQ(layoutOf(scan(line, false)), QStringLiteral("lit@5+6"));
}

TEST(CppTokenScan, ACharLiteralIsOneSpanAndAnEscapedQuoteFitsInIt)
{
    //            012345678
    const QString line = QStringLiteral(R"(c = '\'';)");
    ASSERT_EQ(line.size(), 9);

    // Four characters: quote, backslash, quote, quote. The escape eats the
    // middle quote, so the third one closes the literal.
    EXPECT_EQ(layoutOf(scan(line, false)), QStringLiteral("lit@4+4"));
}

TEST(CppTokenScan, TheApostropheInAStringDoesNotJoinTheNextQuote)
{
    //            01234567890
    const QString line = QStringLiteral(R"(s = "don't";)");
    ASSERT_EQ(line.size(), 12);

    // The old char rule `'(?:[^'\\]|\\.)*'` matched from the apostrophe at 8
    // to the quote at 10 and turned `t"` into a char literal, so the real
    // string was only half covered.
    EXPECT_EQ(layoutOf(scan(line, false)), QStringLiteral("lit@4+7"));
}

TEST(CppTokenScan, DigitSeparatorsAreNotCharLiterals)
{
    //            01234567890123456789
    const QString decimal = QStringLiteral("int big = 1'000'000;");
    ASSERT_EQ(decimal.size(), 20);
    EXPECT_EQ(layoutOf(scan(decimal, false)), QString());

    // A hex separator follows a digit-or-letter, so the apostrophe rule
    // cannot tell it apart by the preceding character alone: what saves it
    // is that no closing quote follows, which makes it a stray quote rather
    // than a literal running to the end of the line.
    const QString hex = QStringLiteral("int m = 0xFF'FF;");
    ASSERT_EQ(hex.size(), 16);
    EXPECT_EQ(layoutOf(scan(hex, false)), QString());
}

TEST(CppTokenScan, AnUnclosedQuoteIsNotALiteral)
{
    //            01234567
    const QString line = QStringLiteral(R"(s = "abc)");
    ASSERT_EQ(line.size(), 8);

    // A C++ literal cannot hold a raw newline. Treating this as a literal
    // would paint the rest of the line green and hide a real `//` in it, so
    // one stray character stays uncoloured instead.
    EXPECT_EQ(layoutOf(scan(line, false)), QString());
}

TEST(CppTokenScan, ACommentAfterALiteralStartsItsOwnSpan)
{
    //            012345678901234
    const QString line = QStringLiteral(R"(f("a"); // done)");
    ASSERT_EQ(line.size(), 15);

    EXPECT_EQ(layoutOf(scan(line, false)), QStringLiteral("lit@2+3,//@8+7"));
}

TEST(CppTokenScan, SpansAreWhatTheWordPassesHaveToAvoid)
{
    const QString line = QStringLiteral(R"(const char* u = "http://x";)");
    const QList<Span> spans = scan(line, false).spans;
    ASSERT_EQ(spans.size(), 1);

    EXPECT_FALSE(overlapsAny(spans, 0, 6));    // `const` — free to colour
    EXPECT_FALSE(overlapsAny(spans, 12, 1));   // `u`
    EXPECT_FALSE(overlapsAny(spans, 26, 1));   // the trailing `;`
    EXPECT_TRUE(overlapsAny(spans, 16, 1));    // the opening quote
    EXPECT_TRUE(overlapsAny(spans, 20, 2));    // `//` inside the URL
    EXPECT_TRUE(overlapsAny(spans, 14, 4));    // reaches in from outside
    EXPECT_TRUE(overlapsAny(spans, 0, 27));    // the whole line
}

TEST(CppTokenScan, AnEmptyLineInsideABlockCommentKeepsTheState)
{
    // QSyntaxHighlighter hands over every block, blank lines included, and a
    // blank line inside a comment must not end it.
    const Line blank = scan(QString(), true);
    EXPECT_EQ(layoutOf(blank), QString());
    EXPECT_TRUE(blank.endsInsideBlockComment);

    const Line empty = scan(QString(), false);
    EXPECT_EQ(layoutOf(empty), QString());
    EXPECT_FALSE(empty.endsInsideBlockComment);
}
