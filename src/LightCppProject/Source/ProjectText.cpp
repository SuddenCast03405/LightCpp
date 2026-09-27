#include "LightCpp/Project/ProjectText.hpp"

#include "LightCpp/Text/Converter.hpp"
#include "LightCpp/Text/Encoding.hpp"

namespace LightCpp::Project {
namespace {

using LightCpp::Text::TextEncoding;

}  // namespace

std::optional<QString> decodeProjectText(const QByteArray& bytes, QString* error)
{
    if (bytes.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("the project file is empty");
        }
        return std::nullopt;
    }

    const LightCpp::Text::EncodingDetection detected =
        LightCpp::Text::detectEncoding(bytes);
    QString text;
    if (detected.encoding == TextEncoding::Utf8) {
        text = QString::fromUtf8(bytes);
    } else if (detected.encoding == TextEncoding::Utf16Le
               || detected.encoding == TextEncoding::Utf16Be
               || detected.encoding == TextEncoding::Utf32Le
               || detected.encoding == TextEncoding::Utf32Be
               || detected.encoding == TextEncoding::Gb18030
               || detected.encoding == TextEncoding::Gbk
               || detected.encoding == TextEncoding::Big5
               || detected.encoding == TextEncoding::ShiftJis
               || detected.encoding == TextEncoding::EucKr
               || detected.encoding == TextEncoding::Windows1252
               || detected.encoding == TextEncoding::Latin1) {
        if (auto decoded = LightCpp::Text::decodeToQString(
                bytes, detected.encoding, /*stripBom=*/true)) {
            text = std::move(*decoded);
        } else {
            if (error != nullptr) {
                *error = QStringLiteral("the project file is not valid in the "
                                        "detected encoding");
            }
            return std::nullopt;
        }
    } else {
        if (error != nullptr) {
            *error = QStringLiteral("the project file is not text");
        }
        return std::nullopt;
    }

    // A UTF-8 BOM would otherwise glue itself onto the first tag or key (the
    // UTF-8 path above keeps it — decodeToQString only strips for the other
    // paths).
    if (text.startsWith(QChar(0xFEFF))) {
        text.remove(0, 1);
    }
    return text;
}

}  // namespace LightCpp::Project
