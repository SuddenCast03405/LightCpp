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

#include <cstddef>
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

/// Name to show in a log line or a tooltip: "GBK" when the encoding was
/// proven, "GBK (guessed)" when the detector could only guess.
///
/// The distinction is the whole point of `EncodingDetection::reliable`, and
/// it used to have no consumer anywhere in the IDE: a file that was not valid
/// UTF-8 showed up as "[GBK, N bytes]" exactly like a file with a BOM, and the
/// user had no way to tell that the text on screen was the result of a guess —
/// until Ctrl+S wrote that guess back over the original bytes.
[[nodiscard]] QString encodingDisplayName(TextEncoding encoding, bool reliable);

/// What to tell the user about a guessed read — empty when `reliable`.
///
/// The rule lives here, next to the detection that produced the guess, for the
/// same reason `bomOnSave` does: the window is not the place where "what does
/// this flag mean for the user" gets decided, and a rule written inside a
/// `MainWindow` slot cannot be tested.
///
/// Two shapes, because the two guesses are not equally dangerous:
/// - `encoding` is UTF-8 and some sequences are damaged: the text is right
///   except for those spots, which became U+FFFD — saving rewrites them;
/// - anything else: the whole file was decoded through another code page, so
///   the text on screen may be entirely wrong.
[[nodiscard]] QString encodingGuessWarning(
    TextEncoding encoding,
    bool reliable,
    std::size_t invalidUtf8Sequences);

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

    /// How many byte sequences in the buffer are not well-formed UTF-8.
    ///
    /// Zero for every buffer that is plain ASCII or valid UTF-8, and also for
    /// UTF-16/32 and NUL-structured binary (the UTF-8 question does not apply
    /// there). It is only meaningful when `reliable` is false:
    ///
    /// - a small count means the file *is* UTF-8 with a few broken bytes (a
    ///   character cut in half by a writer that died mid-save, a stray byte
    ///   pasted in from a legacy tool). `encoding` then says Utf8 with
    ///   `reliable == false`, and the caller decodes leniently — the damaged
    ///   bytes become U+FFFD instead of the whole file turning into mojibake
    ///   in another code page;
    /// - a large count means the buffer really is some other encoding, and
    ///   `encoding` is the local ANSI fallback.
    std::size_t invalidUtf8Sequences = 0;
};

/// Detect the encoding of `bytes`.
///
/// Thread-safe: pure function, no global mutable state.
[[nodiscard]] EncodingDetection detectEncoding(QByteArrayView bytes);

}  // namespace LightCpp::Text
