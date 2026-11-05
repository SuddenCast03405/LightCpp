#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

/// Bracket matching over raw text: given the index of a bracket character,
/// find the index of its partner. Skips over string literals, character
/// literals and comments, so `foo(")")` does not end the scan at the quote.
///
/// Deliberately Qt-free and stateless (see the module rules in LightCppCore):
/// the editor calls it on cursor moves with the whole document text.
namespace LightCpp::Core::Brackets {

/// True when `c` is one of ( ) [ ] { }.
[[nodiscard]] bool isBracket(char c) noexcept;

/// The index of the bracket matching the one at `bracketPos`, or nullopt
/// when `bracketPos` is not a bracket or the partner is missing. Open
/// brackets search forward, close brackets backward; nesting of the same
/// pair type is counted. The text is scanned as bytes, which is safe for
/// the delimiters: UTF-8 multi-byte sequences never contain ASCII bytes.
[[nodiscard]] std::optional<std::size_t> matchBracket(
    std::string_view text, std::size_t bracketPos);

}  // namespace LightCpp::Core::Brackets
