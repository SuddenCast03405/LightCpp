// LightCppCore tests - UTF-8 decoding / validation unit tests.

#include "LightCpp/Core/Utf8.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

using LightCpp::Core::Text::decodeCodePoint;
using LightCpp::Core::Text::validateUtf8;

// --- decodeCodePoint: well-formed inputs -----------------------------------

TEST(Utf8Decode, AsciiSingleByte)
{
    const auto r = decodeCodePoint("A");
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.value, 0x41u);
    EXPECT_EQ(r.consumed, 1u);
}

TEST(Utf8Decode, TwoByteLatin1)
{
    const auto r = decodeCodePoint("\xC3\xA9");  // U+00E9 'é'
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.value, 0xE9u);
    EXPECT_EQ(r.consumed, 2u);
}

TEST(Utf8Decode, ThreeByteCjk)
{
    const auto r = decodeCodePoint("\xE4\xBD\xA0");  // U+4F60 '你'
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.value, 0x4F60u);
    EXPECT_EQ(r.consumed, 3u);
}

TEST(Utf8Decode, FourByteEmoji)
{
    const auto r = decodeCodePoint("\xF0\x9F\x98\x80");  // U+1F600 '😀'
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.value, 0x1F600u);
    EXPECT_EQ(r.consumed, 4u);
}

// --- decodeCodePoint: malformed inputs --------------------------------------

TEST(Utf8Decode, StrayContinuationByte)
{
    const auto r = decodeCodePoint("\x80");
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.value, LightCpp::Core::Text::kReplacementChar);
    EXPECT_EQ(r.consumed, 1u);
}

TEST(Utf8Decode, TruncatedSequence)
{
    const auto r = decodeCodePoint("\xE4\xBD");  // 3-byte lead, only 1 continuation
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.consumed, 2u);  // maximal subpart
}

TEST(Utf8Decode, OverlongEncoding)
{
    const auto r = decodeCodePoint("\xC0\x80");  // overlong NUL
    EXPECT_FALSE(r.valid);
}

TEST(Utf8Decode, SurrogateEncoded)
{
    const auto r = decodeCodePoint("\xED\xA0\x80");  // U+D800 surrogate
    EXPECT_FALSE(r.valid);
}

TEST(Utf8Decode, OutOfRange)
{
    const auto r = decodeCodePoint("\xF5\x80\x80\x80");  // > U+10FFFF
    EXPECT_FALSE(r.valid);
}

TEST(Utf8Decode, InvalidContinuationInsideSequence)
{
    const auto r = decodeCodePoint("\xE4\xBD\x41");  // 'A' breaks the sequence
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.consumed, 2u);  // maximal subpart, 'A' is re-synced by caller
}

// --- validateUtf8 ------------------------------------------------------------

TEST(Utf8Validate, ValidText)
{
    const std::string_view text = "Hello, 你好, \xF0\x9F\x98\x80!";
    std::size_t firstInvalid = 123;
    const auto errors = validateUtf8(text, &firstInvalid);
    EXPECT_EQ(errors, 0u);
    EXPECT_EQ(firstInvalid, std::string_view::npos);
}

TEST(Utf8Validate, InvalidBytes)
{
    // Split literals: a hex escape greedily consumes following hex digits.
    const std::string_view text = "ok\xFF" "\xFE" "end";
    std::size_t firstInvalid = 0;
    const auto errors = validateUtf8(text, &firstInvalid);
    EXPECT_EQ(errors, 2u);
    EXPECT_EQ(firstInvalid, 2u);
}

TEST(Utf8Validate, EmptyInput)
{
    EXPECT_EQ(validateUtf8(""), 0u);
}

// --- constexpr sanity --------------------------------------------------------

TEST(Utf8Decode, ConstexprUsable)
{
    constexpr auto r = decodeCodePoint("\xE4\xBD\xA0");
    static_assert(r.valid && r.value == 0x4F60u && r.consumed == 3u,
                  "decodeCodePoint must be usable at compile time");
    EXPECT_TRUE(r.valid);
}

}  // namespace
