#pragma once

#include <optional>
#include <string>

namespace LightCpp::Project {

/// One diagnostic extracted from a compiler/build log line.
/// Line/column are 0-based (matching the LSP conventions the IDE uses
/// internally); `file` is the path exactly as the compiler printed it
/// (may be relative to the build's working directory). An empty `file`
/// means the diagnostic has no source location (compiler-driver errors
/// like "clang++: error: invalid value ...").
struct CompilerDiagnostic {
    std::string file;
    int line = 0;      // 0-based; 0 when `file` is empty
    int column = 0;    // 0-based; 0 when the compiler printed no column
    /// LSP severity codes: 1 = Error, 2 = Warning.
    int severity = 1;
    std::string message;
};

/// Parse one line of compiler output into a diagnostic, or nullopt when the
/// line is not an error/warning report (progress lines, "In file included
/// from" context lines, linker chatter, ...).
///
/// Recognised shapes:
///   GNU/Clang:  <file>:<line>:<col>: error: <msg>        (col optional,
///                "fatal error" also matches)
///   MSVC:       <file>(<line>[,<col>]): error [Cxxxx]: <msg>
///               <file>(<line>[,<col>]): warning [Cxxxx]: <msg>
///               ("fatal error" also matches)
///   Driver:     <program>: error: <msg>   (e.g. "clang++: error: ...",
///               "collect2.exe: error: ld returned 1 exit status") — no
///                location, `file` comes back empty.
///
/// Windows drive-letter paths ("E:/x.cpp:3:5: error: ...") are handled: the
/// file part is everything before the *last* "<digits>:<digits>:" run.
[[nodiscard]] std::optional<CompilerDiagnostic>
parseCompilerDiagnostic(const std::string& line);

}  // namespace LightCpp::Project
