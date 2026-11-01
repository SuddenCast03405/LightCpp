// LightCppText tests - readDetectedTextFile pipeline.

#include "LightCpp/Text/Converter.hpp"
#include "LightCpp/Text/TextFile.hpp"

#include <gtest/gtest.h>

#include <optional>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QStringEncoder>

#ifdef _WIN32
#include <windows.h>

namespace {
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
using LightCpp::Text::TextFileError;
using LightCpp::Text::TextFileResult;
using LightCpp::Text::readDetectedTextFile;

class TextFileTest : public ::testing::Test {
protected:
    QTemporaryDir dir_;

    /// Write raw bytes into the temp dir, return the absolute path.
    QString writeFile(const QString& name, const QByteArray& bytes)
    {
        const QString path = dir_.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            ADD_FAILURE() << "cannot create " << path.toStdString();
            return path;
        }
        file.write(bytes);
        file.close();
        return path;
    }
};

TEST_F(TextFileTest, ReadsUtf8File)
{
    const QString path = writeFile("utf8.txt", QByteArray("你好，LightC++\r\nint x;\r\n"));
    const auto result = LightCpp::Text::readDetectedTextFile(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().encoding, TextEncoding::Utf8);
    EXPECT_FALSE(result.value().hadBom);
    EXPECT_EQ(result.value().utf8, QByteArray("你好，LightC++\r\nint x;\r\n"));
}

TEST_F(TextFileTest, ReadsUtf8FileWithBom)
{
    const QString path = writeFile("bom.txt", QByteArray("\xEF\xBB\xBF" "text"));
    const auto result = LightCpp::Text::readDetectedTextFile(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().encoding, TextEncoding::Utf8);
    EXPECT_TRUE(result.value().hadBom);
    EXPECT_EQ(result.value().utf8, QByteArray("text"));  // BOM stripped
}

TEST_F(TextFileTest, NearlyUtf8FileKeepsItsTextAndIsMarkedAsAGuess)
{
    // R2-37: the file is UTF-8 with its last character cut in half. Reading it
    // through GBK would garble every Chinese character in it; the answer is to
    // keep the text, mark the two damaged bytes, and remember that this was a
    // guess — the caller has to show that before the buffer is saved back.
    QByteArray bytes("// 中文注释：这是 UTF-8 源文件\n");
    bytes += QByteArray(200, 'x');
    bytes += QByteArray("\xE4\xB8", 2);

    const QString path = writeFile("broken-utf8.cpp", bytes);
    const auto result = LightCpp::Text::readDetectedTextFile(path);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().encoding, TextEncoding::Utf8);
    EXPECT_FALSE(result.value().detectionReliable);
    EXPECT_EQ(result.value().invalidUtf8Sequences, 1u);

    const QString text = QString::fromUtf8(result.value().utf8);
    EXPECT_TRUE(text.startsWith(QStringLiteral("// 中文注释：这是 UTF-8 源文件")))
        << "the clean part must not be decoded through another code page";
    EXPECT_TRUE(text.endsWith(QChar::ReplacementCharacter));
}

#ifdef _WIN32
TEST_F(TextFileTest, ARealGbkFileIsReportedAsAGuessNotAsVerified)
{
    // The other half of R2-37: a guessed read must be distinguishable from a
    // verified one, because saving persists the guess.
    const QString path = writeFile("gbk2.txt", winEncode("这是一个 GBK 文件。", 936));
    const auto result = LightCpp::Text::readDetectedTextFile(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().encoding, LightCpp::Text::localAnsiEncoding());
    EXPECT_FALSE(result.value().detectionReliable);
    EXPECT_GT(result.value().invalidUtf8Sequences, 0u);
}
#endif  // _WIN32

TEST_F(TextFileTest, AVerifiedReadSaysItIsVerified)
{
    // The guard for the flag itself: if everything came back unreliable the
    // marker would be noise and the user would learn to ignore it.
    const QString path = writeFile("clean.cpp", QByteArray("// 中文注释\nint x;\n"));
    const auto result = LightCpp::Text::readDetectedTextFile(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().detectionReliable);
    EXPECT_EQ(result.value().invalidUtf8Sequences, 0u);
}

