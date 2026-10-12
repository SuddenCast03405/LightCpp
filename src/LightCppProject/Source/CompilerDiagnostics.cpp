// CompilerDiagnostics — turn compiler/build log lines into diagnostics.
//
// Pure string work: no Qt, no I/O. The App layer feeds decoded log lines in
// one by one and keeps whatever comes back for the Problems (Compiler) list
// and the editor's squiggle overlay.

#include "LightCpp/Project/CompilerDiagnostics.hpp"

#include <cctype>

namespace LightCpp::Project {

namespace {

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

/// Read a run of decimal digits starting at `pos`; advances `pos` past them.
/// Returns -1 when there are no digits at `pos` at all.
int readNumber(const std::string& s, size_t& pos)
{
    if (pos >= s.size() || !isDigit(s[pos])) {
        return -1;
    }
    int value = 0;
    while (pos < s.size() && isDigit(s[pos])) {
        value = value * 10 + (s[pos] - '0');
        ++pos;
    }
    return value;
}

/// Skip spaces (and tabs) starting at `pos`.
void skipSpaces(const std::string& s, size_t& pos)
{
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
        ++pos;
    }
}

/// Match one of the severity words at `pos` (after optional "fatal ").
/// Returns the LSP severity code (1 = error, 2 = warning) or 0 on no match.
///
/// The localized MSVC words (错误/警告, 致命错误) are matched on their UTF-8
/// bytes: the App layer decodes console output to QString and hands lines
/// over as UTF-8 std::string, so the byte sequences are stable regardless
/// of the compiler's code page.
int readSeverity(const std::string& s, size_t& pos)
{
    static const char* kFatal = "fatal error";
    static const char* kError = "error";
    static const char* kWarning = "warning";
    // Localized MSVC: "致命错误 Cxxxx:", "错误 Cxxxx:", "警告 Cxxxx:".
    static const char* kFatalCn = "\xE8\x87\xB4\xE5\x91\xBD\xE9\x94\x99\xE8\xAF\xAF";   // 致命错误
    static const char* kErrorCn = "\xE9\x94\x99\xE8\xAF\xAF";                           // 错误
    static const char* kWarningCn = "\xE8\xAD\xA6\xE5\x91\x8A";                         // 警告

    const auto startsWith = [&s, &pos](const char* word) {
        const size_t len = std::char_traits<char>::length(word);
        if (s.compare(pos, len, word) != 0) {
            return false;
        }
        // The word must end here (space, colon or end of line), so
        // "errors" does not come back as "error" — and the multi-byte
        // words do not splice into a longer run of bytes.
        const size_t after = pos + len;
        if (after < s.size() && s[after] != ' ' && s[after] != ':' && s[after] != '\t') {
            return false;
        }
        pos = after;
        return true;
    };

    // Save the position: callers expect `pos` untouched on no match.
    const size_t start = pos;
    if (startsWith(kFatal)) {
        return 1;
    }
    pos = start;
    if (startsWith(kFatalCn)) {
        return 1;
    }
    pos = start;
    if (startsWith(kError)) {
        return 1;
    }
    pos = start;
    if (startsWith(kErrorCn)) {
        return 1;
    }
    pos = start;
    if (startsWith(kWarning)) {
        return 2;
    }
    pos = start;
    if (startsWith(kWarningCn)) {
        return 2;
    }
    pos = start;
    return 0;
}

/// Does `s` look like a compiler driver name ("clang++", "g++",
/// "collect2.exe", "cc1plus")? Used to keep driver-level errors from
/// swallowing source-file diagnostics. A path with separators is not a
/// bare driver name.
bool looksLikeDriverName(const std::string& s)
{
    if (s.empty() || s.find('/') != std::string::npos
        || s.find('\\') != std::string::npos) {
        return false;
    }
    for (const char c : s) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c))
                        || c == '+' || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::optional<CompilerDiagnostic> makeLocated(const std::string& file, int line,
                                              int column, int severity,
                                              const std::string& message)
{
    CompilerDiagnostic d;
    d.file = file;
    d.line = line > 0 ? line - 1 : 0;    // store 0-based
    d.column = column > 0 ? column - 1 : 0;
    d.severity = severity;
    d.message = message;
    return d;
}

/// A file part may be anything the compiler printed, but it must not be
/// empty and must not itself be just spaces. Drive letters are fine.
bool looksLikeFileName(const std::string& s)
{
    return !s.empty() && s.find_first_not_of(" \t") != std::string::npos;
}

