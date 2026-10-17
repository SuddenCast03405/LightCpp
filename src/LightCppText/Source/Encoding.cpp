// LightCppText - text encoding detection (implementation).

#include "LightCpp/Text/Encoding.hpp"

#include <array>
#include <string_view>

#include <QStringView>

#include "LightCpp/Core/Utf8.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace LightCpp::Text {

namespace {

struct BomEntry {
    const char* bytes;
    int length;
    TextEncoding encoding;
};

// Order matters: UTF-32LE (FF FE 00 00) must be checked before UTF-16LE
// (FF FE), otherwise the shorter BOM would shadow it.
constexpr std::array kBoms{
    BomEntry{"\x00\x00\xFE\xFF", 4, TextEncoding::Utf32Be},
    BomEntry{"\xFF\xFE\x00\x00", 4, TextEncoding::Utf32Le},
    BomEntry{"\xEF\xBB\xBF", 3, TextEncoding::Utf8},
    BomEntry{"\xFF\xFE", 2, TextEncoding::Utf16Le},
    BomEntry{"\xFE\xFF", 2, TextEncoding::Utf16Be},
};

/// How many broken UTF-8 sequences still count as "this file is UTF-8, some
/// bytes of it are damaged" rather than "this file is not UTF-8".
///
/// Three is the size of one character at most (a four-byte sequence cut in
/// half leaves at most three bytes behind as one bad run), and the percentage
/// test below keeps a couple of stray bytes from reclassifying a small legacy
/// file: `invalid * 100 <= size` means "at most one per cent".
constexpr std::size_t kMaxBrokenUtf8Sequences = 3;

/// True when the buffer "looks like" UTF-16 without a BOM: even length and
/// a large majority of code units carry NUL in the same position (high byte
/// for LE, low byte for BE). Pure-Latin UTF-16 has NUL in every unit, but
/// CJK/Arabic/… text legitimately has non-zero bytes there too — requiring
/// ALL of them to be NUL used to classify any non-Latin UTF-16 file as
/// binary (and `.dev`/`.vcxproj` projects written in UTF-16 failed to open
/// with it).
[[nodiscard]] bool looksLikeUtf16NoBom(QByteArrayView bytes, int stride) noexcept
{
    if (bytes.size() < 8 || bytes.size() % 2 != 0) {
        return false;
    }
    // qsizetype (not int): the buffers here are capped well below 2 GiB today,
    // but scaling a count by 10 in `int` would overflow past ~214M units and
    // silently flip the ratio test. Counting in the buffer's own width costs
    // nothing and removes the trap.
    qsizetype zeros = 0;
    qsizetype checked = 0;
    for (qsizetype i = 0; i + 1 < bytes.size(); i += 2) {
        const char hi = bytes[static_cast<qsizetype>(i) + (stride == 2 ? 1 : 0)];
        const char lo = bytes[static_cast<qsizetype>(i) + (stride == 2 ? 0 : 1)];
        ++checked;
        if (hi == '\0' && lo != '\0') {
            ++zeros;
        }
    }
    // ≥60% zero bytes in the fixed position is far beyond what any 8-bit
    // encoding or binary format produces by accident, while still admitting
    // UTF-16 text dominated by ASCII with CJK runs mixed in. A false "yes"
    // here is cheap: the strict UTF-16 decoder rejects garbage afterwards.
    return checked > 0 && zeros * 10 >= checked * 6;
}

}  // namespace

const char* toName(TextEncoding encoding) noexcept
{
    switch (encoding) {
    case TextEncoding::Utf8: return "UTF-8";
    case TextEncoding::Utf16Le: return "UTF-16LE";
    case TextEncoding::Utf16Be: return "UTF-16BE";
    case TextEncoding::Utf32Le: return "UTF-32LE";
    case TextEncoding::Utf32Be: return "UTF-32BE";
    case TextEncoding::Gb18030: return "GB18030";
    case TextEncoding::Gbk: return "GBK";
    case TextEncoding::Big5: return "Big5";
    case TextEncoding::ShiftJis: return "Shift-JIS";
    case TextEncoding::EucKr: return "EUC-KR";
    case TextEncoding::Windows1252: return "Windows-1252";
    case TextEncoding::Latin1: return "ISO-8859-1";
    case TextEncoding::Binary: return "binary";
    case TextEncoding::Unknown: return "unknown";
    }
    return "unknown";
}

bool isLegacyCodec(TextEncoding encoding) noexcept
{
    switch (encoding) {
    case TextEncoding::Gb18030:
    case TextEncoding::Gbk:
    case TextEncoding::Big5:
    case TextEncoding::ShiftJis:
    case TextEncoding::EucKr:
    case TextEncoding::Windows1252:
        return true;
    default:
        return false;
    }
}

