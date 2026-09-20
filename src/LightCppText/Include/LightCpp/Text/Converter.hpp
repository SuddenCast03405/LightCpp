// LightCppText - encoding conversion to UTF-8.

#pragma once

#include <optional>

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include "LightCpp/Core/Result.hpp"
#include "LightCpp/Text/Encoding.hpp"

namespace LightCpp::Text {

/// Why a conversion failed.
enum class ConvertError {
    /// The encoding enum value has no converter.
    UnsupportedEncoding,
    /// The input is not valid in the requested encoding (strict decoding).
    InvalidInput,
    /// The encoding needs the platform legacy converter API, which is not
    /// available in this build/platform (e.g. GBK on a non-Windows build
    /// without ICU support).
    PlatformUnsupported,
};

/// Either a UTF-8 payload or a ConvertError.
using ConvertResult = LightCpp::Core::Result<QByteArray, ConvertError>;

/// Human-readable description of a ConvertError.
[[nodiscard]] const char* toString(ConvertError error) noexcept;

/// Decode `bytes` in `encoding` into a QString.
///
/// When `stripBom` is true a leading BOM for that encoding is removed;
/// `*hadBom` (if non-null) always reports whether one was present.
///
/// Returns nullopt when the input is not strictly valid in the encoding, or
/// when the encoding is a legacy codec unavailable on this platform.
///
/// Thread-safe.
[[nodiscard]] std::optional<QString> decodeToQString(
    QByteArrayView bytes,
    TextEncoding encoding,
    bool stripBom = true,
    bool* hadBom = nullptr);

/// Convert `bytes` in `encoding` to UTF-8.
///
/// The returned payload never contains a BOM (stripped when `stripBom` is
/// true) and uses no newline translation (binary reads preserve CRLF).
[[nodiscard]] ConvertResult convertToUtf8(
    QByteArrayView bytes,
    TextEncoding encoding,
    bool stripBom = true);

}  // namespace LightCpp::Text