/// GNU/Clang style: <file>:<line>:<col>: severity: <msg>  (col optional).
/// Candidates for the colon that ends the file part are scanned LEFT to
/// RIGHT, taking the first one followed by "<digits>:[<digits>:] <severity>:".
/// Scanning from the left keeps Windows drive letters ("E:/x.cpp") intact —
/// the drive colon is never followed by digits — and anchors the outermost
/// digits run, so "file.cpp:3:5: error:" binds file.cpp to line 3, col 5
/// rather than mis-anchoring on the inner colon.
std::optional<CompilerDiagnostic> parseGnuStyle(const std::string& line)
{
    const size_t size = line.size();
    for (size_t i = 0; i + 1 < size; ++i) {
        if (line[i] != ':') {
            continue;
        }
        size_t pos = i + 1;
        const int lineNo = readNumber(line, pos);
        if (lineNo <= 0) {
            continue;
        }
        if (pos >= line.size() || line[pos] != ':') {
            continue;
        }
        ++pos;
        int columnNo = 0;
        const int maybeColumn = readNumber(line, pos);
        if (maybeColumn > 0 && pos < line.size() && line[pos] == ':') {
            columnNo = maybeColumn;
            ++pos;
        }
        skipSpaces(line, pos);
        const int severity = readSeverity(line, pos);
        if (severity == 0) {
            continue;
        }
        if (pos >= line.size() || line[pos] != ':') {
            continue;
        }
        size_t msgPos = pos + 1;
        skipSpaces(line, msgPos);
        if (msgPos >= line.size()) {
            continue;  // severity with no message — not a report line
        }
        const std::string file = line.substr(0, i);
        if (!looksLikeFileName(file)) {
            continue;
        }
        return makeLocated(file, lineNo, columnNo, severity,
                           line.substr(msgPos));
    }
    return std::nullopt;
}


/// MSVC style: <file>(<line>[,<col>]): severity [Cxxxx]: <msg>.
///
/// The location parenthesis is scanned LEFT to RIGHT, taking the first one
/// whose contents parse as "<digits>[,<digits>]": taking the LAST '(' used
/// to derail as soon as the message itself contained a parenthesis
/// ("error: 括号不匹配((" or an English "unmatched '('"), which every
/// localized-vs-error line with a '(' in its text did.
std::optional<CompilerDiagnostic> parseMsvcStyle(const std::string& line)
{
    for (size_t open = line.find('('); open != std::string::npos;
         open = line.find('(', open + 1)) {
        size_t pos = open + 1;
        const int lineNo = readNumber(line, pos);
        if (lineNo <= 0) {
            continue;
        }
        int columnNo = 0;
        if (pos < line.size() && line[pos] == ',') {
            ++pos;
            columnNo = readNumber(line, pos);
            if (columnNo <= 0) {
                continue;
            }
        }
        if (pos >= line.size() || line[pos] != ')') {
            continue;
        }
        ++pos;
        if (pos >= line.size() || line[pos] != ':') {
            continue;
        }
        ++pos;
        skipSpaces(line, pos);
        const int severity = readSeverity(line, pos);
        if (severity == 0) {
            continue;
        }
        // Optional diagnostic code ("C2065" or the localized shape), then
        // the ':' before the message.
        skipSpaces(line, pos);
        size_t codeEnd = pos;
        while (codeEnd < line.size() && line[codeEnd] != ':'
               && line[codeEnd] != ' ') {
            ++codeEnd;
        }
        if (codeEnd < line.size() && line[codeEnd] == ':') {
            // "error: msg" — the code run was empty; keep pos before the colon.
            pos = codeEnd;
        } else if (codeEnd > pos && codeEnd < line.size()
                   && line[codeEnd] == ' ') {
            // A code word followed by ':' or more spaces — re-scan for ':'.
            size_t scan = codeEnd;
            skipSpaces(line, scan);
            if (scan < line.size() && line[scan] == ':') {
                pos = scan;
            } else {
                continue;  // "warning something else" — not a report
            }
        } else {
            continue;
        }
        size_t msgPos = pos + 1;
        skipSpaces(line, msgPos);
        if (msgPos >= line.size()) {
            continue;
        }
        return makeLocated(line.substr(0, open), lineNo, columnNo, severity,
                           line.substr(msgPos));
    }
    return std::nullopt;
}

/// Driver level: <program>: error: <msg> (no source location).
std::optional<CompilerDiagnostic> parseDriverStyle(const std::string& line)
{
    const size_t sep = line.find(": ");
    if (sep == std::string::npos) {
        return std::nullopt;
    }
    const std::string program = line.substr(0, sep);
    if (!looksLikeDriverName(program)) {
        return std::nullopt;
    }
    size_t pos = sep + 2;
    const int severity = readSeverity(line, pos);
    if (severity == 0) {
        return std::nullopt;
    }
    if (pos >= line.size() || line[pos] != ':') {
        return std::nullopt;
    }
    size_t msgPos = pos + 1;
    skipSpaces(line, msgPos);
    if (msgPos >= line.size()) {
        return std::nullopt;
    }
    CompilerDiagnostic d;
    d.file.clear();
    d.line = 0;
    d.column = 0;
    d.severity = severity;
    d.message = line.substr(msgPos);
    return d;
}

}  // namespace

std::optional<CompilerDiagnostic>
parseCompilerDiagnostic(const std::string& line)
{
    if (line.empty()) {
        return std::nullopt;
    }
    if (auto d = parseGnuStyle(line)) {
        return d;
    }
    if (auto d = parseMsvcStyle(line)) {
        return d;
    }
    return parseDriverStyle(line);
}

}  // namespace LightCpp::Project