bool bomOnSave(TextEncoding encoding, bool sourceHadBom) noexcept
{
    switch (encoding) {
    case TextEncoding::Utf16Le:
    case TextEncoding::Utf16Be:
    case TextEncoding::Utf32Le:
    case TextEncoding::Utf32Be:
        // The BOM is the only thing that makes the endianness explicit.
        return true;
    case TextEncoding::Utf8:
        return sourceHadBom;
    default:
        // Legacy code pages have no BOM; Binary/Unknown never reach a save.
        return false;
    }
}

std::optional<TextEncoding> fromName(QByteArrayView name)
{
    if (name.isEmpty()) {
        return std::nullopt;
    }
    // Normalize: uppercase, drop '-' and '_'.
    QByteArray normalized;
    normalized.reserve(static_cast<qsizetype>(name.size()));
    for (const char c : name) {
        const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
        if (upper == '-' || upper == '_') {
            continue;
        }
        normalized.append(upper);
    }

    static const struct {
        TextEncoding encoding;
        const char* normalized;
    } kMap[] = {
        {TextEncoding::Utf8, "UTF8"},
        {TextEncoding::Utf16Le, "UTF16LE"},
        {TextEncoding::Utf16Be, "UTF16BE"},
        {TextEncoding::Utf32Le, "UTF32LE"},
        {TextEncoding::Utf32Be, "UTF32BE"},
        {TextEncoding::Gb18030, "GB18030"},
        {TextEncoding::Gbk, "GBK"},
        {TextEncoding::Gbk, "GB2312"},  // historical alias, GBK superset in practice
        {TextEncoding::Big5, "BIG5"},
        {TextEncoding::ShiftJis, "SHIFTJIS"},
        {TextEncoding::ShiftJis, "SJIS"},
        {TextEncoding::EucKr, "EUCKR"},
        {TextEncoding::Windows1252, "WINDOWS1252"},
        {TextEncoding::Windows1252, "CP1252"},
        {TextEncoding::Latin1, "ISO88591"},
        {TextEncoding::Latin1, "LATIN1"},
    };
    for (const auto& entry : kMap) {
        if (normalized == entry.normalized) {
            return entry.encoding;
        }
    }
    return std::nullopt;
}

TextEncoding localAnsiEncoding() noexcept
{
#ifdef _WIN32
    // ::GetACP() returns the system's "ANSI" code page identifier.
    //   936   -> GBK (simplified Chinese, zh-CN)
    //   950   -> Big5 (traditional Chinese, zh-TW/HK)
    //   949   -> EUC-KR / CP949 (Korean)
    //   932   -> Shift-JIS (Japanese)
    //   1252  -> Windows-1252 (Western European)
    //   54936 -> GB18030
    //   65001 -> UTF-8 ("Beta: Use Unicode UTF-8 for worldwide language
    //             support" flips the system ANSI page to this; treating it
    //             as Windows-1252 garbled every detected legacy file)
    const UINT cp = ::GetACP();
    switch (cp) {
    case 936:   return TextEncoding::Gbk;
    case 950:   return TextEncoding::Big5;
    case 949:   return TextEncoding::EucKr;
    case 932:   return TextEncoding::ShiftJis;
    case 54936: return TextEncoding::Gb18030;
    case 1252:  return TextEncoding::Windows1252;
    case 65001: return TextEncoding::Utf8;
    default:    return TextEncoding::Windows1252;
    }
#else
    return TextEncoding::Windows1252;
#endif
}

QString localAnsiDisplayName() noexcept
{
    return QStringLiteral("ANSI (%1)")
        .arg(QString::fromLatin1(toName(localAnsiEncoding())));
}

QString encodingDisplayName(TextEncoding encoding, bool reliable)
{
    const QString name = QString::fromLatin1(toName(encoding));
    // "(guessed)" is not decoration: the label is what the log line and the
    // status-bar tooltip show, and it is the only thing that tells the user
    // the text on screen came from guessing rather than from a BOM.
    return reliable ? name : name + QStringLiteral(" (guessed)");
}

QString encodingGuessWarning(
    TextEncoding encoding,
    bool reliable,
    std::size_t invalidUtf8Sequences)
{
    if (reliable) {
        return {};
    }
    if (encoding == TextEncoding::Utf8 && invalidUtf8Sequences > 0) {
        // The text is right except at those spots: say exactly what will be
        // lost, not "the encoding might be wrong" (it is not).
        return QStringLiteral(
                   "Not valid UTF-8: %1 damaged spot(s) are shown as U+FFFD, and "
                   "saving replaces those bytes with it.")
            .arg(invalidUtf8Sequences);
    }
    return QStringLiteral(
               "Not valid UTF-8, so the encoding was guessed (%1). Saving writes "
               "the buffer back in %1 — pick a different encoding in the status "
               "bar if the text looks wrong.")
        .arg(QString::fromLatin1(toName(encoding)));
}

