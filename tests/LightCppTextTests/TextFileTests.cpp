// LightCppText tests - readDetectedTextFile pipeline.

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
TEST_F(TextFileTest, ReadsGbkFileAsUtf8)
{
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

TEST(DetectLineEndings, LoneCarriageReturnsAreIgnored)
{
    EXPECT_EQ(LightCpp::Text::detectLineEndings(QByteArray("a\rb\rc")),
              LightCpp::Text::LineEnding::None);
}

TEST(DetectLineEndings, Utf16NewlinesAreCountedByteWise)
{
    // UTF-16LE "a\n" is 61 00 0A 00: the 0x0A byte still only ever appears
    // as a line feed, so the byte-wise scan works regardless of encoding.
    const unsigned char utf16le[] = {'a', 0x00, '\n', 0x00,
                                     'b', 0x00, '\n', 0x00};
    EXPECT_EQ(LightCpp::Text::detectLineEndings(
                  QByteArray(reinterpret_cast<const char*>(utf16le), 8)),
              LightCpp::Text::LineEnding::Lf);
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
}

TEST(DetectLineEndings, NamesMatchTheStatusBarCombo)
{
    EXPECT_STREQ(LightCpp::Text::toString(LightCpp::Text::LineEnding::Lf),
                 "LF");
    EXPECT_STREQ(LightCpp::Text::toString(LightCpp::Text::LineEnding::CrLf),
                 "CRLF");
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