TEST_F(TextFileTest, ReadsUtf16LeFile)
{
    const QString text = "Hello 你好";
    QByteArray bytes = QByteArray::fromHex("FFFE");
    bytes.append(QStringEncoder(QStringConverter::Utf16LE)(text));
    const QString path = writeFile("utf16le.txt", bytes);
    const auto result = LightCpp::Text::readDetectedTextFile(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().encoding, TextEncoding::Utf16Le);
    EXPECT_TRUE(result.value().hadBom);
    EXPECT_EQ(QString::fromUtf8(result.value().utf8), text);
}

#ifdef _WIN32

// On a zh-CN Windows system the local ANSI code page is GBK (CP936). A GBK
// file is therefore detected as the local ANSI encoding — which on this
// build machine is GBK. Both names are accepted.
//
// The premise is the *machine's* code page (auto-detection falls back to
// localAnsiEncoding()), so the assertion only means something where the ACP
// is 936. On a runner with ACP 1252 the same bytes decode as Windows-1252 and
// the test would be checking the runner's locale instead of the reader —
// which is exactly how it failed on the first CI run.
TEST_F(TextFileTest, ReadsGbkFileAsUtf8)
{
    const UINT ansiCodePage = ::GetACP();
    if (ansiCodePage != 936) {
        GTEST_SKIP() << "local ANSI code page is " << ansiCodePage
                     << ", not GBK (936)";
    }
    const QString text = "你好，世界！这是一个 GBK 文件。";
    const QString path = writeFile("gbk.txt", winEncode(text, 936));
    const auto result = LightCpp::Text::readDetectedTextFile(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().encoding, LightCpp::Text::localAnsiEncoding());
    EXPECT_EQ(QString::fromUtf8(result.value().utf8), text);
}

// Files in non-local encodings (Shift-JIS, Big5, EUC-KR, Latin1) are NOT
// auto-detected — they fall back to the local ANSI code page. The user
// must override the choice via the encoding drop-down (which calls
// readTextFileWithEncoding). This test verifies that override path
// successfully recovers the original text.
TEST_F(TextFileTest, ReadsShiftJisWithForcedEncoding)
{
    const QString text = "こんにちは、世界！";
    const QString path = writeFile("sjis.txt", winEncode(text, 932));
    const auto result =
        LightCpp::Text::readTextFileWithEncoding(path, TextEncoding::ShiftJis);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().encoding, TextEncoding::ShiftJis);
    EXPECT_EQ(QString::fromUtf8(result.value().utf8), text);
}

TEST_F(TextFileTest, ReadsBig5WithForcedEncoding)
{
    const QString text = "這是繁體中文測試。";
    const QString path = writeFile("big5.txt", winEncode(text, 950));
    const auto result =
        LightCpp::Text::readTextFileWithEncoding(path, TextEncoding::Big5);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().encoding, TextEncoding::Big5);
    EXPECT_EQ(QString::fromUtf8(result.value().utf8), text);
}

#endif  // _WIN32

TEST_F(TextFileTest, MissingFile)
{
    const auto result = LightCpp::Text::readDetectedTextFile(
        dir_.filePath("does-not-exist.txt"));
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), TextFileError::NotFound);
}

TEST_F(TextFileTest, DirectoryRejected)
{
    const auto result = LightCpp::Text::readDetectedTextFile(dir_.path());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), TextFileError::NotARegularFile);
}

TEST_F(TextFileTest, TooLargeRejected)
{
    const QString path = writeFile("large.bin", QByteArray(100, 'x'));
    LightCpp::Text::TextFileOptions options;
    options.maxBytes = 10;
    const auto result = LightCpp::Text::readDetectedTextFile(path, options);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), TextFileError::TooLarge);
}

TEST_F(TextFileTest, NonRegularDeviceFilesAreRejectedBeforeReading)
{
#if defined(Q_OS_UNIX)
    const QString path = QStringLiteral("/dev/zero");
    const auto detected = readDetectedTextFile(path);
    ASSERT_FALSE(detected.hasValue());
    EXPECT_EQ(detected.error(), TextFileError::NotARegularFile);

    const auto forced = LightCpp::Text::readTextFileWithEncoding(
        path, TextEncoding::Utf8);
    ASSERT_FALSE(forced.hasValue());
    EXPECT_EQ(forced.error(), TextFileError::NotARegularFile);

    EXPECT_FALSE(
        LightCpp::Text::readSearchableTextFile(path, 1024).has_value());
#else
    GTEST_SKIP() << "the /dev/zero device path is POSIX-specific";
#endif
}

