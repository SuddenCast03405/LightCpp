// LightCppText - high-level text file pipeline (implementation).

#include "LightCpp/Text/TextFile.hpp"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "LightCpp/Text/Converter.hpp"

namespace LightCpp::Text {

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

LineEnding detectLineEndings(QByteArrayView bytes) noexcept
{
    // Multi-byte encodings (GBK/UTF-16...) cannot produce a 0x0A or 0x0D0A
    // sequence as part of a character code, so counting raw bytes is safe
    // regardless of the file's encoding: UTF-16 pairs the NUL with the \n
    // but the \n byte still only ever appears as a line feed, and GBK trail
    // bytes never reach 0x0A.
    qsizetype lf = 0;
    qsizetype crlf = 0;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != '\n') {
            continue;
        }
        ++lf;
        if (i > 0 && bytes[i - 1] == '\r') {
            ++crlf;
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
    content.lineEndings = detectLineEndings(data);

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
    content.lineEndings = detectLineEndings(data);

    return TextFileResult::success(std::move(content));
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
