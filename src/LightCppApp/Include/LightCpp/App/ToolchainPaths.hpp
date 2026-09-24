#pragma once

#include <QString>

/// Paths to the external tools this IDE drives.
///
/// One place rather than one per file: the compiler, the language server and
/// the formatter all come out of the same MSYS2 UCRT64 install, and a second
/// copy of that path is a second thing to keep in step when it moves.
namespace LightCpp::App {

/// Toolchain directory (MSYS2 UCRT64): gcc/g++/clang/clang++/clangd/
/// clang-format plus the runtime DLLs the programs they build need at run
/// time.
inline const QString kMingwBin = QStringLiteral("E:/msys64/ucrt64/bin");

/// The language server used for syntax checking.
inline const QString kClangdPath = QStringLiteral("E:/msys64/ucrt64/bin/clangd.exe");

/// Formatter behind Edit > Format Document.
inline const QString kClangFormatPath =
    QStringLiteral("E:/msys64/ucrt64/bin/clang-format.exe");

/// Git. The MSYS2 copy comes from the same prefix as everything else here;
/// when it is absent the one on PATH is used instead.
inline const QString kGitPath = QStringLiteral("E:/msys64/ucrt64/bin/git.exe");

}  // namespace LightCpp::App
