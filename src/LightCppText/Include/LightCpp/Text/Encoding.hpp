// LightCppText - text encoding detection (Qt 6 based).
//
// Detects the encoding of a byte buffer using:
//   1. Unicode byte-order marks (UTF-8/16/32),
//   2. structural validation (UTF-8 via the core validator, UTF-16/32
//      no-BOM alignment heuristics, binary rejection),
//   3. for everything that is not Unicode, falls back to the system's
//      local "ANSI" code page (GBK on zh-CN Windows, Shift-JIS on ja-JP,
//      EUC-KR on ko-KR, Windows-1252 on en-US, etc.).
//
// The local-ANSI fallback is deliberately simpler and more reliable than
// scoring legacy candidates against each other: real-world non-Unicode
// files on a user's machine almost always match the machine's locale.
// Users who need to override the choice can do so via the encoding
// drop-down in the GUI (which re-opens the file with the chosen codec).
//
// Legacy codecs are decoded through the platform converter API
// (MultiByteToWideChar on Windows; Qt's QStringConverter has no ICU support
// in the MSYS2 build used for development).

#pragma once

#include <cstdint>
#include <optional>

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

namespace LightCpp::Text {

/// Text encodings the IDE can detect/convert. Ordering groups Unicode
/// encodings first, then CJK legacies, then 8-bit western, then non-text.
enum class TextEncoding : std::uint8_t {
    Utf8,
    Utf16Le,
    Utf16Be,
    Utf32Le,
    Utf32Be,
    Gb18030,
    Gbk,
    Big5,
    ShiftJis,
    EucKr,
    Windows1252,
    Latin1,
    Binary,
    Unknown,
};

/// Canonical name for an encoding (IANA-style, e.g. "UTF-8", "GB18030").
[[nodiscard]] const char* toName(TextEncoding encoding) noexcept;

/// True when the encoding requires the platform legacy converter API
/// (unavailable on some builds/platforms; see decodeToQString).
[[nodiscard]] bool isLegacyCodec(TextEncoding encoding) noexcept;

/// Whether saving a buffer in `encoding` must write a byte-order mark.
///
/// UTF-16/32 always do: without one, reopening has to guess endianness.
///
/// UTF-8 follows the file it was loaded from. A UTF-8 BOM is not decoration —
/// other tools act on it (MSVC picks its code page from it, and without one
/// falls back to the ANSI code page, so Chinese comments come back as C4819
/// and mojibake). Dropping it on save silently changes how the file is read
/// everywhere else. The other direction matters too: a file that had no BOM
/// must not gain one, or the first line of every such file shows up as
/// modified in a diff.
///
/// Legacy code pages never take a BOM.
[[nodiscard]] bool bomOnSave(TextEncoding encoding, bool sourceHadBom) noexcept;

/// Parse an encoding name (case-insensitive, '-'/'_' ignored).
/// Returns nullopt for unknown names.
[[nodiscard]] std::optional<TextEncoding> fromName(QByteArrayView name);

/// The system's local "ANSI" code page as a TextEncoding. On a zh-CN
/// Windows system this is GBK (CP936); on ja-JP it is Shift-JIS (CP932);
/// on en-US it is Windows-1252. Used as the default for non-Unicode
/// byte streams when no BOM or UTF-8 structure is detected.
[[nodiscard]] TextEncoding localAnsiEncoding() noexcept;

/// Human-friendly name for the local ANSI code page, e.g. "ANSI (GBK)".
/// Suitable for display in the encoding drop-down.
[[nodiscard]] QString localAnsiDisplayName() noexcept;

/// Outcome of encoding detection over a byte buffer.
struct EncodingDetection {
    TextEncoding encoding = TextEncoding::Unknown;

    /// A byte-order mark was present (and will be stripped when converting).
    bool hadBom = false;

    /// Relative confidence, scaled to 0..1000. BOM-based detection scores
    /// 1000; heuristic detection scores less.
    int score = 0;

    /// True for BOM-based detection or a strong heuristic result.
    bool reliable = false;
};

/// Detect the encoding of `bytes`.
///
/// Thread-safe: pure function, no global mutable state.
[[nodiscard]] EncodingDetection detectEncoding(QByteArrayView bytes);

}  // namespace LightCpp::Text
