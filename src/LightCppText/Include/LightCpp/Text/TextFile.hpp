// LightCppText - high-level "read a text file in any supported encoding"
// pipeline built on QFile, detectEncoding() and convertToUtf8().

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

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
    Cr,    ///< bare \r (classic Mac style — rare, but a real file format)
};

/// Human-readable name: "LF", "CRLF", "CR" or "" for None — the exact strings
/// the status-bar line-ending combo uses.
[[nodiscard]] const char* toString(LineEnding ending) noexcept;

/// Count the line endings in the bytes and report the dominant style.
///
/// A lone \r counts as a line ending of its own: a file written by a classic
/// Mac tool has them everywhere, and answering "None" for it meant the save
/// path fell back to its own default and rewrote every line ending in the
/// file. Content with no newline at all reports None, meaning "no evidence" —
/// callers fall back to their own default rather than guessing from nothing.
/// The majority wins; on a tie the one-byte styles win (LF before CR), because
/// rewriting to CRLF would add a byte to every line.
///
/// Pass the encoding the bytes are in. For the single-byte encodings (and
/// Unknown, which treats them as bytes) a '\n' byte only ever appears as a
/// line feed, so counting raw bytes is exact. UTF-16/32 are different: a line
/// feed is a two- or four-byte code unit, and U+0A0D-style characters put a
/// 0x0A byte *inside* a character, so the scan walks whole code units in the
/// right byte order instead.
[[nodiscard]] LineEnding detectLineEndings(
    QByteArrayView bytes,
    TextEncoding encoding = TextEncoding::Unknown) noexcept;

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
    /// Broken UTF-8 sequences behind an unreliable detection: non-zero means
    /// the file is UTF-8 with that many damaged spots, and `utf8` below holds
    /// U+FFFD where they were (see EncodingDetection::invalidUtf8Sequences).
    /// Writing the buffer back replaces those bytes for good, so the caller
    /// has to show the user that the encoding was a guess.
    std::size_t invalidUtf8Sequences = 0;
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

/// Read the whole file at `path` as UTF-8 for a whole-file text scan.
///
/// A scan is not an editor: it wants every byte of a file or none of it, and
/// it must not report hits on text it had to guess at. So the file is read in
/// one piece and rejected outright when it is not plainly UTF-8 text — a NUL
/// byte in the first block (see the binary sniff below), or more bytes than
/// `maxBytes` (0 means no limit).
///
/// The sniff reads the file before looking at it rather than peeking at a
/// prefix and then reading "the rest": reading a prefix advances the position,
/// and a reader that forgets that loses exactly that prefix from the returned
/// text — the first 4 KiB of every file, which is where the includes and the
/// file header are.
///
/// Returns std::nullopt when the file cannot be opened, is too large or is not
/// plain text. Safe to call from background threads (never touches the GUI).
[[nodiscard]] std::optional<QString> readSearchableTextFile(
    const QString& path, std::int64_t maxBytes);

/// True when `relativePath` — relative to a scan root, with either '\' or '/'
/// separators — names a file inside a directory a source scan never looks at:
/// version-control metadata (`.git`, `.svn`, `.hg`, `.vs`) and build or cache
/// output (`build`, `cmake-build-*`, `.ccls-cache`, `.workbuddy`,
/// `node_modules`).
///
/// Every segment above the file name is checked, so `src/build/out.cpp` is
/// excluded exactly like `build/out.cpp`. Matching only the first segment is
/// how a scan ends up walking `.git` internals and spending its result cap on
/// generated files, leaving the real hits unreported.
[[nodiscard]] bool isUnderSkippedDirectory(QStringView relativePath);

/// Why writing a text file failed.
enum class WriteFileError {
    OpenFailed,    ///< the temporary file could not be created
    WriteFailed,   ///< the bytes could not be written or flushed to disk
    CommitFailed,  ///< the temporary file could not replace the target
};

/// Human-readable description of a WriteFileError.
[[nodiscard]] const char* toString(WriteFileError error) noexcept;

/// Write `bytes` to `path`, replacing the file's contents atomically.
///
/// The bytes go to a temporary file in the target's own directory and are
/// flushed before that temporary replaces `path`. A crash, a full disk or an
/// antivirus holding the file therefore leaves the previous contents intact.
/// A plain `QIODevice::Truncate` + write() cannot offer that: it cuts the
/// file down to zero bytes first, so a failure loses the old content *and*
/// leaves nothing of the new one.
///
/// Binary-safe (NUL bytes and CRLF pass through untouched); the caller
/// supplies the final byte sequence, see Converter for encoding and
/// line-ending selection.
///
/// Returns std::nullopt on success, or the reason the write failed. A value
/// coming back means the target still holds its previous contents and the
/// caller must not report a successful save. Safe to call from background
/// threads (never touches the GUI).
[[nodiscard]] std::optional<WriteFileError> writeFileAtomically(
    const QString& path, const QByteArray& bytes);

}  // namespace LightCpp::Text