TEST_F(TextFileTest, BinaryRejected)
{
    const QByteArray bytes("\x00\x01\x02\x03\xFF\xFE\x00\x00binary\x00", 16);
    const QString path = writeFile("blob.bin", bytes);
    const auto result = LightCpp::Text::readDetectedTextFile(path);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), TextFileError::BinaryFile);
}

}  // namespace

// --- detectLineEndings -------------------------------------------------

TEST(DetectLineEndings, EmptyContentIsNone)
{
    EXPECT_EQ(LightCpp::Text::detectLineEndings(QByteArray()),
              LightCpp::Text::LineEnding::None);
}

TEST(DetectLineEndings, NoNewlineAtAllIsNone)
{
    EXPECT_EQ(LightCpp::Text::detectLineEndings(QByteArray("one line")),
              LightCpp::Text::LineEnding::None);
}

TEST(DetectLineEndings, LfFile)
{
    EXPECT_EQ(LightCpp::Text::detectLineEndings(QByteArray("a\nb\nc")),
              LightCpp::Text::LineEnding::Lf);
}

TEST(DetectLineEndings, CrLfFile)
{
    EXPECT_EQ(LightCpp::Text::detectLineEndings(QByteArray("a\r\nb\r\nc")),
              LightCpp::Text::LineEnding::CrLf);
}

TEST(DetectLineEndings, MixedContentTakesTheMajority)
{
    // 4 CRLF vs 1 LF -> CRLF.
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray("a\r\nb\r\nc\r\nd\r\ne\n")),
              LightCpp::Text::LineEnding::CrLf);
    // 1 CRLF vs 4 LF -> LF.
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray("a\r\nb\nc\nd\ne\n")),
              LightCpp::Text::LineEnding::Lf);
}

TEST(DetectLineEndings, PureCrFileKeepsItsStyle)
{
    // Used to answer None ("no evidence"), and None is what makes the save
    // path fall back to the global default — so opening a classic-Mac file and
    // pressing Ctrl+S rewrote every line ending in it. A lone \r is a line
    // ending; count it.
    EXPECT_EQ(LightCpp::Text::detectLineEndings(QByteArray("a\rb\rc")),
              LightCpp::Text::LineEnding::Cr);
    // A trailing CR (no text after it) is still one line ending.
    EXPECT_EQ(LightCpp::Text::detectLineEndings(QByteArray("a\r")),
              LightCpp::Text::LineEnding::Cr);
}

TEST(DetectLineEndings, CarriageReturnCountsOnlyWhenNotFollowedByLineFeed)
{
    // "a\rb\r\nc\r" is 2 bare CR + 1 CRLF: the CR of a CRLF pair must not be
    // counted twice, or every Windows file would look like it had double the
    // line endings it has.
    EXPECT_EQ(LightCpp::Text::detectLineEndings(QByteArray("a\rb\r\nc\r")),
              LightCpp::Text::LineEnding::Cr);
}

TEST(DetectLineEndings, CrLosesToLfOnATie)
{
    // 2 bare LF vs 2 bare CR: the one-byte styles tie, and LF is the safer
    // answer (both are one byte, but LF is what every other tool expects).
    EXPECT_EQ(LightCpp::Text::detectLineEndings(QByteArray("a\nb\nc\rd\re")),
              LightCpp::Text::LineEnding::Lf);
}

TEST(DetectLineEndings, CrBeatsCrLfOnATie)
{
    // 2 CR vs 2 CRLF: prefer the one-byte style rather than adding a byte to
    // every line.
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray("a\rb\rc\r\nd\r\n")),
              LightCpp::Text::LineEnding::Cr);
}

TEST(DetectLineEndings, Utf16LeCrLfIsCrLf)
{
    // UTF-16LE "a\r\n" is 61 00 0D 00 0A 00. Counting raw bytes sees the
    // 0x0A with a 0x00 before it (not 0x0D) and wrongly reports LF — which
    // also rewrote the whole file to LF on the next save. The code-unit scan
    // pairs 0D 00 with the 0A 00 that follows it and reports CRLF.
    const unsigned char utf16leCrLf[] = {'a', 0x00, '\r', 0x00, '\n', 0x00};
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray(reinterpret_cast<const char*>(utf16leCrLf), 6),
                  LightCpp::Text::TextEncoding::Utf16Le),
              LightCpp::Text::LineEnding::CrLf);

    // Same bytes with no encoding hint keep the old byte-wise answer; that is
    // the bug the encoding argument exists to avoid.
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray(reinterpret_cast<const char*>(utf16leCrLf), 6)),
              LightCpp::Text::LineEnding::Lf);
}

