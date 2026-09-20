// LightCppText - encoding conversion to UTF-8 (implementation).

#include "LightCpp/Text/Converter.hpp"

#include <cstring>

#include <QStringConverter>
#include <QStringDecoder>
#include <QStringEncoder>

#ifdef _WIN32
#include <windows.h>
#endif

namespace LightCpp::Text {

namespace {

/// Byte-order mark bytes per encoding (used to strip/consume the BOM).
struct BomSpec {
    const char* bytes;
    int length;
};

std::optional<BomSpec> bomFor(TextEncoding encoding) noexcept
{
    switch (encoding) {
    case TextEncoding::Utf8: return BomSpec{"\xEF\xBB\xBF", 3};
    case TextEncoding::Utf16Le: return BomSpec{"\xFF\xFE", 2};
    case TextEncoding::Utf16Be: return BomSpec{"\xFE\xFF", 2};
    case TextEncoding::Utf32Le: return BomSpec{"\xFF\xFE\x00\x00", 4};
    case TextEncoding::Utf32Be: return BomSpec{"\x00\x00\xFE\xFF", 4};
    default: return std::nullopt;
    }
}

#ifdef _WIN32
/// Windows code page for a legacy encoding.
std::optional<UINT> codePageFor(TextEncoding encoding) noexcept
{
    switch (encoding) {
    case TextEncoding::Gb18030: return 54936;  // GB18030
    case TextEncoding::Gbk: return 936;        // GBK / CP936
    case TextEncoding::Big5: return 950;
    case TextEncoding::ShiftJis: return 932;
    case TextEncoding::EucKr: return 51949;
    case TextEncoding::Windows1252: return 1252;
    default: return std::nullopt;
    }
}

/// Strict decoding via MultiByteToWideChar; nullopt on invalid input.
std::optional<QString> winDecode(QByteArrayView bytes, UINT codePage)
{
    if (bytes.isEmpty()) {
        return QString();
    }
    const int srcLen = static_cast<int>(bytes.size());
    const char* src = bytes.constData();

    const int wlen = ::MultiByteToWideChar(
        codePage, MB_ERR_INVALID_CHARS, src, srcLen, nullptr, 0);
    if (wlen <= 0) {
        return std::nullopt;
    }
    QString out;
    out.resize(wlen);
    const int converted = ::MultiByteToWideChar(
        codePage, MB_ERR_INVALID_CHARS, src, srcLen,
        reinterpret_cast<wchar_t*>(out.data()), wlen);
    if (converted != wlen) {
        return std::nullopt;
    }
    return out;
}
#endif  // _WIN32

}  // namespace

const char* toString(ConvertError error) noexcept
{
    switch (error) {
    case ConvertError::UnsupportedEncoding: return "encoding has no converter";
    case ConvertError::InvalidInput: return "input is not valid in the encoding";
    case ConvertError::PlatformUnsupported:
        return "legacy codec unavailable on this platform/build";
    }
    return "unknown error";
}

std::optional<QString> decodeToQString(
    QByteArrayView bytes,
    TextEncoding encoding,
    bool stripBom,
    bool* hadBom)
{
    if (hadBom != nullptr) {
        *hadBom = false;
    }

    // -- BOM handling --------------------------------------------------------
    if (const std::optional<BomSpec> bom = bomFor(encoding)) {
        if (bytes.size() >= bom->length
            && QByteArrayView(bytes.constData(), bom->length)
                   == QByteArrayView(bom->bytes, bom->length)) {
            if (hadBom != nullptr) {
                *hadBom = true;
            }
            if (stripBom) {
                bytes = bytes.slice(bom->length);
            }
        }
    }

    if (bytes.isEmpty()) {
        return QString();
    }

    switch (encoding) {
    // -- Unicode families: Qt built-ins ----------------------------------
    case TextEncoding::Utf8: {
        // Invalid sequences are replaced with U+FFFD by Qt, matching the
        // core module's default sanitize policy. (Detection validated the
        // buffer before choosing UTF-8.)
        return QString::fromUtf8(bytes);
    }
    case TextEncoding::Utf16Le:
    case TextEncoding::Utf16Be:
    case TextEncoding::Utf32Le:
    case TextEncoding::Utf32Be: {
        QStringDecoder decoder(
            encoding == TextEncoding::Utf16Le ? QStringConverter::Utf16LE
            : encoding == TextEncoding::Utf16Be ? QStringConverter::Utf16BE
            : encoding == TextEncoding::Utf32Le ? QStringConverter::Utf32LE
                                                : QStringConverter::Utf32BE);
        QString decoded = decoder(bytes);
        if (decoder.hasError()) {
            return std::nullopt;
        }
        // Qt's decoder is lenient with lone surrogates (it may silently
        // pass them through instead of reporting an error). Reject any
        // unpaired surrogate to enforce strict UTF-16/32 conformance.
        for (const QChar ch : decoded) {
            const char16_t cu = ch.unicode();
            if (cu >= 0xD800u && cu <= 0xDFFFu) {
                return std::nullopt;
            }
        }
        return decoded;
    }
    case TextEncoding::Latin1:
        return QString::fromLatin1(bytes);

    // -- Legacy codecs: platform API ----------------------------------------
    case TextEncoding::Gb18030:
    case TextEncoding::Gbk:
    case TextEncoding::Big5:
    case TextEncoding::ShiftJis:
    case TextEncoding::EucKr:
    case TextEncoding::Windows1252: {
#ifdef _WIN32
        const std::optional<UINT> codePage = codePageFor(encoding);
        if (!codePage.has_value()) {
            return std::nullopt;
        }
        return winDecode(bytes, *codePage);
#else
        // Non-Windows builds rely on Qt+ICU; the MSYS2 build used in
        // development has no ICU support, so this path is Windows-first for
        // now (see README "Platform notes").
        Q_UNUSED(bytes);
        return std::nullopt;
#endif
    }

    default:
        return std::nullopt;
    }
}

ConvertResult convertToUtf8(QByteArrayView bytes, TextEncoding encoding, bool stripBom)
{
    std::optional<QString> decoded = decodeToQString(bytes, encoding, stripBom);
    if (!decoded.has_value()) {
        return ConvertResult::failure(ConvertError::InvalidInput);
    }

    QStringEncoder encoder(QStringConverter::Utf8);
    QByteArray utf8 = encoder(*decoded);
    if (encoder.hasError()) {
        return ConvertResult::failure(ConvertError::InvalidInput);
    }
    return ConvertResult::success(std::move(utf8));
}

}  // namespace LightCpp::Text
