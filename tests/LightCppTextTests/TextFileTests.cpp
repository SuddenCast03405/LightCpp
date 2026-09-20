// LightCppText tests - readDetectedTextFile pipeline.

#include "LightCpp/Text/TextFile.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
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