TEST(DetectLineEndings, Utf16LeLfAndNoneAreCorrect)
{
    const unsigned char lf[] = {'a', 0x00, '\n', 0x00, 'b', 0x00, '\n', 0x00};
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray(reinterpret_cast<const char*>(lf), 8),
                  LightCpp::Text::TextEncoding::Utf16Le),
              LightCpp::Text::LineEnding::Lf);

    // A character whose code unit merely *contains* a 0x0A byte (U+8C0A is
    // 0A 8C in little-endian) is not a line feed.
    const unsigned char insideACodeUnit[] = {0x0A, 0x8C};
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray(reinterpret_cast<const char*>(insideACodeUnit), 2),
                  LightCpp::Text::TextEncoding::Utf16Le),
              LightCpp::Text::LineEnding::None);
}

TEST(DetectLineEndings, Utf16BeCrLfIsCrLf)
{
    const unsigned char beCrLf[] = {0x00, 'a', 0x00, '\r', 0x00, '\n'};
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray(reinterpret_cast<const char*>(beCrLf), 6),
                  LightCpp::Text::TextEncoding::Utf16Be),
              LightCpp::Text::LineEnding::CrLf);
}

TEST(DetectLineEndings, Utf32CrLfIsCrLf)
{
    // UTF-32LE "a\r\n": 61 00 00 00 | 0D 00 00 00 | 0A 00 00 00.
    const unsigned char leCrLf[] = {'a', 0x00, 0x00, 0x00,
                                    0x0D, 0x00, 0x00, 0x00,
                                    0x0A, 0x00, 0x00, 0x00};
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray(reinterpret_cast<const char*>(leCrLf), 12),
                  LightCpp::Text::TextEncoding::Utf32Le),
              LightCpp::Text::LineEnding::CrLf);
}

// --- readDetectedTextFile carries the detection ------------------------

TEST_F(TextFileTest, ReadReportsLineEndingStyle)
{
    const QString lfPath = writeFile("lines_lf.txt", QByteArray("a\nb\n"));
    const TextFileResult lf = readDetectedTextFile(lfPath);
    ASSERT_TRUE(lf.hasValue());
    EXPECT_EQ(lf.value().lineEndings, LightCpp::Text::LineEnding::Lf);

    const QString crlfPath =
        writeFile("lines_crlf.txt", QByteArray("a\r\nb\r\n"));
    const TextFileResult crlf = readDetectedTextFile(crlfPath);
    ASSERT_TRUE(crlf.hasValue());
    EXPECT_EQ(crlf.value().lineEndings, LightCpp::Text::LineEnding::CrLf);

    const TextFileResult none = readDetectedTextFile(
        writeFile("single.txt", QByteArray("no newline")));
    ASSERT_TRUE(none.hasValue());
    EXPECT_EQ(none.value().lineEndings, LightCpp::Text::LineEnding::None);

    const TextFileResult cr = readDetectedTextFile(
        writeFile("lines_cr.txt", QByteArray("a\rb\r")));
    ASSERT_TRUE(cr.hasValue());
    EXPECT_EQ(cr.value().lineEndings, LightCpp::Text::LineEnding::Cr);
    // The decoded text is normalised to LF by QTextDocument, so the ending
    // style only survives if the read reports it — which is the whole point.
    EXPECT_EQ(cr.value().utf8, QByteArray("a\rb\r"));
}

TEST(DetectLineEndings, NamesMatchTheStatusBarCombo)
{
    EXPECT_STREQ(LightCpp::Text::toString(LightCpp::Text::LineEnding::Lf),
                 "LF");
    EXPECT_STREQ(LightCpp::Text::toString(LightCpp::Text::LineEnding::CrLf),
                 "CRLF");
    EXPECT_STREQ(LightCpp::Text::toString(LightCpp::Text::LineEnding::Cr),
                 "CR");
    EXPECT_STREQ(LightCpp::Text::toString(LightCpp::Text::LineEnding::None),
                 "");
}

// ---------------------------------------------------------------------------
// writeFileAtomically
// ---------------------------------------------------------------------------

using LightCpp::Text::writeFileAtomically;
using LightCpp::Text::WriteFileError;

