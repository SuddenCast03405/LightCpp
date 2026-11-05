#pragma once

#include <QString>
#include <QStringList>

namespace LightCpp::App::DefaultSettings {

/// What ensureDefaultConfigFiles() did.
struct WriteResult {
    /// Files actually written (absolute paths).
    QStringList written;
    /// Files that already existed and were left untouched.
    QStringList kept;
};

/// Write the starter configuration into `dir` (<exe dir>/config).
///
/// Only missing files are written — a config the user has edited is never
/// regenerated. The content is assembled from what this machine actually
/// has: the MSYS2 UCRT64 tools when present, git, and an MSVC kit when a
/// Visual Studio install is detected. A directory that ends up with no
/// files at all (nothing detected) is left uncreated.
[[nodiscard]] WriteResult ensureDefaultConfigFiles(const QString& dir);

}  // namespace LightCpp::App::DefaultSettings
