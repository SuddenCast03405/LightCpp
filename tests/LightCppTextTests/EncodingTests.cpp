// LightCppText tests - encoding detection & conversion.

#include "LightCpp/Text/Converter.hpp"
#include "LightCpp/Text/Encoding.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QStringEncoder>

#ifdef _WIN32
#include <windows.h>

namespace {

/// Fixture encoder: QString -> bytes via WideCharToMultiByte.
QByteArray winEncode(const QString& text, UINT codePage)
{
    const int len = ::WideCharToMultiByte(codePage, 0,
                                          reinterpret_cast<const wchar_t*>(text.utf16()),
                                          text.size(), nullptr, 0, nullptr, nullptr);
    QByteArray bytes(len, Qt::Uninitialized);
    ::WideCharToMultiByte(codePage, 0,
                          reinterpret_cast<const wchar_t*>(text.utf16()),
                          text.size(), bytes.data(), len, nullptr, nullptr);
    return bytes;
}

}  // namespace
#endif  // _WIN32

namespace {

using LightCpp::Text::TextEncoding;

QString utf8(const QByteArray& bytes)
{
    return QString::fromUtf8(bytes);
}

}  // namespace

// --- BOM-based detection (always reliable) ------------------------------------

TEST(EncodingDetect, Utf8WithBom)
{
    const QByteArray bytes = "\xEF\xBB\xBF" "你好，世界";
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, TextEncoding::Utf8);
    EXPECT_TRUE(d.hadBom);
    EXPECT_TRUE(d.reliable);
    EXPECT_EQ(d.score, 1000);
}

TEST(EncodingDetect, Utf16LeWithBom)
{
    const QString text = "Hello 你好";
    const QByteArray payload = QStringEncoder(QStringConverter::Utf16LE)(text);
    QByteArray bytes = QByteArray::fromHex("FFFE");
    bytes.append(payload);
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, TextEncoding::Utf16Le);
    EXPECT_TRUE(d.hadBom);
}

TEST(EncodingDetect, Utf16BeWithBom)
{
    const QString text = "Hello 你好";
    const QByteArray payload = QStringEncoder(QStringConverter::Utf16BE)(text);
    QByteArray bytes = QByteArray::fromHex("FEFF");
    bytes.append(payload);
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, TextEncoding::Utf16Be);
    EXPECT_TRUE(d.hadBom);
}

TEST(EncodingDetect, Utf32LeWithBom)
{
    const QString text = "Hi 你";
    const QByteArray payload = QStringEncoder(QStringConverter::Utf32LE)(text);
    QByteArray bytes = QByteArray::fromHex("FFFE0000");
    bytes.append(payload);
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, TextEncoding::Utf32Le);
    EXPECT_TRUE(d.hadBom);
}

// --- UTF-8 / ASCII without BOM --------------------------------------------------

TEST(EncodingDetect, Utf8NoBom)
{
    const auto d = LightCpp::Text::detectEncoding(QByteArray("你好，世界"));
    EXPECT_EQ(d.encoding, TextEncoding::Utf8);
    EXPECT_FALSE(d.hadBom);
    EXPECT_TRUE(d.reliable);
}

TEST(EncodingDetect, AsciiIsUtf8)
{
    const auto d = LightCpp::Text::detectEncoding(QByteArray("int main() { return 0; }"));
    EXPECT_EQ(d.encoding, TextEncoding::Utf8);
    EXPECT_FALSE(d.hadBom);
}

TEST(EncodingDetect, EmptyInput)
{
    const auto d = LightCpp::Text::detectEncoding(QByteArray());
    EXPECT_EQ(d.encoding, TextEncoding::Utf8);
}

// --- UTF-16 without BOM (alignment heuristic) -----------------------------------

TEST(EncodingDetect, Utf16LeNoBom)
{
    const QString text = "ascii only text";
    const QByteArray bytes = QStringEncoder(QStringConverter::Utf16LE)(text);
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, TextEncoding::Utf16Le);
}

// --- binary --------------------------------------------------------------------

TEST(EncodingDetect, BinaryWithNuls)
{
    const auto d = LightCpp::Text::detectEncoding(
        QByteArray("\x89PNG\r\n\x1A\n\x00\x00\x00\x0D", 16));
    EXPECT_EQ(d.encoding, TextEncoding::Binary);
}

// --- local ANSI code page ------------------------------------------------------

TEST(LocalAnsi, ReturnsValidEncoding)
{
    // The local ANSI encoding must be one of the supported legacy codecs
    // (GBK on zh-CN, Shift-JIS on ja-JP, EUC-KR on ko-KR, Windows-1252
    // on en-US). Sanity check that it is never Unknown or Binary.
    const TextEncoding e = LightCpp::Text::localAnsiEncoding();
    EXPECT_TRUE(e == TextEncoding::Gbk
                || e == TextEncoding::Big5
                || e == TextEncoding::ShiftJis
                || e == TextEncoding::EucKr
                || e == TextEncoding::Gb18030
                || e == TextEncoding::Windows1252);
}