namespace {

QByteArray readBack(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray("<unreadable>");
    }
    return file.readAll();
}

/// Files left in the temp dir. The atomic write must not leave its
/// temporary behind on either path (commit or rollback).
QStringList filesIn(const QTemporaryDir& dir)
{
    return QDir(dir.path()).entryList(QDir::Files);
}

}  // namespace

TEST_F(TextFileTest, WriteFileAtomicallyCreatesAndReplacesContent)
{
    const QString path = dir_.filePath("out.txt");

    EXPECT_FALSE(writeFileAtomically(path, QByteArray("first")).has_value());
    EXPECT_EQ(readBack(path), QByteArray("first"));
    EXPECT_EQ(filesIn(dir_), QStringList{QStringLiteral("out.txt")});

    // A second save replaces the contents and leaves no temporary behind.
    EXPECT_FALSE(writeFileAtomically(path, QByteArray("second")).has_value());
    EXPECT_EQ(readBack(path), QByteArray("second"));
    EXPECT_EQ(filesIn(dir_), QStringList{QStringLiteral("out.txt")});
}

TEST_F(TextFileTest, WriteFileAtomicallyIsBinarySafeAndKeepsLineEndings)
{
    const QString path = dir_.filePath("bytes.txt");
    // NUL, CRLF and non-ASCII must survive verbatim: the editor's payload is
    // already the final byte sequence, this must not re-encode anything.
    QByteArray payload("line1\r\n\x00binary\xC3\xA9\r\n");
    EXPECT_FALSE(writeFileAtomically(path, payload).has_value());
    EXPECT_EQ(readBack(path), payload);
}

TEST_F(TextFileTest, WriteFileAtomicallyWritesAnEmptyFile)
{
    // A zero-byte payload is a legitimate save (the user deleted everything);
    // it must not be mistaken for a failed/short write.
    const QString path = dir_.filePath("empty.txt");
    EXPECT_FALSE(writeFileAtomically(path, QByteArray()).has_value());
    ASSERT_TRUE(QFile::exists(path));
    EXPECT_EQ(QFileInfo(path).size(), 0);
    EXPECT_EQ(filesIn(dir_), QStringList{QStringLiteral("empty.txt")});
}

TEST_F(TextFileTest, WriteFileAtomicallyOverwritesAnExistingFile)
{
    const QString path = writeFile("existing.txt", QByteArray("ORIGINAL"));
    EXPECT_FALSE(writeFileAtomically(path, QByteArray("NEW")).has_value());
    EXPECT_EQ(readBack(path), QByteArray("NEW"));
    EXPECT_EQ(filesIn(dir_), QStringList{QStringLiteral("existing.txt")});
}

TEST_F(TextFileTest, WriteFileAtomicallyFailsWhenTheDirectoryIsMissing)
{
    const QString path = dir_.filePath("nope/deeper/out.txt");
    const std::optional<WriteFileError> error =
        writeFileAtomically(path, QByteArray("x"));
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, WriteFileError::OpenFailed);
    EXPECT_FALSE(QFile::exists(path));
    // Nothing may be created anywhere on the way there.
    EXPECT_TRUE(filesIn(dir_).isEmpty());
}

#ifdef _WIN32
TEST_F(TextFileTest, WriteFileAtomicallyKeepsOldContentWhenTheTargetIsLocked)
{
    // The reason this function exists. A Truncate + write() would already
    // have emptied the file at this point, losing the old content and
    // leaving the new one nowhere; the atomic write must leave the file
    // exactly as it was.
    const QString path = writeFile("locked.txt", QByteArray("ORIGINAL"));

    // Hold the target with no sharing at all, like an antivirus scanner or
    // another editor would while it has the file open.
    const HANDLE handle = ::CreateFileW(
        reinterpret_cast<const wchar_t*>(path.utf16()),
        0, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);

    const std::optional<WriteFileError> error =
        writeFileAtomically(path, QByteArray("REPLACEMENT"));

    ::CloseHandle(handle);

    ASSERT_TRUE(error.has_value()) << "replacing a locked file must fail";
    // Which of the two it is depends on how far Qt gets before the sharing
    // violation stops it (it may fail reading the target's permissions too);
    // either way nothing was written.
    EXPECT_TRUE(*error == WriteFileError::CommitFailed
                || *error == WriteFileError::OpenFailed)
        << "unexpected reason: " << LightCpp::Text::toString(*error);
    EXPECT_EQ(readBack(path), QByteArray("ORIGINAL"));
    // The temporary it wrote must be gone, not orphaned next to the target.
    EXPECT_EQ(filesIn(dir_), QStringList{QStringLiteral("locked.txt")});
}
#endif  // _WIN32

