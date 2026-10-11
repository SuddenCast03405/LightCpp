#pragma once

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

/// Whole-word occurrence search over raw text: every [start, end) range
/// where `word` appears with non-identifier characters on both sides.
/// This is what "highlight the same symbol" wants — the word under the
/// caret stands out everywhere it occurs, but `main` inside `mainly` stays
/// quiet.
///
/// Deliberately Qt-free (see the module rules in LightCppCore): the editor
/// passes the document text and gets byte offsets back.
namespace LightCpp::Core::Words {

/// True for [A-Za-z0-9_] — the characters that make up an identifier.
[[nodiscard]] bool isIdentChar(char c) noexcept;

/// Every whole-word occurrence of `word` in `text`, left to right, as
/// [start, end) byte offsets into `text`. Returns at most `maxCount`
/// occurrences; an empty `word` returns nothing. Word boundaries only apply
/// where they exist: an occurrence at the very start or end of the text
/// counts, and any character counts as a boundary there.
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> wordOccurrences(
    std::string_view text, std::string_view word, std::size_t maxCount);

}  // namespace LightCpp::Core::Words
