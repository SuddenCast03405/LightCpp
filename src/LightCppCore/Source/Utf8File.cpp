// LightCppCore - UTF-8 text file reading (implementation).

#include "LightCpp/Core/Utf8File.hpp"

#include "LightCpp/Core/Utf8.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

namespace LightCpp::Core::Text {

std::string toString(ReadError error)
{
    switch (error) {
    case ReadError::NotFound:
        return "file not found";
    case ReadError::NotARegularFile:
        return "path is not a regular file";
    case ReadError::TooLarge:
        return "file exceeds the configured size limit";
    case ReadError::IoError:
        return "I/O error while reading the file";
    case ReadError::InvalidEncoding:
        return "file is not valid UTF-8";
    }
    return "unknown error";
}

namespace {

/// RAII wrapper around std::FILE* that never leaks, even on early returns.
struct FileHandle {
    std::FILE* handle{nullptr};

    explicit FileHandle(std::FILE* h) noexcept : handle(h) {}
    ~FileHandle()
    {
        if (handle != nullptr) {
            std::fclose(handle);
        }
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
};

/// Replace every invalid UTF-8 sequence in `bytes` with U+FFFD and return the
/// fixed-up content plus the number of replacements made.
std::pair<std::string, std::size_t> sanitizeUtf8(std::string_view bytes)
{
    std::size_t errors = validateUtf8(bytes);
    if (errors == 0) {
        return {std::string(bytes), 0};
    }

    std::string fixed;
    fixed.reserve(bytes.size() + errors * 2);

    // U+FFFD encoded in UTF-8 is EF BF BD (3 bytes).
    constexpr std::string_view kReplacementUtf8{"\xEF\xBF\xBD"};

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto result = decodeCodePoint(bytes.substr(offset));
        if (result.valid) {
            fixed.append(bytes.substr(offset, result.consumed));
        } else {
            fixed.append(kReplacementUtf8);
        }
        offset += (result.consumed > 0) ? result.consumed : 1;
    }
    return {std::move(fixed), errors};
}

}  // namespace

ReadResult readUtf8File(const std::filesystem::path& path, const ReadOptions& options)
{
    std::error_code ec;

    // -- 1. Existence & type checks -----------------------------------------
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    if (ec) {
        // Distinguish "path missing" (ENOENT-style) from other stat failures.
        std::error_code probe;
        const bool exists = std::filesystem::exists(path, probe);
        return ReadResult::failure(exists ? ReadError::IoError : ReadError::NotFound);
    }
    if (!std::filesystem::is_regular_file(status)) {
        return ReadResult::failure(ReadError::NotARegularFile);
    }

    // -- 2. Size limit -------------------------------------------------------
    if (options.maxBytes != 0) {
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            return ReadResult::failure(ReadError::IoError);
        }
        if (static_cast<std::uint64_t>(size) > options.maxBytes) {
            return ReadResult::failure(ReadError::TooLarge);
        }
    }

    // -- 3. Open & read ------------------------------------------------------
#ifdef _WIN32
    // On Windows std::filesystem::path::c_str() yields wchar_t, so use the
    // wide-character fopen to support non-ANSI paths (e.g. Chinese filenames).
    FileHandle file{::_wfopen(path.c_str(), L"rb")};
#else
    FileHandle file{::fopen(path.c_str(), "rb")};
#endif
    if (file.handle == nullptr) {
        return ReadResult::failure(ReadError::IoError);
    }

    std::string buffer;
    constexpr std::size_t kChunkSize = 64 * 1024;
    buffer.resize(kChunkSize);

    std::size_t totalRead = 0;
    while (true) {
        const std::size_t n =
            std::fread(buffer.data() + totalRead, 1, kChunkSize, file.handle);
        if (n == 0) {
            if (std::ferror(file.handle) != 0) {
                return ReadResult::failure(ReadError::IoError);
            }
            break;  // Clean EOF.
        }
        totalRead += n;
        if (n < kChunkSize) {
            break;  // Short read: EOF reached.
        }
        // Grow for the next chunk, guarding against size_t overflow.
        if (totalRead > std::size_t(-1) - kChunkSize) {
            return ReadResult::failure(ReadError::IoError);
        }
        buffer.resize(totalRead + kChunkSize);
    }
    buffer.resize(totalRead);

    TextFile result;
    result.byteSize = static_cast<std::uint64_t>(totalRead);

    // -- 4. BOM handling ----------------------------------------------------
    std::string_view view{buffer};
    if (view.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        result.hadBom = true;
        if (options.stripBom) {
            view.remove_prefix(kUtf8Bom.size());
        }
    }

    // -- 5. UTF-8 sanitization ----------------------------------------------
    if (options.strictUtf8) {
        if (validateUtf8(view) != 0) {
            return ReadResult::failure(ReadError::InvalidEncoding);
        }
        result.content.assign(view);
    } else {
        auto [content, replaced] = sanitizeUtf8(view);
        result.content = std::move(content);
        result.replacedInvalidSequences = replaced;
    }

    return ReadResult::success(std::move(result));
}

}  // namespace LightCpp::Core::Text