TEST(WriteFileErrorNames, DescribeEveryFailure)
{
    EXPECT_STRNE(LightCpp::Text::toString(WriteFileError::OpenFailed), "");
    EXPECT_STRNE(LightCpp::Text::toString(WriteFileError::WriteFailed), "");
    EXPECT_STRNE(LightCpp::Text::toString(WriteFileError::CommitFailed), "");
}

// ---------------------------------------------------------------------------
// Forced-encoding round trips
//
// The status-bar encoding drop-down promises that picking an encoding
// relabels the tab and the next save writes those bytes. That only holds if
// encode -> decode really is a round trip for each codec the drop-down
// offers, which is what these pin down.
// ---------------------------------------------------------------------------

namespace {

using LightCpp::Text::ConvertResult;
using LightCpp::Text::convertToUtf8;
using LightCpp::Text::encodeFromQString;

void expectRoundTrip(const QString& text, TextEncoding encoding)
{
    const ConvertResult encoded = encodeFromQString(text, encoding);
    ASSERT_TRUE(encoded.hasValue())
        << "encode failed for " << LightCpp::Text::toName(encoding)
        << ": " << LightCpp::Text::toString(encoded.error());
    const ConvertResult decoded = convertToUtf8(encoded.value(), encoding);
    ASSERT_TRUE(decoded.hasValue())
        << "decode failed for " << LightCpp::Text::toName(encoding)
        << ": " << LightCpp::Text::toString(decoded.error());
    EXPECT_EQ(QString::fromUtf8(decoded.value()), text)
        << "round trip changed the text for "
        << LightCpp::Text::toName(encoding);
}

}  // namespace

TEST(EncodingRoundTrip, LegacyCjkCodecs)
{
    expectRoundTrip(QString::fromUtf8("你好，世界——GBK 往返。"),
                    TextEncoding::Gbk);
    expectRoundTrip(QString::fromUtf8("這是繁體中文往返測試。"),
                    TextEncoding::Big5);
    expectRoundTrip(QString::fromUtf8("こんにちは、世界。"),
                    TextEncoding::ShiftJis);
}

TEST(EncodingRoundTrip, Gb18030RoundTripsBmpText)
{
    // GB18030 saving used to fail outright: CP54936 rejects both
    // WC_NO_BEST_FIT_CHARS (ERROR_INVALID_FLAGS) and a non-null
    // lpUsedDefaultChar (ERROR_INVALID_PARAMETER), so every write came back
    // as "input is not valid in the encoding" — plain ASCII included.
    expectRoundTrip(QStringLiteral("plain ascii"), TextEncoding::Gb18030);
    expectRoundTrip(QString::fromUtf8("你好，世界"), TextEncoding::Gb18030);
    expectRoundTrip(QString::fromUtf8("€ 欧元符号"), TextEncoding::Gb18030);
}

TEST(EncodingRoundTrip, Gb18030RejectsBytesThatCannotRoundTripExactly)
{
    // Some platform GB18030 tables decode this four-byte sequence to two BMP
    // characters and then choose a different byte spelling when encoding
    // those characters. Such a sequence must be rejected at decode time:
    // opening it and later saving it would silently rewrite the source bytes.
    const QByteArray bytes = QByteArray::fromHex("f0a0aeb7");
    const ConvertResult decoded = convertToUtf8(bytes, TextEncoding::Gb18030);
    if (!decoded.hasValue()) {
        SUCCEED() << "the platform codec refuses a non-canonical byte sequence";
        return;
    }
    const ConvertResult reencoded = encodeFromQString(
        QString::fromUtf8(decoded.value()), TextEncoding::Gb18030);
    ASSERT_TRUE(reencoded.hasValue());
    EXPECT_EQ(reencoded.value(), bytes)
        << "a GB18030 load/save cycle changed the bytes";

    // GBK reads the same bytes as two unrelated Han characters — no
    // four-byte form exists there at all. Whatever it makes of them, it must
    // be lossless in the same sense.
    const ConvertResult asGbk = convertToUtf8(bytes, TextEncoding::Gbk);
    if (asGbk.hasValue()) {
        const ConvertResult back = encodeFromQString(
            QString::fromUtf8(asGbk.value()), TextEncoding::Gbk);
        ASSERT_TRUE(back.hasValue());
        EXPECT_EQ(back.value(), bytes)
            << "a GBK load/save cycle changed the bytes";
    }
}

