// LightCppText - encoding conversion to UTF-8 (implementation).

#include "LightCpp/Text/Converter.hpp"

#include <cerrno>
#include <cstring>
#include <limits>

#ifndef _WIN32
#include <iconv.h>
#endif

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
/// GB18030's code page. Worth naming: it is the one code page here whose
/// WideCharToMultiByte does not behave like the others (see encodeFromQString).
constexpr UINT kGb18030CodePage = 54936;

/// Windows code page for a legacy encoding.
std::optional<UINT> codePageFor(TextEncoding encoding) noexcept
{
    switch (encoding) {
    case TextEncoding::Gb18030: return kGb18030CodePage;
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
    // MultiByteToWideChar takes the length as an int; a buffer larger than that
    // cannot be passed faithfully, and truncating would decode only a prefix
    // and silently drop the tail. The IDE caps reads far below this, so the
    // guard is a correctness backstop rather than a reachable path.
    if (bytes.size() > std::numeric_limits<int>::max()) {
        return std::nullopt;
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

#ifndef _WIN32
/// iconv codec names for the legacy encodings supported by the editor.
const char* iconvName(TextEncoding encoding) noexcept
{
    switch (encoding) {
    case TextEncoding::Gb18030: return "GB18030";
    case TextEncoding::Gbk: return "GBK";
    case TextEncoding::Big5: return "BIG5";
    case TextEncoding::ShiftJis: return "SHIFT_JIS";
    case TextEncoding::EucKr: return "EUC-KR";
    case TextEncoding::Windows1252: return "WINDOWS-1252";
    default: return nullptr;
    }
}

/// Strict conversion through iconv. No //TRANSLIT suffix is used: a
/// character that cannot be represented must fail instead of being replaced
/// with a best-fit character or '?'. The output buffer grows only when the
/// converter asks for more space.
std::optional<QByteArray> iconvConvert(
    QByteArrayView input, const char* targetEncoding, const char* sourceEncoding)
{
    iconv_t converter = iconv_open(targetEncoding, sourceEncoding);
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        return std::nullopt;
    }
    struct Closer {
        iconv_t value;
        ~Closer() { iconv_close(value); }
    } closer{converter};

    const std::size_t inputSize = static_cast<std::size_t>(input.size());
    if (inputSize > (std::numeric_limits<std::size_t>::max() - 16) / 4) {
        return std::nullopt;
    }
    std::size_t capacity = qMax<std::size_t>(64, inputSize * 4 + 16);
    if (capacity > static_cast<std::size_t>(
                       std::numeric_limits<qsizetype>::max())) {
        return std::nullopt;
    }
    QByteArray output(static_cast<qsizetype>(capacity), Qt::Uninitialized);
    std::size_t used = 0;
#ifdef __APPLE__
    const char* inputPointer = input.constData();
#else
    char* inputPointer = const_cast<char*>(input.constData());
#endif
    std::size_t inputRemaining = inputSize;

    auto grow = [&]() {
        if (capacity > static_cast<std::size_t>(
                           std::numeric_limits<qsizetype>::max()) / 2) {
            return false;
        }
        capacity *= 2;
        output.resize(static_cast<qsizetype>(capacity));
        return true;
    };

    while (inputRemaining > 0) {
        char* outputPointer = output.data() + used;
        std::size_t outputRemaining = capacity - used;
        const std::size_t converted = iconv(
            converter, &inputPointer, &inputRemaining,
            &outputPointer, &outputRemaining);
        used = capacity - outputRemaining;
        if (converted != static_cast<std::size_t>(-1)) {
            if (converted != 0) {
                return std::nullopt;  // non-reversible conversion
            }
            continue;
        }
        if (errno != E2BIG || !grow()) {
            return std::nullopt;
        }
    }

    // Flush any stateful encoding shift sequence. The selected legacy codecs
    // are normally stateless, but this keeps the helper correct by contract.
    for (;;) {
        char* outputPointer = output.data() + used;
        std::size_t outputRemaining = capacity - used;
        const std::size_t converted = iconv(
            converter, nullptr, nullptr, &outputPointer, &outputRemaining);
        used = capacity - outputRemaining;
        if (converted != static_cast<std::size_t>(-1)) {
            if (converted != 0) {
                return std::nullopt;
            }
            break;
        }
        if (errno != E2BIG || !grow()) {
            return std::nullopt;
        }
    }

    output.resize(static_cast<qsizetype>(used));
    return output;
}
#endif  // !_WIN32

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
        // Strict: invalid sequences must fail the conversion, not silently
        // become U+FFFD. The auto-detected path only lands here after the
        // detector validated the buffer, so nothing changes for it — but a
        // manual encoding override (readTextFileWithEncoding) bypasses that
        // validation, and lenient decoding used to produce U+FFFD soup that
        // a later Ctrl+S wrote back over the original bytes, destroying the
        // file's real text.
        QStringDecoder decoder(QStringConverter::Utf8);
        QString decoded = decoder(bytes);
        if (decoder.hasError()) {
            return std::nullopt;
        }
        return decoded;
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
        // pass them through instead of reporting an error). Reject the
        // *unpaired* ones to enforce strict UTF-16/32 conformance.
        //
        // A well-formed pair is not an error: it is how UTF-16 spells every
        // code point above U+FFFF (an emoji, 𠮷, …), and Qt keeps it as two
        // QChars with the same D800-DFFF range. Rejecting the whole range —
        // rather than only the unpaired half — used to make every file
        // containing such a character fail to open.
        for (qsizetype i = 0; i < decoded.size(); ++i) {
            const char16_t cu = decoded[i].unicode();
            if (cu >= 0xDC00u && cu <= 0xDFFFu) {
                return std::nullopt;  // low surrogate with nothing before it
            }
            if (cu >= 0xD800u && cu <= 0xDBFFu) {
                if (i + 1 >= decoded.size()) {
                    return std::nullopt;  // high surrogate at end of text
                }
                const char16_t low = decoded[i + 1].unicode();
                if (low < 0xDC00u || low > 0xDFFFu) {
                    return std::nullopt;  // high surrogate not paired
                }
                ++i;  // skip the low half of a well-formed pair
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
        const char* codec = iconvName(encoding);
        if (codec == nullptr) {
            return std::nullopt;
        }
        const auto utf8 = iconvConvert(bytes, "UTF-8", codec);
        return utf8.has_value()
            ? std::optional<QString>(QString::fromUtf8(*utf8))
            : std::nullopt;
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

ConvertResult convertUtf8Lenient(QByteArrayView bytes)
{
    // QString::fromUtf8 is the lenient decoder: every byte that does not
    // complete a character becomes U+FFFD, and decoding carries on after the
    // maximal subpart. That is what "nearly UTF-8" needs — the alternative
    // (strict failure) sends the whole buffer to the local code page.
    //
    // No BOM handling: a BOM would have been detected before this point
    // (detectEncoding returns on the BOM gate), so a BOM cannot be present.
    const QString decoded =
        QString::fromUtf8(bytes.constData(), static_cast<qsizetype>(bytes.size()));
    QStringEncoder encoder(QStringConverter::Utf8);
    QByteArray utf8 = encoder(decoded);
    if (encoder.hasError()) {
        // Encoding a QString we just built can only fail on an unpaired
        // surrogate, which a decoder never produces — but a failure must not
        // look like success.
        return ConvertResult::failure(ConvertError::InvalidInput);
    }
    return ConvertResult::success(std::move(utf8));
}

ConvertResult encodeFromQString(const QString& text, TextEncoding encoding, bool addBom)
{
    QByteArray out;

    switch (encoding) {
    // -- Unicode families: Qt built-ins ------------------------------------
    case TextEncoding::Utf8: {
        QStringEncoder encoder(QStringConverter::Utf8);
        out = encoder(text);
        if (encoder.hasError()) {
            return ConvertResult::failure(ConvertError::InvalidInput);
        }
        // A UTF-8 BOM is written only when the caller asks for it — the
        // decision belongs to Text::bomOnSave(), which keeps whatever the
        // file had. Saving a BOM-bearing file without one turns the BOM
        // into three text characters on the next load, and makes MSVC fall
        // back to the ANSI code page (C4819 on every Chinese comment).
        if (addBom) {
            if (const std::optional<BomSpec> bom = bomFor(encoding)) {
                out.prepend(bom->bytes, bom->length);
            }
        }
        break;
    }
    case TextEncoding::Utf16Le:
    case TextEncoding::Utf16Be:
    case TextEncoding::Utf32Le:
    case TextEncoding::Utf32Be: {
        QStringEncoder encoder(
            encoding == TextEncoding::Utf16Le ? QStringConverter::Utf16LE
            : encoding == TextEncoding::Utf16Be ? QStringConverter::Utf16BE
            : encoding == TextEncoding::Utf32Le ? QStringConverter::Utf32LE
                                                : QStringConverter::Utf32BE);
        out = encoder(text);
        if (encoder.hasError()) {
            return ConvertResult::failure(ConvertError::InvalidInput);
        }
        if (addBom) {
            if (const std::optional<BomSpec> bom = bomFor(encoding)) {
                out.prepend(bom->bytes, bom->length);
            }
        }
        break;
    }
    case TextEncoding::Latin1: {
        QStringEncoder encoder(QStringConverter::Latin1);
        out = encoder(text);
        if (encoder.hasError()) {
            return ConvertResult::failure(ConvertError::InvalidInput);
        }
        break;
    }

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
            return ConvertResult::failure(ConvertError::UnsupportedEncoding);
        }
        if (text.isEmpty()) {
            break;  // nothing to encode
        }
        // WideCharToMultiByte takes the length as an int (see winDecode): a
        // longer input cannot be converted faithfully, so fail rather than
        // truncate it and save a prefix of the user's file over the whole.
        if (text.size() > std::numeric_limits<int>::max()) {
            return ConvertResult::failure(ConvertError::InvalidInput);
        }
        const wchar_t* src = reinterpret_cast<const wchar_t*>(text.constData());
        const int srcLen = static_cast<int>(text.size());
        // WC_NO_BEST_FIT_CHARS: without it Windows silently substitutes
        // look-alike characters ("best fit") for characters the code page
        // cannot represent, and `usedDefault` is NOT set for those — the
        // guard below would pass corrupted text through as a success.
        // With the flag every unmappable character sets `usedDefault` and
        // the conversion fails, which the caller reports to the user
        // instead of destroying their text on disk.
        //
        // CP54936 (GB18030) is the exception: its converter refuses both the
        // flag (ERROR_INVALID_FLAGS) and a non-null lpUsedDefaultChar
        // (ERROR_INVALID_PARAMETER), so asking for either made *every*
        // GB18030 save fail with "input is not valid in the encoding" —
        // plain ASCII included. It gets flags = 0 and no out-parameter, and
        // does its substitution check by decoding the result back below.
        const bool selfChecking = (*codePage == kGb18030CodePage);
        const DWORD encodeFlags = selfChecking ? 0 : WC_NO_BEST_FIT_CHARS;
        const int size = ::WideCharToMultiByte(
            *codePage, encodeFlags, src, srcLen, nullptr, 0, nullptr, nullptr);
        if (size <= 0) {
            return ConvertResult::failure(ConvertError::InvalidInput);
        }
        out.resize(size, '\0');
        BOOL usedDefault = FALSE;
        const int written = ::WideCharToMultiByte(
            *codePage, encodeFlags, src, srcLen, out.data(), size, nullptr,
            selfChecking ? nullptr : &usedDefault);
        if (written != size) {
            return ConvertResult::failure(ConvertError::InvalidInput);
        }
        if (selfChecking) {
            // GB18030's converter also substitutes '?' without saying so:
            // surrogate pairs it cannot map (U+20BB7 on this Windows build)
            // come back as 0x3F. Decoding the bytes again is the only way to
            // notice, and a character the encoding cannot carry must fail
            // rather than replace the user's text with question marks.
            const std::optional<QString> back =
                winDecode(QByteArrayView(out), *codePage);
            if (!back.has_value() || *back != text) {
                return ConvertResult::failure(ConvertError::InvalidInput);
            }
        } else if (usedDefault) {
            // usedDefault = some character had to be substituted with the code
            // page's default char — the encoding cannot represent the text, so
            // fail instead of silently corrupting it.
            return ConvertResult::failure(ConvertError::InvalidInput);
        }
#else
        const char* codec = iconvName(encoding);
        if (codec == nullptr) {
            return ConvertResult::failure(ConvertError::UnsupportedEncoding);
        }
        const QByteArray utf8 = text.toUtf8();
        const auto converted = iconvConvert(QByteArrayView(utf8), codec, "UTF-8");
        if (!converted.has_value()) {
            return ConvertResult::failure(ConvertError::InvalidInput);
        }
        out = *converted;
#endif
        break;
    }

    default:
        return ConvertResult::failure(ConvertError::UnsupportedEncoding);
    }

    return ConvertResult::success(std::move(out));
}

}  // namespace LightCpp::Text
