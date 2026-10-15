// LightCppText - high-level text file pipeline (implementation).

#include "LightCpp/Text/TextFile.hpp"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

#include "LightCpp/Text/Converter.hpp"

namespace LightCpp::Text {
namespace {

/// How much of a file is inspected for a NUL byte before deciding it is not
/// text. Long enough to catch a binary's header or a UTF-16 file, short
/// enough that the check never dominates the read.
constexpr qsizetype kBinarySniffBytes = 4096;

}  // namespace

const char* toString(TextFileError error) noexcept
{
    switch (error) {
    case TextFileError::NotFound: return "file not found";
    case TextFileError::NotARegularFile: return "path is not a regular file";
    case TextFileError::TooLarge: return "file exceeds the configured size limit";
    case TextFileError::ReadError: return "I/O error while reading the file";
    case TextFileError::BinaryFile: return "file does not appear to contain text";
    case TextFileError::UnsupportedConversion:
        return "detected encoding cannot be converted on this platform";
    case TextFileError::DecodeError: return "file could not be decoded";
    }
    return "unknown error";
}

const char* toString(LineEnding ending) noexcept
{
    switch (ending) {
    case LineEnding::Lf: return "LF";
    case LineEnding::CrLf: return "CRLF";
    case LineEnding::None: break;
    }
    return "";
}

LineEnding detectLineEndings(QByteArrayView bytes, TextEncoding encoding) noexcept
{
    // Pick the code-unit width and byte order. A '\n' byte only ever means a
    // line feed in the single-byte encodings (GBK trail bytes never reach
    // 0x0A), so there the raw byte scan is exact. UTF-16/32 need the unit
    // boundaries: the line feed is a whole unit, and a character like U+0Axx
    // carries a 0x0A byte inside itself that must not be counted.
    int stride = 1;
    bool littleEndian = true;
    switch (encoding) {
    case TextEncoding::Utf16Le: stride = 2; littleEndian = true; break;
    case TextEncoding::Utf16Be: stride = 2; littleEndian = false; break;
    case TextEncoding::Utf32Le: stride = 4; littleEndian = true; break;
    case TextEncoding::Utf32Be: stride = 4; littleEndian = false; break;
    default: break;
    }

    qsizetype lf = 0;
    qsizetype crlf = 0;
    if (stride == 1) {
        for (qsizetype i = 0; i < bytes.size(); ++i) {
            if (bytes[i] != '\n') {
                continue;
            }
            ++lf;
            if (i > 0 && bytes[i - 1] == '\r') {
                ++crlf;
            }
        }
    } else {
        const qsizetype units = bytes.size() / stride;
        const auto codeUnit = [&](qsizetype unit) -> quint32 {
            quint32 value = 0;
            if (littleEndian) {
                for (int k = stride - 1; k >= 0; --k) {
                    value = (value << 8)
                            | static_cast<unsigned char>(bytes[unit * stride + k]);
                }
            } else {
                for (int k = 0; k < stride; ++k) {
                    value = (value << 8)
                            | static_cast<unsigned char>(bytes[unit * stride + k]);
                }
            }
            return value;
        };
        for (qsizetype unit = 0; unit < units; ++unit) {
            if (codeUnit(unit) != 0x0Au) {
                continue;
            }
            ++lf;
            if (unit > 0 && codeUnit(unit - 1) == 0x0Du) {
                ++crlf;
            }
        }
    }
    if (lf == 0) {
        return LineEnding::None;
    }
    // The dominant style wins; a file that genuinely mixes gets whichever
    // has the majority, and an exact tie goes to LF (the least destructive
    // choice — CRLF would add a byte per line).
    return crlf * 2 > lf ? LineEnding::CrLf : LineEnding::Lf;
}

TextFileResult readDetectedTextFile(const QString& path, const TextFileOptions& options)
{
    const QFileInfo info(path);

    if (!info.exists()) {
        return TextFileResult::failure(TextFileError::NotFound);
    }
    if (info.isDir()) {
        return TextFileResult::failure(TextFileError::NotARegularFile);
    }
    if (options.maxBytes > 0 && info.size() > options.maxBytes) {
        return TextFileResult::failure(TextFileError::TooLarge);
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return TextFileResult::failure(TextFileError::ReadError);
    }
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return TextFileResult::failure(TextFileError::ReadError);
    }

    const EncodingDetection detection = detectEncoding(data);
    if (detection.encoding == TextEncoding::Binary) {
        return TextFileResult::failure(TextFileError::BinaryFile);
    }
    if (detection.encoding == TextEncoding::Unknown) {
        return TextFileResult::failure(TextFileError::DecodeError);
    }

    ConvertResult converted = convertToUtf8(data, detection.encoding);
    if (!converted.hasValue()) {
        switch (converted.error()) {
        case ConvertError::PlatformUnsupported:
            return TextFileResult::failure(TextFileError::UnsupportedConversion);
        case ConvertError::UnsupportedEncoding:
        case ConvertError::InvalidInput:
            return TextFileResult::failure(TextFileError::DecodeError);
        }
        return TextFileResult::failure(TextFileError::DecodeError);
    }

    TextFileContent content;
    content.utf8 = converted.takeValue();
    content.encoding = detection.encoding;
    content.hadBom = detection.hadBom;
    content.byteSize = data.size();
    content.detectionScore = detection.score;
    content.detectionReliable = detection.reliable;
    content.lineEndings = detectLineEndings(data, detection.encoding);

