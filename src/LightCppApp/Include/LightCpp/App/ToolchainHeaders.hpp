#pragma once

#include <QString>
#include <QStringList>

namespace LightCpp::App {

/// Which compiler's standard-library and system headers the language server
/// should resolve `#include` directives against.
///
/// clangd falls back to whatever its own clang build was configured for. That
/// is the wrong answer as soon as the IDE compiles with a different compiler:
/// with MSVC selected, `<iostream>` still resolved to MinGW's libstdc++, so the
/// editor underlined code cl.exe compiles without a murmur.
///
/// clangd only takes include paths from a compilation database, and this IDE
/// has no project to generate one from, so LanguageServerClient writes a
/// single-file database (compile_flags.txt) holding exactly these values and
/// points clangd at it with --compile-commands-dir. Constructing an empty
/// instance means "leave clangd alone", which is right for the MinGW
/// toolchains — clangd's own default already is the MSYS2 GCC it was built
/// against.
struct ToolchainHeaders {
    /// clang target triple, e.g. "x86_64-pc-windows-msvc". Empty keeps
    /// clangd's default, which is what the MinGW toolchains want.
    QString target;
    /// Extra compiler flags, e.g. "-fms-compatibility-version=19.30".
    QStringList extraFlags;
    /// System include directories, in search order.
    QStringList includeDirs;

    [[nodiscard]] bool isEmpty() const
    {
        return target.isEmpty() && extraFlags.isEmpty() && includeDirs.isEmpty();
    }

    /// Value comparison, so a caller can tell whether a toolchain switch
    /// actually changes anything before restarting the server over it.
    [[nodiscard]] bool operator==(const ToolchainHeaders& other) const
    {
        return target == other.target && extraFlags == other.extraFlags
               && includeDirs == other.includeDirs;
    }
};

}  // namespace LightCpp::App
