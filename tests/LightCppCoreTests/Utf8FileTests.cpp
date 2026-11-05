// LightCppCore tests - UTF-8 file reading unit tests.

#include "LightCpp/Core/Utf8File.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>   // getpid
#endif

namespace {

namespace fs = std::filesystem;
using LightCpp::Core::Text::ReadError;
using LightCpp::Core::Text::ReadOptions;
using LightCpp::Core::Text::readUtf8File;

/// Per-test scratch directory, cleaned up automatically.
///
/// The name carries the process id, and it has to: ctest runs one *process*
/// per test (`--parallel N`), and a counter alone restarts at 1 in every one
/// of them. Two concurrent tests would then share one directory and delete
/// each other's files mid-test — and a `_wstat64` on a path whose directory
/// is being removed underneath it was measured to spin inside UCRT
/// (`common_stat_handle_file_not_opened` → `call_wfullpath`) instead of
/// returning an error, which hung whole full-suite runs at 599/600.
class Utf8FileTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path()
             / ("lightcpp_test_" + std::to_string(processId()) + "_"
                + std::to_string(++counter_));
        fs::create_directories(dir_);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    /// Write raw bytes to `name` inside the scratch dir, return full path.
    fs::path writeFile(std::string_view name, std::string_view bytes)
    {
        const fs::path path = dir_ / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            ADD_FAILURE() << "failed to write test file " << path.string();
        }
        return path;
    }

    fs::path dir_;

private:
    /// This process's id, so parallel ctest runs cannot land in one directory.
    static unsigned long processId()
    {
#ifdef _WIN32
        return static_cast<unsigned long>(::_getpid());
#else
        return static_cast<unsigned long>(::getpid());
#endif
    }

    static int counter_;
};

int Utf8FileTest::counter_ = 0;

// --- happy paths -------------------------------------------------------------

TEST_F(Utf8FileTest, ReadsPlainAscii)
{
    const auto path = writeFile("ascii.txt", "int main() { return 0; }\n");
    const auto result = readUtf8File(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().content, "int main() { return 0; }\n");
    EXPECT_EQ(result.value().byteSize, 25u);
    EXPECT_FALSE(result.value().hadBom);
    EXPECT_EQ(result.value().replacedInvalidSequences, 0u);
}

TEST_F(Utf8FileTest, ReadsUnicodePath)
{
#ifdef _WIN32
    const fs::path path = dir_ / L"中文文件.txt";
#else
    const fs::path path = dir_ / "中文文件.txt";
#endif
    const std::string payload = "路径中的中文文件名\n";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ASSERT_TRUE(out.good());
    }

    const auto result = readUtf8File(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().content, payload);
}

TEST_F(Utf8FileTest, ReadsMultiByteUtf8PreservingNewlines)
{
    const std::string payload = "// 注释\xE4\xB8\xAD\r\nint x;\r\n";
    const auto path = writeFile("cjk.cpp", payload);
    const auto result = readUtf8File(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().content, payload);  // CRLF untouched (binary mode)
    EXPECT_EQ(result.value().replacedInvalidSequences, 0u);
}

TEST_F(Utf8FileTest, StripsBomByDefault)
{
    const auto path = writeFile("bom.txt", "\xEF\xBB\xBFhello");
    const auto result = readUtf8File(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().hadBom);
    EXPECT_EQ(result.value().content, "hello");
    EXPECT_EQ(result.value().byteSize, 8u);  // 3 BOM bytes + 5 payload bytes
}

TEST_F(Utf8FileTest, KeepsBomWhenAsked)
{
    const auto path = writeFile("bom_keep.txt", "\xEF\xBB\xBFhello");
    ReadOptions options;
    options.stripBom = false;
    const auto result = readUtf8File(path, options);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().hadBom);
    EXPECT_EQ(result.value().content, "\xEF\xBB\xBFhello");
}

TEST_F(Utf8FileTest, EmptyFile)
{
    const auto path = writeFile("empty.txt", "");
    const auto result = readUtf8File(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().content.empty());
    EXPECT_EQ(result.value().byteSize, 0u);
}

// --- sanitization ------------------------------------------------------------

TEST_F(Utf8FileTest, ReplacesInvalidBytesWithReplacementChar)
{
    const auto path = writeFile("bad.txt", "a\xC3\x28" "b");
    // \xC3 expects a continuation, gets '(' (0x28): one replacement + 'b' kept.
    const auto result = readUtf8File(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().content, "a\xEF\xBF\xBD(b");
    EXPECT_EQ(result.value().replacedInvalidSequences, 1u);
}

TEST_F(Utf8FileTest, StrictModeRejectsInvalidEncoding)
{
    const auto path = writeFile("bad_strict.txt", "ok\xFF");
    ReadOptions options;
    options.strictUtf8 = true;
    const auto result = readUtf8File(path, options);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), ReadError::InvalidEncoding);
}

TEST_F(Utf8FileTest, StrictModeAcceptsValidUtf8)
{
    const auto path = writeFile("good_strict.txt", "你好, world");
    ReadOptions options;
    options.strictUtf8 = true;
    const auto result = readUtf8File(path, options);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().content, "你好, world");
}

// --- size limits --------------------------------------------------------------

TEST_F(Utf8FileTest, RejectsTooLargeFile)
{
    const auto path = writeFile("large.txt", "0123456789");
    ReadOptions options;
    options.maxBytes = 5;
    const auto result = readUtf8File(path, options);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), ReadError::TooLarge);
}

TEST_F(Utf8FileTest, ZeroMeansNoLimit)
{
    const auto path = writeFile("limit0.txt", "0123456789");
    ReadOptions options;
    options.maxBytes = 0;
    const auto result = readUtf8File(path, options);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().content, "0123456789");
}

// --- error paths ---------------------------------------------------------------

TEST_F(Utf8FileTest, MissingFileIsNotFound)
{
    const fs::path missing = dir_ / "does_not_exist.txt";
    const auto result = readUtf8File(missing);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), ReadError::NotFound);
}

TEST_F(Utf8FileTest, DirectoryIsNotARegularFile)
{
    const auto result = readUtf8File(dir_);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), ReadError::NotARegularFile);
}

// --- multiple chunks (exercise the growth path) --------------------------------

TEST_F(Utf8FileTest, LargeFileSpansMultipleChunks)
{
    std::string payload(200 * 1024, 'x');  // 200 KiB > one 64 KiB chunk
    payload.replace(100 * 1024, 3, "\xE4\xBD\xA0");  // inject '你' mid-file
    const auto path = writeFile("multi_chunk.txt", payload);
    const auto result = readUtf8File(path);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().content.size(), payload.size());
    EXPECT_EQ(result.value().content.substr(100 * 1024, 3), "\xE4\xBD\xA0");
    EXPECT_EQ(result.value().replacedInvalidSequences, 0u);
}

// --- error descriptions ---------------------------------------------------------

TEST(ReadErrorToString, AllValuesHaveDescriptions)
{
    using LightCpp::Core::Text::toString;
    EXPECT_FALSE(toString(ReadError::NotFound).empty());
    EXPECT_FALSE(toString(ReadError::NotARegularFile).empty());
    EXPECT_FALSE(toString(ReadError::TooLarge).empty());
    EXPECT_FALSE(toString(ReadError::IoError).empty());
    EXPECT_FALSE(toString(ReadError::InvalidEncoding).empty());
}

}  // namespace
