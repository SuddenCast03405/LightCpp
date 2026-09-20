// LightCppText - high-level text file pipeline (implementation).

#include "LightCpp/Text/TextFile.hpp"

#include <QFile>
#include <QFileInfo>

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

    return TextFileResult::success(std::move(content));
}

}  // namespace LightCpp::Text