TEST(EncodingRoundTrip, NeverSilentlySubstitutes)
{
    // A codec that cannot carry a character must refuse, not write '?'.
    // Windows' GB18030 encoder replaces U+20BB7 with 0x3F and reports
    // nothing, so this pins the decode-back guard: either the call fails, or
    // the bytes it produced really do carry the character.
    const QString text = QString::fromUtf8("𠮷");
    const TextEncoding codecs[] = {TextEncoding::Gbk, TextEncoding::Gb18030,
                                   TextEncoding::Big5, TextEncoding::ShiftJis};
    for (const TextEncoding encoding : codecs) {
        const ConvertResult encoded = encodeFromQString(text, encoding);
        if (!encoded.hasValue()) {
            continue;  // refused: nothing written, nothing lost
        }
        const ConvertResult back = convertToUtf8(encoded.value(), encoding);
        ASSERT_TRUE(back.hasValue()) << LightCpp::Text::toName(encoding);
        EXPECT_EQ(QString::fromUtf8(back.value()), text)
            << LightCpp::Text::toName(encoding)
            << " replaced a character it cannot represent";
    }
}

// ---------------------------------------------------------------------------
// readSearchableTextFile — the whole-file read behind "Search in Project"
// ---------------------------------------------------------------------------

TEST_F(TextFileTest, SearchReadKeepsTheStartOfAFileBiggerThanTheSniffBlock)
{
    // The scan sniffed the first 4 KiB for NUL bytes and then called
    // readAll(), which returns what is left *after* the bytes already read —
    // so every file larger than 4 KiB was searched without its opening block:
    // the #include lines and the file header were simply not there, and every
    // reported line number was off by the size of the missing prefix.
    QByteArray content("// needle_at_the_very_top\n#include <vector>\n");
    while (content.size() < 4096 + 128) {
        content += "// padding, to push this file past the sniff block\n";
    }
    const QString path = writeFile("big.cpp", content);
    ASSERT_GT(content.size(), 4096);

    const auto text =
        LightCpp::Text::readSearchableTextFile(path, 8 * 1024 * 1024);

    ASSERT_TRUE(text.has_value());
    EXPECT_TRUE(text->startsWith(QStringLiteral("// needle_at_the_very_top")));
    EXPECT_TRUE(text->contains(QStringLiteral("#include <vector>")));
    // Nothing was dropped: same number of lines as the file has.
    EXPECT_EQ(text->count(QLatin1Char('\n')), content.count('\n'));
    EXPECT_EQ(*text, QString::fromUtf8(content));
}

TEST_F(TextFileTest, SearchReadRejectsNulBytesInTheOpeningBlock)
{
    QByteArray content("plain text at the start\n");
    content.append(4000 - content.size(), '.');
    content.append('\0');
    content.append("more bytes after the NUL\n");
    const QString path = writeFile("binary.bin", content);

    EXPECT_FALSE(
        LightCpp::Text::readSearchableTextFile(path, 8 * 1024 * 1024).has_value());
}

TEST_F(TextFileTest, SearchReadOnlySniffsTheOpeningBlock)
{
    // The guard is a header check, not a whole-file scan: a NUL past the
    // sniff block is tolerated so that a stray byte late in a large file does
    // not make the whole file unsearchable. Pinned here so the boundary is a
    // decision rather than an accident.
    QByteArray content("text, then a long stretch without NUL\n");
    content.append(5000 - content.size(), '.');
    content.append('\0');
    const QString path = writeFile("late-nul.txt", content);

    const auto text =
        LightCpp::Text::readSearchableTextFile(path, 8 * 1024 * 1024);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->size(), content.size());
}

TEST_F(TextFileTest, SearchReadRejectsFilesAboveTheLimit)
{
    const QByteArray content("0123456789");
    const QString path = writeFile("small.txt", content);

    EXPECT_TRUE(LightCpp::Text::readSearchableTextFile(path, 1024).has_value());
    EXPECT_TRUE(LightCpp::Text::readSearchableTextFile(path, 10).has_value());
    EXPECT_FALSE(LightCpp::Text::readSearchableTextFile(path, 9).has_value());
    // 0 means "no limit" rather than "nothing fits".
    EXPECT_TRUE(LightCpp::Text::readSearchableTextFile(path, 0).has_value());
}