TEST(LocalAnsi, DisplayNameHasAnsiPrefix)
{
    // e.g. "ANSI (GBK)" on zh-CN, "ANSI (Shift-JIS)" on ja-JP.
    const QString name = LightCpp::Text::localAnsiDisplayName();
    EXPECT_TRUE(name.startsWith(QStringLiteral("ANSI (")));
    EXPECT_TRUE(name.endsWith(u')'));
}

#ifdef _WIN32

// On this build machine (zh-CN), GetACP() returns 936 -> GBK. The test
// asserts the documented mapping from CP ID to TextEncoding.
TEST(LocalAnsi, MatchesWindowsAcp)
{
    const UINT cp = ::GetACP();
    const TextEncoding expected = [cp]() -> TextEncoding {
        switch (cp) {
        case 936:   return TextEncoding::Gbk;
        case 950:   return TextEncoding::Big5;
        case 949:   return TextEncoding::EucKr;
        case 932:   return TextEncoding::ShiftJis;
        case 54936: return TextEncoding::Gb18030;
        default:    return TextEncoding::Windows1252;
        }
    }();
    EXPECT_EQ(LightCpp::Text::localAnsiEncoding(), expected);
}

// --- legacy codec detection (local ANSI fallback) -----------------------------
//
// Per the new detection algorithm, a non-Unicode byte stream is reported as
// the local ANSI code page. On this zh-CN build machine that is GBK (CP936),
// so any of the GBK / GB18030 / Big5 / Shift-JIS / EUC-KR / Latin1 byte
// streams below are detected as GBK. The user can override the choice via
// the encoding drop-down (which calls readTextFileWithEncoding).

TEST(EncodingDetect, GbkChinese)
{
    const QByteArray bytes = winEncode("你好，世界！今天天气不错。", 936);
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, LightCpp::Text::localAnsiEncoding())
        << "detected: " << LightCpp::Text::toName(d.encoding);
}

TEST(EncodingDetect, GbkMixedWithAsciiFallsBackToLocalAnsi)
{
    const QByteArray bytes =
        winEncode("// 注释：这是一个测试函数\n"
                  "int main() { return 0; }\n"
                  "// 输出结果到控制台\n", 936);
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, LightCpp::Text::localAnsiEncoding())
        << "detected: " << LightCpp::Text::toName(d.encoding);
}

TEST(EncodingDetect, NonLocalBig5FallsBackToLocalAnsi)
{
    // Big5 byte stream on a zh-CN machine: detected as the local ANSI
    // (GBK), not Big5. The user must manually pick Big5 from the
    // drop-down to read it correctly.
    const QByteArray bytes = winEncode("你好，世界！這是繁體中文測試。", 950);
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, LightCpp::Text::localAnsiEncoding())
        << "detected: " << LightCpp::Text::toName(d.encoding);
}

TEST(EncodingDetect, NonLocalShiftJisFallsBackToLocalAnsi)
{
    const QByteArray bytes = winEncode("こんにちは世界。これはテストです。", 932);
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, LightCpp::Text::localAnsiEncoding())
        << "detected: " << LightCpp::Text::toName(d.encoding);
}

TEST(EncodingDetect, NonLocalEucKrFallsBackToLocalAnsi)
{
    const QByteArray bytes = winEncode("안녕하세요 세계. 이것은 테스트입니다.", 51949);
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, LightCpp::Text::localAnsiEncoding())
        << "detected: " << LightCpp::Text::toName(d.encoding);
}

TEST(EncodingDetect, NonLocalLatin1FallsBackToLocalAnsi)
{
    const QByteArray bytes("caf\xE9 na\xEFve r\xE9sum\xE9");
    const auto d = LightCpp::Text::detectEncoding(bytes);
    EXPECT_EQ(d.encoding, LightCpp::Text::localAnsiEncoding())
        << "detected: " << LightCpp::Text::toName(d.encoding);
}

// --- explicit conversion (always picks the right codec) -----------------------

TEST(EncodingConvert, GbkToUtf8RoundTrip)
{
    const QString text = "你好，世界！";
    const QByteArray gbk = winEncode(text, 936);
    const auto converted = LightCpp::Text::convertToUtf8(gbk, TextEncoding::Gbk);
    ASSERT_TRUE(converted.hasValue());
    EXPECT_EQ(utf8(converted.value()), text);
}

