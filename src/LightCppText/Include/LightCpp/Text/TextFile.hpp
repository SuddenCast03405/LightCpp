// LightCppText - high-level "read a text file in any supported encoding"
// pipeline built on QFile, detectEncoding() and convertToUtf8().

#pragma once

#include <cstdint>

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include "LightCpp/Core/Result.hpp"
#include "LightCpp/Text/Encoding.hpp"

namespace LightCpp::Text {

/// Why reading a detected text file failed.
enum class TextFileError {
    NotFound,
    NotARegularFile,
    TooLarge,
    ReadError,
    BinaryFile,
    UnsupportedConversion,
    DecodeError,
};

/// Human-readable description of a TextFileError.
[[nodiscard]] const char* toString(TextFileError error) noexcept;

/// The line-ending style a file's bytes use.
enum class LineEnding {
    None,  ///< no newline at all (empty file or single line without \n)
    Lf,    ///< \n line feeds (Unix style)
    CrLf,  ///< \r\n pairs (Windows style)
};

/// Human-readable name: "LF", "CRLF" or "" for None — the exact strings
/// the status-bar line-ending combo uses.
[[nodiscard]] const char* toString(LineEnding ending) noexcept;

/// Count the line endings in the bytes and report the dominant style.
///
/// A lone \r (classic Mac, essentially extinct) is ignored. Ties and
/// newline-free content report None, meaning "no evidence" — callers fall
/// back to their own default rather than guessing from nothing.
[[nodiscard]] LineEnding detectLineEndings(QByteArrayView bytes) noexcept;

/// Payload of a successful read.
struct TextFileContent {
    /// Decoded text as UTF-8, BOM stripped, newlines untouched.
    QByteArray utf8;
    /// Encoding detected for the file.
    TextEncoding encoding = TextEncoding::Unknown;
    /// The file started with a byte-order mark.
    bool hadBom = false;
    /// Original file size in bytes (BOM included).
    std::int64_t byteSize = 0;
    /// Detection confidence, 0..1000 (see EncodingDetection::score).
    int detectionScore = 0;
    /// True for BOM-based or strong heuristic detection.
    bool detectionReliable = false;
    /// The line-ending style the file's bytes use, so the editor can keep
    /// it on save instead of rewriting every line to the global default.
    LineEnding lineEndings = LineEnding::None;
};

/// Either a TextFileContent or a TextFileError.
using TextFileResult = LightCpp::Core::Result<TextFileContent, TextFileError>;

/// Options for readDetectedTextFile.
struct TextFileOptions {
    /// Maximum accepted file size in bytes; 0 means unlimited.
    std::int64_t maxBytes = 64 * 1024 * 1024;
};

/// Read the whole file at `path`, detect its encoding and return the
/// content as UTF-8.
///
/// Uses Qt file APIs (QFile/QFileInfo); binary mode, no newline translation.
/// Safe to call from background threads (never touches the GUI), per the
/// IDE's threading rules.
[[nodiscard]] TextFileResult readDetectedTextFile(
    const QString& path,
    const TextFileOptions& options = {});

/// Read the whole file at `path` and decode it with the given `encoding`,
/// bypassing automatic detection. Used when the user picks a specific
/// encoding from the GUI drop-down to override a misdetected file.
///
/// Uses Qt file APIs (QFile/QFileInfo); binary mode, no newline translation.
/// Safe to call from background threads.
[[nodiscard]] TextFileResult readTextFileWithEncoding(
    const QString& path,
    TextEncoding encoding,
    const TextFileOptions& options = {});

}  // namespace LightCpp::Text
