#include "LightCpp/Core/WordOccurrences.hpp"

#include <cctype>

namespace LightCpp::Core::Words {

bool isIdentChar(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::vector<std::pair<std::size_t, std::size_t>> wordOccurrences(
    std::string_view text, std::string_view word, std::size_t maxCount)
{
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (word.empty() || maxCount == 0) {
        return out;
    }

    std::size_t from = 0;
    while (out.size() < maxCount) {
        const std::size_t at = text.find(word, from);
        if (at == std::string_view::npos) {
            break;
        }
        const std::size_t end = at + word.size();
        // Whole word: the neighbours must not be identifier characters. The
        // edges of the text count as boundaries.
        const bool leftOk = at == 0 || !isIdentChar(text[at - 1]);
        const bool rightOk = end == text.size() || !isIdentChar(text[end]);
        if (leftOk && rightOk) {
            out.emplace_back(at, end);
        }
        from = at + 1;
    }
    return out;
}

}  // namespace LightCpp::Core::Words
