// Bracket matching and whole-word occurrence search: the pure-logic halves
// of the editor's bracket-match highlight and same-symbol highlight.
#include <gtest/gtest.h>
#include "LightCpp/Core/BracketMatch.hpp"
#include "LightCpp/Core/WordOccurrences.hpp"

using LightCpp::Core::Brackets::isBracket;
using LightCpp::Core::Brackets::matchBracket;
using LightCpp::Core::Words::wordOccurrences;

// --- bracket matching -------------------------------------------------------

TEST(BracketMatch, MatchesSimplePairsBothDirections)
{
    const std::string text = "int f() { return (a[0]); }";
    // '(' at 5 matches ')' at 6, and back.
    EXPECT_EQ(matchBracket(text, 5).value_or(std::size_t(-1)), std::size_t(6));
    EXPECT_EQ(matchBracket(text, 6).value_or(std::size_t(-1)), std::size_t(5));
    // '{' at 8 matches '}' at 25, and back.
    EXPECT_EQ(matchBracket(text, 8).value_or(std::size_t(-1)), std::size_t(25));
    EXPECT_EQ(matchBracket(text, 25).value_or(std::size_t(-1)), std::size_t(8));
    // '[' at 19 matches ']' at 21.
    EXPECT_EQ(matchBracket(text, 19).value_or(std::size_t(-1)), std::size_t(21));
}

TEST(BracketMatch, CountsNestingOfTheSameType)
{
    const std::string text = "f(g(), (h));";
    // outer '(' at 1 matches ')' at 10; the inner ')' at 9 matches '(' at 7.
    EXPECT_EQ(matchBracket(text, 1).value_or(std::size_t(-1)), std::size_t(10));
    EXPECT_EQ(matchBracket(text, 9).value_or(std::size_t(-1)), std::size_t(7));
    EXPECT_EQ(matchBracket(text, 7).value_or(std::size_t(-1)), std::size_t(9));
}

TEST(BracketMatch, IgnoresBracketsInStringsAndComments)
{
    // The ')' inside the string, the ')' in the line comment and the '(' in
    // the block comment are all invisible to the matcher.
    const std::string text =
        "f(\")\"); // )\n"
        "/* ( */ g();";
    // f( at 1 closes at 5: the ')' inside the string (3) is text.
    EXPECT_EQ(matchBracket(text, 1).value_or(std::size_t(-1)), std::size_t(5));
    EXPECT_EQ(matchBracket(text, 5).value_or(std::size_t(-1)), std::size_t(1));
}

TEST(BracketMatch, EscapedQuoteDoesNotCloseTheString)
{
    // "a\")" — the escaped quote keeps the string open, so its ')' is text.
    const std::string text = "f(\"a\\\")\");";
    // '(' at 1 closes at 8; the ')' at 6 sits inside the string literal.
    EXPECT_EQ(matchBracket(text, 1).value_or(std::size_t(-1)), std::size_t(8));
    EXPECT_EQ(matchBracket(text, 8).value_or(std::size_t(-1)), std::size_t(1));
}

TEST(BracketMatch, UnmatchedReturnsNullopt)
{
    EXPECT_FALSE(matchBracket("((", 0).has_value());
    EXPECT_FALSE(matchBracket("))", 1).has_value());
    EXPECT_FALSE(matchBracket("(a\nb", 0).has_value());  // run off the text
}

TEST(BracketMatch, NonBracketInputIsNullopt)
{
    EXPECT_FALSE(matchBracket("abc", 1).has_value());
    EXPECT_FALSE(isBracket('x'));
    EXPECT_TRUE(isBracket('('));
}

TEST(BracketMatch, MultiByteTextDoesNotConfuseTheScanner)
{
    // UTF-8 continuation bytes are >= 0x80, never ASCII brackets, so a
    // Chinese identifier around the brackets cannot break the match.
    const std::string text = "中文(参数)";
    const std::size_t open = text.find('(');
    const std::size_t close = text.find(')');
    EXPECT_EQ(matchBracket(text, open).value_or(std::size_t(-1)), close);
    EXPECT_EQ(matchBracket(text, close).value_or(std::size_t(-1)), open);
}

// --- whole-word occurrences -------------------------------------------------

TEST(WordOccurrences, FindsWholeWordsOnly)
{
    const std::string text = "main mainly main(); main_x";
    const auto hits = wordOccurrences(text, "main", 100);
    // "main" at 0 and the call at 12; "mainly" and "main_x" are other words.
    ASSERT_EQ(hits.size(), std::size_t(2));
    EXPECT_EQ(hits[0].first, std::size_t(0));
    EXPECT_EQ(hits[1].first, std::size_t(12));
}

TEST(WordOccurrences, TextEdgesCountAsBoundaries)
{
    const auto first = wordOccurrences("main", "main", 10);
    ASSERT_EQ(first.size(), std::size_t(1));
    EXPECT_EQ(first[0], std::make_pair(std::size_t(0), std::size_t(4)));
}

TEST(WordOccurrences, RespectsTheCap)
{
    const std::string text = "a a a a a";
    EXPECT_EQ(wordOccurrences(text, "a", 3).size(), std::size_t(3));
    EXPECT_EQ(wordOccurrences(text, "a", 0).size(), std::size_t(0));
}

TEST(WordOccurrences, EmptyWordAndNoHitAreEmpty)
{
    EXPECT_TRUE(wordOccurrences("abc", "", 10).empty());
    EXPECT_TRUE(wordOccurrences("abc", "xyz", 10).empty());
}

TEST(WordOccurrences, DigitsCountAsIdentifierCharacters)
{
    // `x1` must not match the x in `x11`.
    const std::string text = "x1 x11 x1";
    const auto hits = wordOccurrences(text, "x1", 10);
    ASSERT_EQ(hits.size(), std::size_t(2));
    EXPECT_EQ(hits[0].first, std::size_t(0));
    EXPECT_EQ(hits[1].first, std::size_t(7));
}
