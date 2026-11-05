#include "LightCpp/Core/BracketMatch.hpp"

#include <array>

namespace LightCpp::Core::Brackets {
namespace {

constexpr std::array<std::pair<char, char>, 3> kPairs{
    std::pair<char, char>{'(', ')'},
    std::pair<char, char>{'[', ']'},
    std::pair<char, char>{'{', '}'},
};

/// Lexical states outside of brackets. String/Char have forward and backward
/// variants because a quote means the opposite thing depending on the scan
/// direction: forward a quote OPENS the literal, backward it CLOSES one.
enum class LexState {
    Code,
    LineComment,
    BlockComment,
    String,
    Char,
    StringBackward,  // entered on a closing quote, exits at the opener
    CharBackward,
};

/// Advance one character from `state` in `direction`. Returns the next state
/// and how many characters to jump (comments consume their opener, literals
/// skip escaped characters).
std::pair<LexState, std::size_t> stepState(std::string_view text,
                                           std::size_t i, LexState state,
                                           int direction)
{
    switch (state) {
    case LexState::Code:
        if (direction > 0) {
            if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/') {
                return {LexState::LineComment, std::size_t{2}};
            }
            if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*') {
                return {LexState::BlockComment, std::size_t{2}};
            }
            if (text[i] == '"') {
                return {LexState::String, std::size_t{1}};
            }
            if (text[i] == '\'') {
                return {LexState::Char, std::size_t{1}};
            }
        } else {
            // Scanning backward, a comment or literal extends upward from
            // here: */ opens a block comment, a quote closes a literal.
            // "//" hides nothing backward of the slash itself, and a bare
            // backslash is just code.
            if (i + 1 < text.size() && text[i] == '*' && text[i + 1] == '/') {
                return {LexState::BlockComment, std::size_t{2}};
            }
            if (text[i] == '"') {
                return {LexState::StringBackward, std::size_t{1}};
            }
            if (text[i] == '\'') {
                return {LexState::CharBackward, std::size_t{1}};
            }
        }
        return {LexState::Code, std::size_t{1}};

    case LexState::LineComment:
        if (text[i] == '\n') {
            return {LexState::Code, std::size_t{1}};
        }
        return {state, std::size_t{1}};

    case LexState::BlockComment:
        if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/') {
            return {LexState::Code, std::size_t{2}};
        }
        return {state, std::size_t{1}};

    case LexState::String:
    case LexState::Char:
        if (text[i] == '\\' && i + 1 < text.size()) {
            return {state, std::size_t{2}};
        }
        if ((state == LexState::String && text[i] == '"')
            || (state == LexState::Char && text[i] == '\'')) {
            return {LexState::Code, std::size_t{1}};
        }
        return {state, std::size_t{1}};

    case LexState::StringBackward:
    case LexState::CharBackward:
        // The literal ends at its opening quote, which is the first quote
        // that is not itself escaped: text[i-1] == '\\' means this quote is
        // content (a preceding escaped backslash \\" is not modelled).
        if ((state == LexState::StringBackward && text[i] == '"')
            || (state == LexState::CharBackward && text[i] == '\'')) {
            if (i == 0 || text[i - 1] != '\\') {
                return {LexState::Code, std::size_t{1}};
            }
        }
        return {state, std::size_t{1}};
    }
    return {LexState::Code, std::size_t{1}};
}

/// Walk from `from` in `direction` until a bracket of `want` is seen in Code
/// state; nested `open`/`close` pairs are counted. Returns nullopt when the
/// scan runs off the text.
std::optional<std::size_t> scan(std::string_view text, std::size_t from,
                                int direction, char want, char open,
                                char close)
{
    LexState state = LexState::Code;
    int depth = 0;
    std::size_t i = from;
    while (i < text.size()) {
        const auto [next, jump] = stepState(text, i, state, direction);
        if (state == LexState::Code) {
            const char c = text[i];
            if (c == want && depth == 0) {
                return i;
            }
            if (c == open) {
                ++depth;
            } else if (c == close && depth > 0) {
                --depth;
            }
        }
        state = next;
        if (direction < 0 && i == 0) {
            break;
        }
        i = static_cast<std::size_t>(
            static_cast<long long>(i) + direction * static_cast<long long>(jump));
    }
    return std::nullopt;
}

}  // namespace

bool isBracket(char c) noexcept
{
    return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}';
}

std::optional<std::size_t> matchBracket(std::string_view text,
                                        std::size_t bracketPos)
{
    if (bracketPos >= text.size()) {
        return std::nullopt;
    }
    for (const auto& [open, close] : kPairs) {
        if (text[bracketPos] == open) {
            return scan(text, bracketPos + 1, +1, close, open, close);
        }
        if (text[bracketPos] == close) {
            if (bracketPos == 0) {
                return std::nullopt;
            }
            return scan(text, bracketPos - 1, -1, open, close, open);
        }
    }
    return std::nullopt;
}

}  // namespace LightCpp::Core::Brackets
