#include "LightCpp/Core/FoldModel.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace LightCpp::Core::Fold {
namespace {

constexpr std::size_t kMaxRawDelimiter = 16;

/// Where the lexer is between two characters. Block comments and raw strings
/// are the only constructs that can still be open when a line ends.
enum class LexState {
    Normal,
    LineComment,
    BlockComment,
    String,
    Char,
    RawString,
};

bool isBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

char lower(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool isIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// Reads one identifier starting at or after `from`, lower-cased.
/// `next` is left just past it; an empty result means there was none.
std::string readWord(std::string_view text, std::size_t from, std::size_t& next)
{
    std::size_t i = from;
    while (i < text.size() && isBlank(text[i])) {
        ++i;
    }
    const std::size_t start = i;
    while (i < text.size() && isIdentChar(text[i])) {
        ++i;
    }
    next = i;
    std::string word;
    word.reserve(i - start);
    for (std::size_t k = start; k < i; ++k) {
        word.push_back(lower(text[k]));
    }
    return word;
}

/// The preprocessor directive on a line that starts with '#', normalised to
/// lower case. `#pragma` keeps the word that follows it, so callers see "if",
/// "endif", "pragma region", "pragma endregion"; `#region` is accepted as the
/// Dev-C++/MSVC spelling of the same thing. Returns empty when there is no
/// directive.
std::string directiveOf(std::string_view rest)
{
    std::size_t next = 0;
    const std::string first = readWord(rest, 0, next);
    if (first.empty() || first == "region" || first == "endregion") {
        return first;
    }
    if (first != "pragma") {
        return first;
    }
    // "#pragma region" / "#pragma endregion" are two words; any other #pragma
    // ("once", "pack", …) is not a fold delimiter.
    const std::string second = readWord(rest, next, next);
    if (second.empty()) {
        return first;
    }
    return first + ' ' + second;
}

/// Consumes `R"delim(` when the quote at `quote` opens a raw string: on
/// success `delim` holds the delimiter and `openParen` the index of the '(',
/// so the caller can continue scanning from just after the delimiter.
bool enterRawString(const std::string& text, std::size_t quote,
                    std::size_t& openParen, std::string& delim)
{
    std::size_t p = quote + 1;
    std::string candidate;
    while (p < text.size() && text[p] != '(' && text[p] != ')' && text[p] != '\\'
           && text[p] != '"' && !isBlank(text[p])
           && candidate.size() < kMaxRawDelimiter) {
        candidate.push_back(text[p]);
        ++p;
    }
    if (p >= text.size() || text[p] != '(') {
        return false;
    }
    delim = candidate;
    openParen = p;
    return true;
}

}  // namespace

std::vector<FoldRegion> computeFoldRegions(const std::vector<std::string>& lines)
{
    std::vector<FoldRegion> regions;
    std::vector<std::size_t> braceStack;      // lines carrying an open '{'
    std::vector<std::size_t> conditionalStack;  // lines carrying an open #if
    std::vector<std::size_t> regionStack;       // lines carrying #pragma region

    LexState state = LexState::Normal;
    std::string rawDelim;

    for (std::size_t line = 0; line < lines.size(); ++line) {
        const std::string& text = lines[line];

        // A directive is recognised only where a directive can actually be —
        // a line starting with '#' that the lexer reaches in normal state.
        // Inside a block comment "  #endif" is just comment text.
        bool preprocessorLine = false;
        std::string directive;
        if (state == LexState::Normal) {
            std::size_t k = 0;
            while (k < text.size() && isBlank(text[k])) {
                ++k;
            }
            if (k < text.size() && text[k] == '#') {
                preprocessorLine = true;
                directive = directiveOf(std::string_view(text).substr(k + 1));
            }
        }

        // Directives are whole lines, so they are resolved before the scanner
        // walk rather than inside it.
        if (!directive.empty()) {
            if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
                conditionalStack.push_back(line);
            } else if (directive == "endif") {
                if (!conditionalStack.empty()) {
                    const std::size_t start = conditionalStack.back();
                    conditionalStack.pop_back();
                    if (line > start) {
                        regions.push_back({start, line, FoldKind::Conditional});
                    }
                }
            } else if (directive == "pragma region" || directive == "region") {
                regionStack.push_back(line);
            } else if (directive == "pragma endregion" || directive == "endregion") {
                if (!regionStack.empty()) {
                    const std::size_t start = regionStack.back();
                    regionStack.pop_back();
                    if (line > start) {
                        regions.push_back({start, line, FoldKind::Region});
                    }
                }
            }
        }

        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            switch (state) {
            case LexState::Normal:
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                    state = LexState::LineComment;
                    ++i;
                } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
                    state = LexState::BlockComment;
                    ++i;
                } else if (c == '"') {
                    // A quote preceded by 'R' opens a raw string (this covers
                    // R"…", u8R"…", LR"…" and friends in one check).
                    std::size_t openParen = 0;
                    if (i > 0 && text[i - 1] == 'R'
                        && enterRawString(text, i, openParen, rawDelim)) {
                        state = LexState::RawString;
                        i = openParen;
                    } else {
                        state = LexState::String;
                    }
                } else if (c == '\'') {
                    state = LexState::Char;
                } else if (!preprocessorLine && c == '{') {
                    braceStack.push_back(line);
                } else if (!preprocessorLine && c == '}') {
                    if (!braceStack.empty()) {
                        const std::size_t start = braceStack.back();
                        braceStack.pop_back();
                        if (line > start) {
                            regions.push_back({start, line, FoldKind::Brace});
                        }
                    }
                }
                break;
            case LexState::LineComment:
                break;  // nothing to look for; the state ends with the line
            case LexState::BlockComment:
                if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') {
                    state = LexState::Normal;
                    ++i;
                }
                break;
            case LexState::String:
                if (c == '\\') {
                    ++i;  // an escaped quote does not close the literal
                } else if (c == '"') {
                    state = LexState::Normal;
                }
                break;
            case LexState::Char:
                if (c == '\\') {
                    ++i;
                } else if (c == '\'') {
                    state = LexState::Normal;
                }
                break;
            case LexState::RawString:
                if (c == ')' && text.compare(i + 1, rawDelim.size(), rawDelim) == 0) {
                    const std::size_t after = i + 1 + rawDelim.size();
                    if (after < text.size() && text[after] == '"') {
                        state = LexState::Normal;
                        i = after;
                    }
                }
                break;
            }
        }

        // Only a trailing backslash continues a string or character literal
        // onto the next line; an unterminated literal is treated as closed so
        // one stray quote cannot swallow the rest of the file.
        if (state == LexState::LineComment) {
            state = LexState::Normal;
        } else if (state == LexState::String || state == LexState::Char) {
            if (text.empty() || text.back() != '\\') {
                state = LexState::Normal;
            }
        }
    }

    // Braces are reported when their '}' is met, so inner blocks come first.
    // Sorting by position makes the order stable, and for the same start line
    // the wider region wins — that is what an "fold this block" click wants.
    std::sort(regions.begin(), regions.end(),
              [](const FoldRegion& a, const FoldRegion& b) {
                  if (a.startLine != b.startLine) {
                      return a.startLine < b.startLine;
                  }
                  return a.endLine > b.endLine;
              });
    return regions;
}

}  // namespace LightCpp::Core::Fold
