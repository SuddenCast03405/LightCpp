#pragma once

#include <QByteArray>
#include <optional>
#include <QString>

namespace LightCpp::Project {

/// Decode pre-Unicode project descriptor text (.dsp, .vcproj, ...) into a
/// QString.
///
/// Visual Studio before VS2012 wrote project files in the machine's ANSI code
/// page (GBK on zh-CN); VS2005-2008 .vcproj files declare `encoding="gb2312"`
/// while their content may be plain ANSI. Decoding such bytes as UTF-8
/// silently replaces every non-ASCII character with U+FFFD and produces
/// output paths no tool can open. Detection (BOM -> UTF-8 validation ->
/// legacy codecs) is exactly what the editor uses for source files, so the
/// project file and the editor always agree about what the bytes mean.
///
/// Returns nullopt with `error` filled when the bytes are not text in any
/// supported encoding.
[[nodiscard]] std::optional<QString> decodeProjectText(
    const QByteArray& bytes, QString* error = nullptr);

}  // namespace LightCpp::Project
