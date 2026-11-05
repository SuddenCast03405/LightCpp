#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

/// Build configurations — the Debug/Release choice in the toolbar.
///
/// It is a plain value type with no editor state so that the flag tables can
/// be tested on their own; the window only has to hold one of these.
namespace LightCpp::Project {

enum class BuildConfiguration {
    Debug,
    Release,
};

/// Which compiler family the flags are aimed at. The two differ enough
/// (optimisation, NDEBUG, debug info) that a single table would be wrong for
/// one of them.
enum class CompilerFamily {
    Gnu,   // gcc / g++ / clang / clang++
    Msvc,  // cl.exe
};

/// Canonical spelling, capitalised — also exactly what CMake wants for
/// CMAKE_BUILD_TYPE, so the same string serves the toolbar and the cache.
[[nodiscard]] QString toString(BuildConfiguration config);

/// Parses a canonical name. Anything else (including an empty string) yields
/// nullopt, so a bad value from settings degrades to the caller's default
/// rather than quietly becoming Debug.
[[nodiscard]] std::optional<BuildConfiguration> buildConfigurationFromName(
    QStringView name);

/// The flags one configuration contributes to a compile and link command.
///
/// Debug deliberately carries no debug database for MSVC: /Zi would drop a
/// multi-megabyte .pdb next to the user's source, and this IDE's build path is
/// "compile it and run it", not "attach a debugger". Debug only turns
/// optimisation off.
[[nodiscard]] QStringList flagsFor(BuildConfiguration config, CompilerFamily family);

}  // namespace LightCpp::Project