EncodingDetection detectEncoding(QByteArrayView bytes)
{
    EncodingDetection result;

    if (bytes.isEmpty()) {
        result.encoding = TextEncoding::Utf8;
        result.reliable = true;
        return result;
    }

    // -- 1. Byte-order marks (always authoritative) -------------------------
    for (const BomEntry& bom : kBoms) {
        if (bytes.size() >= bom.length
            && QByteArrayView(bytes.constData(), bom.length)
                   == QByteArrayView(bom.bytes, bom.length)) {
            result.encoding = bom.encoding;
            result.hadBom = true;
            result.score = 1000;
            result.reliable = true;
            return result;
        }
    }

    // -- 2. NUL-based structure (UTF-16/32 without BOM, or binary) ----------
    const qsizetype size = bytes.size();
    qsizetype zeros = 0;
    qsizetype controls = 0;  // C0 bytes other than \t \n \r
    for (qsizetype i = 0; i < size; ++i) {
        const char c = bytes[i];
        if (c == '\0') {
            ++zeros;
        } else if (static_cast<unsigned char>(c) < 0x20
                   && c != '\t' && c != '\n' && c != '\r') {
            ++controls;
        }
    }

    if (zeros > 0) {
        if (looksLikeUtf16NoBom(bytes, 2)) {
            result.encoding = TextEncoding::Utf16Le;
            result.score = 700;
            result.reliable = true;
            return result;
        }
        if (looksLikeUtf16NoBom(bytes, 1)) {
            result.encoding = TextEncoding::Utf16Be;
            result.score = 700;
            result.reliable = true;
            return result;
        }
        // NULs without a Unicode alignment pattern: treat as binary.
        result.encoding = TextEncoding::Binary;
        result.score = 0;
        result.reliable = true;
        return result;
    }
    if (size > 0 && static_cast<double>(controls) / static_cast<double>(size) > 0.05) {
        result.encoding = TextEncoding::Binary;
        result.reliable = true;
        return result;
    }

    // -- 3. UTF-8 (with or without multi-byte sequences) --------------------
    std::size_t utf8Errors = 0;
    {
        const std::string_view view(bytes.constData(), static_cast<std::size_t>(size));
        utf8Errors = LightCpp::Core::Text::validateUtf8(view);
        if (utf8Errors == 0) {
            result.encoding = TextEncoding::Utf8;
            result.score = 800;
            result.reliable = true;
            return result;
        }
    }

    // -- 3b. Nearly UTF-8: a few broken sequences in an otherwise UTF-8 file --
    // Strict validation is all-or-nothing, and the fallback below is the local
    // ANSI code page. So one byte cut out of a character — a writer that died
    // mid-save, a file truncated at a byte boundary, a legacy tool appending
    // something — turned an entire UTF-8 source file into mojibake: every
    // Chinese comment and string literal in it decoded through GBK. That is
    // not a wrong guess about a legacy file, it is the wrong failure mode for
    // a file that is 99.99% UTF-8.
    //
    // A handful of bad sequences in a buffer that otherwise decodes cleanly is
    // strong evidence for UTF-8, so say so — but as a *guess*, because the
    // damaged bytes cannot be written back (they have no character). The
    // caller decodes leniently and treats the result as something the user
    // must confirm before it is saved over the original bytes.
    if (utf8Errors <= kMaxBrokenUtf8Sequences
        && utf8Errors * 100 <= static_cast<std::size_t>(size)) {
        result.encoding = TextEncoding::Utf8;
        result.score = 300;
        result.reliable = false;
        result.invalidUtf8Sequences = utf8Errors;
        return result;
    }

    // -- 4. Not Unicode: default to local ANSI code page -------------------
    // The previous implementation tried to score each legacy candidate
    // (GBK, GB18030, Big5, Shift-JIS, EUC-KR) and pick the best by content
    // frequency. That approach was brittle: GBK and EUC-KR byte ranges
    // overlap so heavily that almost any GBK file is also syntactically
    // valid EUC-KR, and Korean text is also syntactically valid GBK — pure
    // scoring could flip either way. In practice, non-Unicode files on a
    // user's machine almost always match the machine's locale, so defaulting
    // to the local ANSI code page is both simpler and more reliable.
    // Users who need to override the choice can do so via the GUI's
    // encoding drop-down (which re-opens the file with the chosen codec).
    result.encoding = localAnsiEncoding();
    result.score = 100;
    result.reliable = false;  // guessed, not verified
    result.invalidUtf8Sequences = utf8Errors;
    return result;
}

}  // namespace LightCpp::Text
