#pragma once

#include <compare>
#include <cstddef>
#include <string>
#include <vector>

/// Code folding: which lines can be collapsed, computed from the text alone.
///
/// Deliberately Qt-free and free of any editor state (see the module rules in
/// LightCppCore): the analysis is pure text work, and keeping it here means the
/// awkward cases — braces inside comments, string literals and raw strings —
/// can be tested directly instead of through a widget.
namespace LightCpp::Core::Fold {

enum class FoldKind {
    Brace,        // a { ... } block
    Conditional,  // #if / #ifdef / #ifndef ... #endif
    Region,       // #pragma region ... #pragma endregion
};

/// One collapsible range. Both ends are 0-based line indices and are the
/// lines the delimiters sit on — `startLine` is the line with the `{`, so a
/// folded region normally hides [startLine + 1, endLine] and leaves the
/// opening line visible as the header.
struct FoldRegion {
    std::size_t startLine = 0;
    std::size_t endLine = 0;
    FoldKind kind = FoldKind::Brace;

    [[nodiscard]] auto operator<=>(const FoldRegion&) const = default;
};

/// Every foldable range in `lines`, sorted by start line; when two regions
/// begin on the same line the outer one comes first.
///
/// Only ranges spanning at least two lines are reported: `struct A { int x; };`
/// fits on one line and there is nothing to hide.
std::vector<FoldRegion> computeFoldRegions(const std::vector<std::string>& lines);

}  // namespace LightCpp::Core::Fold