    return TextFileResult::success(std::move(content));
}

TextFileResult readTextFileWithEncoding(
    const QString& path,
    TextEncoding encoding,
    const TextFileOptions& options)
{
    const QFileInfo info(path);

    if (!info.exists()) {
        return TextFileResult::failure(TextFileError::NotFound);
    }
    if (info.isDir()) {
        return TextFileResult::failure(TextFileError::NotARegularFile);
    }
    if (options.maxBytes > 0 && info.size() > options.maxBytes) {
        return TextFileResult::failure(TextFileError::TooLarge);
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return TextFileResult::failure(TextFileError::ReadError);
    }
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return TextFileResult::failure(TextFileError::ReadError);
    }

    if (encoding == TextEncoding::Binary) {
        return TextFileResult::failure(TextFileError::BinaryFile);
    }
    if (encoding == TextEncoding::Unknown) {
        return TextFileResult::failure(TextFileError::DecodeError);
    }

    ConvertResult converted = convertToUtf8(data, encoding);
    if (!converted.hasValue()) {
        switch (converted.error()) {
        case ConvertError::PlatformUnsupported:
            return TextFileResult::failure(TextFileError::UnsupportedConversion);
        case ConvertError::UnsupportedEncoding:
        case ConvertError::InvalidInput:
            return TextFileResult::failure(TextFileError::DecodeError);
        }
        return TextFileResult::failure(TextFileError::DecodeError);
    }

    TextFileContent content;
    content.utf8 = converted.takeValue();
    content.encoding = encoding;
    // Re-detect BOM presence for display only (the converter strips it
    // automatically when needed).
    content.hadBom = (data.size() >= 3
                      && static_cast<unsigned char>(data[0]) == 0xEF
                      && static_cast<unsigned char>(data[1]) == 0xBB
                      && static_cast<unsigned char>(data[2]) == 0xBF)
                     || (data.size() >= 2
                         && ((static_cast<unsigned char>(data[0]) == 0xFF
                              && static_cast<unsigned char>(data[1]) == 0xFE)
                             || (static_cast<unsigned char>(data[0]) == 0xFE
                                 && static_cast<unsigned char>(data[1]) == 0xFF)));
    content.byteSize = data.size();
    content.detectionScore = 1000;  // user override is authoritative
    content.detectionReliable = true;
    content.lineEndings = detectLineEndings(data, encoding);

    return TextFileResult::success(std::move(content));
}

std::optional<QString> readSearchableTextFile(const QString& path,
                                              std::int64_t maxBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    if (maxBytes > 0 && bytes.size() > maxBytes) {
        return std::nullopt;
    }
    // Binary guard: a NUL byte in the first block means this is not text. The
    // sniff looks at the bytes already in hand instead of reading a prefix
    // first — a prefix read moves the file position, and readAll() afterwards
    // would then return the file *minus* that prefix.
    const qsizetype sniffLength =
        qMin<qsizetype>(bytes.size(), kBinarySniffBytes);
    if (bytes.first(sniffLength).contains('\0')) {
        return std::nullopt;
    }
    return QString::fromUtf8(bytes);
}

bool isUnderSkippedDirectory(QStringView relativePath)
{
    // Names of the directories a source scan never descends into.
    static const QSet<QString> kSkipped = {
        QStringLiteral(".git"),            QStringLiteral(".svn"),
        QStringLiteral(".hg"),             QStringLiteral(".vs"),
        QStringLiteral("build"),           QStringLiteral("cmake-build-debug"),
        QStringLiteral("cmake-build-release"),
        QStringLiteral(".ccls-cache"),     QStringLiteral(".workbuddy"),
        QStringLiteral("node_modules"),
    };
    // Compare whole path segments, not substrings: `mybuild/` must not match
    // `build`, and the check must see every segment, not just the first one.
    const QString normalised =
        relativePath.toString().replace(QLatin1Char('\\'), QLatin1Char('/'));
    for (const QString& name : kSkipped) {
        const QString segment = name + QLatin1Char('/');
        if (normalised.startsWith(segment)
            || normalised.contains(QLatin1Char('/') + segment)) {
            return true;
        }
    }
    return false;
}

const char* toString(WriteFileError error) noexcept
{
    switch (error) {
    case WriteFileError::OpenFailed:
        return "could not create a temporary file next to the target";
    case WriteFileError::WriteFailed:
        return "the contents could not be written to disk";
    case WriteFileError::CommitFailed:
        return "the new contents could not replace the file";
    }
    return "unknown error";
}

std::optional<WriteFileError> writeFileAtomically(const QString& path,
                                                  const QByteArray& bytes)
{
    // QSaveFile writes beside the target and swaps the file in on commit(),
    // so the target keeps its old contents until the new ones are complete.
    // The temporary must live in the target's own directory for the final
    // rename to stay on one volume (and therefore be atomic).
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return WriteFileError::OpenFailed;
    }
    // write() may satisfy the request in pieces; QIODevice loops internally
    // but still reports short writes, and a short write here means the
    // buffer could not reach the disk driver.
    if (file.write(bytes) != bytes.size()) {
        // Drop the temporary: leaving it behind both litters the user's
        // directory and would make the next save's rename collide.
        file.cancelWriting();
        return WriteFileError::WriteFailed;
    }
    if (!file.commit()) {
        return WriteFileError::CommitFailed;
    }
    return std::nullopt;
}

}  // namespace LightCpp::Text