TEST_F(TextFileTest, SearchReadStopsAtTheConfiguredLimitForLargeFiles)
{
    const QString path = dir_.filePath(QStringLiteral("large-sparse.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_TRUE(file.resize(64LL * 1024 * 1024));
    file.close();

    EXPECT_FALSE(
        LightCpp::Text::readSearchableTextFile(path, 1024).has_value());
}

TEST_F(TextFileTest, SearchReadReturnsEachFileOfItsOwnAccord)
{
    // Two consecutive reads must not depend on each other: the old code left
    // the position at 4096, so a second read of a longer file was short by a
    // block as well.
    QByteArray content("first line marker\n");
    content.append(6000, 'x');
    const QString path = writeFile("repeat.cpp", content);

    const auto first =
        LightCpp::Text::readSearchableTextFile(path, 8 * 1024 * 1024);
    const auto second =
        LightCpp::Text::readSearchableTextFile(path, 8 * 1024 * 1024);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(*first, QString::fromUtf8(content));
}

TEST_F(TextFileTest, SearchReadRefusesWhatItCannotOpen)
{
    EXPECT_FALSE(LightCpp::Text::readSearchableTextFile(
                     dir_.filePath("no-such-file.cpp"), 8 * 1024 * 1024)
                     .has_value());
    // A directory is not a file.
    EXPECT_FALSE(
        LightCpp::Text::readSearchableTextFile(dir_.path(), 8 * 1024 * 1024)
            .has_value());
}

// isUnderSkippedDirectory — the policy behind "Search in Project"'s decision
// never to descend into generated or VCS-internal directories.

TEST(SkippedScanDirectories, MatchesTheDirectorySegmentAtAnyDepth)
{
    using LightCpp::Text::isUnderSkippedDirectory;

    // At the scan root.
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral("build/out.cpp")));
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral(".git/HEAD")));
    // Nested — the scan only used to look at the first segment, so src/build
    // and sub/.git were walked anyway and could spend the whole hit cap.
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral("src/build/out.cpp")));
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral("sub/.git/index")));
    EXPECT_TRUE(isUnderSkippedDirectory(
        QStringLiteral("a/b/node_modules/x/y.js")));
    // The rest of the configured names.
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral(".svn/entries")));
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral(".hg/store")));
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral(".vs/x.vcxproj")));
    EXPECT_TRUE(
        isUnderSkippedDirectory(QStringLiteral("cmake-build-debug/x.cpp")));
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral(".ccls-cache/@x/y.hpp")));
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral(".workbuddy/memory/x.md")));
}

TEST(SkippedScanDirectories, ComparesWholeSegmentsOnly)
{
    using LightCpp::Text::isUnderSkippedDirectory;

    // `mybuild` is not `build`, and a source whose *name* merely contains a
    // skipped word must stay in the scan.
    EXPECT_FALSE(isUnderSkippedDirectory(QStringLiteral("mybuild/out.cpp")));
    EXPECT_FALSE(isUnderSkippedDirectory(QStringLiteral("src/buildable/x.cpp")));
    EXPECT_FALSE(isUnderSkippedDirectory(QStringLiteral("src/rebuild.cpp")));
    EXPECT_FALSE(isUnderSkippedDirectory(QStringLiteral("build.cpp")));
    EXPECT_FALSE(isUnderSkippedDirectory(QStringLiteral("git/HEAD")));
    // A *file* named like a skipped directory is still a file: only the
    // segments above the last one are directories.
    EXPECT_FALSE(isUnderSkippedDirectory(QStringLiteral("build")));
    EXPECT_FALSE(isUnderSkippedDirectory(QStringLiteral("src/build")));
}

TEST(SkippedScanDirectories, AcceptsNativeSeparators)
{
    using LightCpp::Text::isUnderSkippedDirectory;

    // The dialog hands over '/' separators, but a caller passing Windows ones
    // straight through must get the same answer.
    EXPECT_TRUE(isUnderSkippedDirectory(QStringLiteral("src\\build\\out.cpp")));
    EXPECT_FALSE(isUnderSkippedDirectory(QStringLiteral("src\\real\\out.cpp")));
}
