// LightCppCore - Encoding utilities shared by text file readers.
//
// Part of the LightC++ IDE (https://example.invalid/lightcpp) core library.
// Pure standard-library implementation, Qt-free on purpose: this layer may be
// used from background threads and unit tests without a QApplication.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace LightCpp::Core::Text {

/// A decoded code point plus the number of bytes it occupied in the input.
struct DecodeResult {
    std::uint32_t value{};   ///< Unicode scalar value, or replacement char on error.
    std::size_t consumed{};  ///< Bytes consumed (>=1 even on invalid input).
    bool valid{false};       ///< True when the input bytes were well-formed UTF-8.
};

/// Maximum number of bytes a single UTF-8 code point may occupy.
inline constexpr std::size_t kMaxCodePointSize = 4;

/// Unicode replacement character U+FFFD, returned for invalid input.
inline constexpr std::uint32_t kReplacementChar = 0xFFFDu;

/// UTF-8 BOM (EF BB BF), as it appears at the start of a file.
inline constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF"};

/// True when `byte` is a UTF-8 continuation byte (0b10xxxxxx).
[[nodiscard]] constexpr bool isContinuation(std::uint8_t byte) noexcept
{
    return (byte & 0xC0u) == 0x80u;
}

/// True when `cp` is a valid Unicode scalar value (excludes surrogates).
[[nodiscard]] constexpr bool isScalarValue(std::uint32_t cp) noexcept
{
    return cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu);
}

/// Decode one UTF-8 code point starting at `input[0]`.
///
/// Follows the WHATWG / Unicode "maximal subpart" strategy: on invalid input
/// the shortest invalid prefix is consumed and U+FFFD is emitted, so callers
/// can resynchronize without losing well-formed data that follows.
[[nodiscard]] constexpr DecodeResult decodeCodePoint(std::string_view input) noexcept
{
    if (input.empty()) {
        return {kReplacementChar, 0, true};  // Nothing to decode: treat as valid.
    }

    const std::uint8_t first = static_cast<std::uint8_t>(input.front());

    // Fast path: plain ASCII.
    if (first < 0x80u) {
        return {first, 1, true};
    }

    std::size_t expected = 0;
    std::uint32_t value = 0;

    if ((first & 0xE0u) == 0xC0u) {          // 110xxxxx -> 2-byte sequence
        expected = 2;
        value = first & 0x1Fu;
    } else if ((first & 0xF0u) == 0xE0u) {   // 1110xxxx -> 3-byte sequence
        expected = 3;
        value = first & 0x0Fu;
    } else if ((first & 0xF8u) == 0xF0u) {   // 11110xxx -> 4-byte sequence
        expected = 4;
        value = first & 0x07u;
    } else {
        // Stray continuation byte or invalid lead byte: one replacement char.
        return {kReplacementChar, 1, false};
    }

    if (input.size() < expected) {
        // Truncated sequence at end of buffer.
        return {kReplacementChar, input.size(), false};
    }

    for (std::size_t i = 1; i < expected; ++i) {
        const std::uint8_t cont = static_cast<std::uint8_t>(input[i]);
        if (!isContinuation(cont)) {
            // Maximal subpart: consume only the valid prefix, re-sync here.
            return {kReplacementChar, i, false};
        }
        value = (value << 6u) | (cont & 0x3Fu);
    }

    if (!isScalarValue(value)) {
        return {kReplacementChar, expected, false};
    }

    // Reject overlong encodings (e.g. 0xC0 0x80 for U+0000).
    constexpr std::uint32_t kMinValue[] = {0x0u, 0x0u, 0x80u, 0x800u, 0x10000u};
    if (value < kMinValue[expected]) {
        return {kReplacementChar, expected, false};
    }

    return {value, expected, true};
}

/// Validate a complete buffer as UTF-8 without decoding every code point into
/// a separate array. Returns the number of invalid code points found (0 == valid).
/// If `firstInvalidOffset` is non-null it receives the offset of the first
/// invalid byte (std::string_view::npos when the buffer is fully valid).
[[nodiscard]] constexpr std::size_t validateUtf8(
    std::string_view bytes,
    std::size_t* firstInvalidOffset = nullptr)
{
    std::size_t errors = 0;
    std::size_t offset = 0;
    bool reported = false;

    while (offset < bytes.size()) {
        const auto result = decodeCodePoint(bytes.substr(offset));
        if (!result.valid) {
            ++errors;
            if (!reported && firstInvalidOffset != nullptr) {
                *firstInvalidOffset = offset;
                reported = true;
            }
        }
        offset += (result.consumed > 0) ? result.consumed : 1;
    }

    if (firstInvalidOffset != nullptr && !reported) {
        *firstInvalidOffset = std::string_view::npos;
    }
    return errors;
}

}  // namespace LightCpp::Core::Text