TEST(EncodingConvert, Big5ToUtf8RoundTrip)
{
    const QString text = "這是繁體中文。";
    const QByteArray big5 = winEncode(text, 950);
    const auto converted = LightCpp::Text::convertToUtf8(big5, TextEncoding::Big5);
    ASSERT_TRUE(converted.hasValue());
    EXPECT_EQ(utf8(converted.value()), text);
}

TEST(EncodingConvert, ShiftJisToUtf8RoundTrip)
{
    const QString text = "こんにちは世界。";
    const QByteArray sjis = winEncode(text, 932);
    const auto converted =
        LightCpp::Text::convertToUtf8(sjis, TextEncoding::ShiftJis);
    ASSERT_TRUE(converted.hasValue());
    EXPECT_EQ(utf8(converted.value()), text);
}

TEST(EncodingConvert, InvalidGbkRejected)
{
    // 0x81 is a GBK lead byte; 0xFF is never a valid GBK trail byte.
    const auto converted =
        LightCpp::Text::convertToUtf8(QByteArray("\x81\xFF", 2), TextEncoding::Gbk);
    EXPECT_FALSE(converted.hasValue());
}

#endif  // _WIN32

// --- conversion: Unicode families (cross-platform, Qt built-ins) ---------------

TEST(EncodingConvert, Utf16LeToUtf8)
{
    const QString text = "你好，world";
    const QByteArray bytes = QStringEncoder(QStringConverter::Utf16LE)(text);
    const auto converted =
        LightCpp::Text::convertToUtf8(bytes, TextEncoding::Utf16Le);
    ASSERT_TRUE(converted.hasValue());
    EXPECT_EQ(utf8(converted.value()), text);
}

TEST(EncodingConvert, Utf16LeBomStripped)
{
    const QString text = "text";
    QByteArray bytes = QByteArray::fromHex("FFFE");
    bytes.append(QStringEncoder(QStringConverter::Utf16LE)(text));
    const auto converted =
        LightCpp::Text::convertToUtf8(bytes, TextEncoding::Utf16Le);
    ASSERT_TRUE(converted.hasValue());
    EXPECT_EQ(utf8(converted.value()), text);  // BOM must not leak into output
}

TEST(EncodingConvert, Utf8Passthrough)
{
    const auto converted = LightCpp::Text::convertToUtf8(
        QByteArray("hello"), TextEncoding::Utf8);
    ASSERT_TRUE(converted.hasValue());
    EXPECT_EQ(converted.value(), QByteArray("hello"));
}

TEST(EncodingConvert, Latin1ToUtf8)
{
    const auto converted = LightCpp::Text::convertToUtf8(
        QByteArray("caf\xE9"), TextEncoding::Latin1);
    ASSERT_TRUE(converted.hasValue());
    EXPECT_EQ(utf8(converted.value()), QString::fromUtf8("caf\xC3\xA9"));
}

TEST(EncodingConvert, UnpairedSurrogateRejected)
{
    // A lone low surrogate in UTF-16LE must fail strict decoding.
    const auto converted = LightCpp::Text::convertToUtf8(
        QByteArray::fromHex("00DC"), TextEncoding::Utf16Le);
    EXPECT_FALSE(converted.hasValue());
}

TEST(EncodingConvert, InvalidUtf8Rejected)
{
    // 0xFF is never valid UTF-8. A manual "reopen as UTF-8" override on a
    // misdetected legacy file used to produce U+FFFD soup here, which a
    // later save then wrote back over the original bytes — the strict
    // decoder must fail instead.
    const QByteArray invalid("a\xFF""b");
    EXPECT_FALSE(LightCpp::Text::decodeToQString(
        invalid, TextEncoding::Utf8).has_value());
    const auto converted = LightCpp::Text::convertToUtf8(
        invalid, TextEncoding::Utf8);
    ASSERT_FALSE(converted.hasValue());
    EXPECT_EQ(converted.error(), LightCpp::Text::ConvertError::InvalidInput);
}

// --- encoding: QString -> target bytes (the save path) --------------------------

TEST(EncodingEncode, Utf8NoBom)
{
    const QString text = "你好 hello";
    const auto encoded =
        LightCpp::Text::encodeFromQString(text, TextEncoding::Utf8);
    ASSERT_TRUE(encoded.hasValue());
    EXPECT_EQ(encoded.value(), text.toUtf8());
}

TEST(EncodingEncode, Utf16LeWithBom)
{
    const QString text = "你好 world";
    const auto encoded =
        LightCpp::Text::encodeFromQString(text, TextEncoding::Utf16Le, true);
    ASSERT_TRUE(encoded.hasValue());
    // Re-decoding must restore the text and report the BOM.
    bool hadBom = false;
    const auto decoded = LightCpp::Text::decodeToQString(
        encoded.value(), TextEncoding::Utf16Le, true, &hadBom);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(hadBom);
    EXPECT_EQ(*decoded, text);
}

