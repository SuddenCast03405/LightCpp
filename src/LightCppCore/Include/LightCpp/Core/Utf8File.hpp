// LightCppCore - UTF-8 text file reading.
//
// Reads a whole file as UTF-8 text with:
//   * BOM detection and optional stripping,
//   * strict UTF-8 validation (invalid sequences are replaced with U+FFFD
//     using the "maximal subpart" strategy),
//   * an upper size limit to prevent accidentally loading binaries or huge
//     files into memory,
//   * fine-grained error reporting (missing file / IO error / too large /
//     invalid encoding) suitable for surfacing in the IDE UI.
//
// Everything here is pure standard library and thread-safe: safe to call from
// background threads (see "Asynchronous & UI Thread Safety" in the spec).

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace LightCpp::Core::Text {

/// Why a read attempt failed.
enum class ReadError {
    /// The file does not exist.
    NotFound,
    /// The path exists but is not a regular file (directory, device, ...).
    NotARegularFile,
    /// The file is larger than the configured limit.
    TooLarge,
    /// A read() or stat() level failure (permissions, sharing violation, IO).
    IoError,
    /// The file is not valid UTF-8 (only reported when strict mode is on).
    InvalidEncoding,
};

/// Human-readable description for a ReadError, suitable for UI display.
[[nodiscard]] std::string toString(ReadError error);

/// Result payload of a successful read.
struct TextFile {
    std::string content;      ///< File bytes with BOM stripped, newlines untouched.
    std::uint64_t byteSize{}; ///< Original file size in bytes (BOM included).
    bool hadBom{false};       ///< True when the file started with a UTF-8 BOM.
    /// Number of invalid code points that were replaced with U+FFFD.
    /// Zero in strict mode (strict reads fail instead of replacing).
    std::size_t replacedInvalidSequences{};
};

/// Outcome of readUtf8File: either a TextFile or a ReadError.
///
/// A tiny C++20-compatible sum type (std::expected is C++23, which is above
/// the project's enforced C++20 baseline, so it must not be used here).
class ReadResult {
public:
    /// Construct a successful result.
    static ReadResult success(TextFile file) noexcept
    {
        return ReadResult(std::move(file));
    }

    /// Construct a failed result.
    static ReadResult failure(ReadError error) noexcept
    {
        return ReadResult(error);
    }

    /// True when the read succeeded.
    [[nodiscard]] bool hasValue() const noexcept { return file_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    /// Access the payload; valid only when hasValue() is true.
    [[nodiscard]] const TextFile& value() const& noexcept { return *file_; }
    [[nodiscard]] TextFile& value() & noexcept { return *file_; }
    [[nodiscard]] TextFile&& takeValue() noexcept { return std::move(*file_); }

    /// The failure reason; valid only when hasValue() is false.
    [[nodiscard]] ReadError error() const noexcept { return error_; }

private:
    explicit ReadResult(TextFile file) noexcept : file_(std::move(file)) {}
    explicit ReadResult(ReadError error) noexcept : error_(error) {}

    std::optional<TextFile> file_;
    ReadError error_{ReadError::IoError};
};

/// Options controlling readUtf8File behaviour.
struct ReadOptions {
    /// Maximum accepted file size in bytes. Defaults to 32 MiB, enough for
    /// even the largest source files an IDE typically faces.
    std::uint64_t maxBytes{32u * 1024u * 1024u};
    /// Strip a leading UTF-8 BOM from the returned content.
    bool stripBom{true};
    /// When false (default) invalid bytes are replaced with U+FFFD and the
    /// read still succeeds. When true the read fails with InvalidEncoding.
    bool strictUtf8{false};
};

/// Read the whole file at `path` as UTF-8 text.
///
/// The file is opened with binary flags; no newline translation is performed.
/// `options.maxBytes == 0` means "no limit".
///
/// Thread-safety: const and safe to call from any thread; no global state.
[[nodiscard]] ReadResult readUtf8File(
    const std::filesystem::path& path,
    const ReadOptions& options = {});

}  // namespace LightCpp::Core::Text
