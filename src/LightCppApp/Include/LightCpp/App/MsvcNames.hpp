#pragma once

#include <QString>

namespace LightCpp::App::MsvcNames {

/// The name a user recognises for the Visual Studio behind `score`.
///
/// `score` is the scale MsvcToolchain shares across its sources: major*1000 +
/// minor, so vswhere's "17.10.…" becomes 17010. That number is the installer's
/// own version, and printing it back — "Visual Studio 17.10" — is what R2-57
/// reported: there is no such product. The year is the name, keeping the
/// installer's number beside it, because two installs of the same year
/// (17.0 and 17.10) are otherwise indistinguishable in the picker:
///
///   17010 -> "Visual Studio 2022 (17.10)"
///   17000 -> "Visual Studio 2022"
///    6000 -> "Visual Studio 6.0"      (no year exists for that one)
///       0 -> "Visual Studio"          (nothing was read)
[[nodiscard]] QString productName(int score);

/// Where 32-bit programs live, from what the system says — in this order:
/// the environment variable, the registry, then the system drive.
///
/// The fixed string this replaces was `C:/Program Files (x86)`, which is a
/// guess in two separate ways: Windows need not be on C:, and 32-bit programs
/// need not be next to Windows. On a machine with neither the environment
/// variable nor a C: drive, the guess turned "VS is installed on D:" into
/// "No Visual Studio installation was found".
///
/// Returns empty when nothing is known — the caller then probes nothing. A
/// last-resort guess is deliberately absent: it cannot succeed where the three
/// answers above failed, and it only makes a wrong answer look considered.
[[nodiscard]] QString programFilesX86From(const QString& envValue,
                                          const QString& registryValue,
                                          const QString& systemDrive);

}  // namespace LightCpp::App::MsvcNames