TEST(EncodingEncode, Utf16BeNoBomByDefault)
{
    const auto encoded =
        LightCpp::Text::encodeFromQString("abc", TextEncoding::Utf16Be);
    ASSERT_TRUE(encoded.hasValue());
    EXPECT_EQ(encoded.value().size(), 6);  // 3 chars * 2 bytes, no BOM
}

TEST(EncodingEncode, Latin1)
{
    const auto encoded =
        LightCpp::Text::encodeFromQString(QString::fromUtf8("caf\xC3\xA9"), TextEncoding::Latin1);
    ASSERT_TRUE(encoded.hasValue());
    EXPECT_EQ(encoded.value(), QByteArray("caf\xE9"));
}

TEST(EncodingEncode, EmptyInput)
{
    for (const TextEncoding encoding : {
             TextEncoding::Utf8, TextEncoding::Utf16Le, TextEncoding::Utf16Be,
             TextEncoding::Gb18030, TextEncoding::Big5, TextEncoding::Latin1}) {
        const auto encoded =
            LightCpp::Text::encodeFromQString(QString(), encoding, true);
        ASSERT_TRUE(encoded.hasValue());
        // Encoded empty text is either empty or just the requested BOM.
        EXPECT_LE(encoded.value().size(), 4);
    }
}

TEST(EncodingEncode, UnsupportedEncodingRejected)
{
    const auto encoded =
        LightCpp::Text::encodeFromQString("x", TextEncoding::Binary);
    EXPECT_FALSE(encoded.hasValue());
}

#ifdef _WIN32

TEST(EncodingEncode, GbkRoundTripThroughDecode)
{
    const QString text = "中文注释 // comment";
    const auto encoded =
        LightCpp::Text::encodeFromQString(text, TextEncoding::Gbk);
    ASSERT_TRUE(encoded.hasValue());
    const auto decoded =
        LightCpp::Text::decodeToQString(encoded.value(), TextEncoding::Gbk);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, text);
}

TEST(EncodingEncode, UnmappableCharacterRejected)
{
    // U+3053 is hiragana — not representable in Windows-1252. The encoder
    // must fail instead of silently writing a best-fit '?' byte.
    const auto encoded = LightCpp::Text::encodeFromQString(
        QString::fromUtf8("\xE3\x81\x93"), TextEncoding::Windows1252);
    EXPECT_FALSE(encoded.hasValue());
}

TEST(EncodingEncode, GbkUnmappableEmojiRejected)
{
    // Emoji are outside GBK: saving a GBK tab containing one must fail.
    const auto encoded = LightCpp::Text::encodeFromQString(
        QString::fromUtf8("\xF0\x9F\x98\x80"), TextEncoding::Gbk);
    EXPECT_FALSE(encoded.hasValue());
}

TEST(EncodingEncode, BestFitCharacterRejected)
{
    // U+0410 (Cyrillic capital A) has a best-fit mapping to ASCII 'A' in
    // Windows-1252. Best-fit substitution does not set usedDefault, so a
    // plain dwFlags=0 encoder used to report SUCCESS while writing a
    // different character than the user typed — silent corruption. The
    // WC_NO_BEST_FIT_CHARS encoder must fail instead.
    const QChar cyrillicA(0x0410);
    const auto encoded = LightCpp::Text::encodeFromQString(
        QString(cyrillicA), TextEncoding::Windows1252);
    EXPECT_FALSE(encoded.hasValue());
}

#endif  // _WIN32


// --- name mapping ----------------------------------------------------------------

TEST(EncodingNames, RoundTrip)
{
    for (const TextEncoding encoding : {
             TextEncoding::Utf8, TextEncoding::Utf16Le, TextEncoding::Utf16Be,
             TextEncoding::Utf32Le, TextEncoding::Utf32Be, TextEncoding::Gb18030,
             TextEncoding::Gbk, TextEncoding::Big5, TextEncoding::ShiftJis,
             TextEncoding::EucKr, TextEncoding::Windows1252, TextEncoding::Latin1}) {
        const auto parsed = LightCpp::Text::fromName(LightCpp::Text::toName(encoding));
        ASSERT_TRUE(parsed.has_value()) << LightCpp::Text::toName(encoding);
        EXPECT_EQ(*parsed, encoding);
    }
}

TEST(EncodingNames, CaseAndSeparatorInsensitive)
{
    EXPECT_EQ(LightCpp::Text::fromName("utf8"), TextEncoding::Utf8);
    EXPECT_EQ(LightCpp::Text::fromName("shift_jis"), TextEncoding::ShiftJis);
    EXPECT_EQ(LightCpp::Text::fromName("gb2312"), TextEncoding::Gbk);
    EXPECT_EQ(LightCpp::Text::fromName("no-such-codec"), std::nullopt);
}
