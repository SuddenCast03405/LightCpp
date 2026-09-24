// FoldModelTests — code-folding analysis.
//
// The interesting cases are all about what must NOT be seen as a delimiter:
// braces and conditionals hidden inside comments, string literals and raw
// strings. Each of those has a paired positive case so that "no region"
// cannot pass merely because the analysis found nothing at all.

#include "LightCpp/Core/FoldModel.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace {

using LightCpp::Core::Fold::computeFoldRegions;
using LightCpp::Core::Fold::FoldKind;
using LightCpp::Core::Fold::FoldRegion;

std::vector<std::string> lines(std::initializer_list<const char*> text)
{
    std::vector<std::string> out;
    out.reserve(text.size());
    for (const char* line : text) {
        out.emplace_back(line);
    }
    return out;
}

FoldRegion brace(std::size_t start, std::size_t end)
{
    return {start, end, FoldKind::Brace};
}

FoldRegion conditional(std::size_t start, std::size_t end)
{
    return {start, end, FoldKind::Conditional};
}

FoldRegion region(std::size_t start, std::size_t end)
{
    return {start, end, FoldKind::Region};
}

}  // namespace

TEST(FoldModel, EmptyAndBlankInputHasNothingToFold)
{
    EXPECT_TRUE(computeFoldRegions({}).empty());
    EXPECT_TRUE(computeFoldRegions(lines({"", "   ", "\t"})).empty());
}

TEST(FoldModel, BracesOnOneLineAreNotFoldable)
{
    // Nothing would be hidden, so offering a fold marker would be a lie.
    EXPECT_TRUE(computeFoldRegions(lines({"struct A { int x; };"})).empty());
    EXPECT_TRUE(computeFoldRegions(lines({"{ }"})).empty());
}

TEST(FoldModel, FunctionBodyFoldsFromTheBraceLine)
{
    const auto found = computeFoldRegions(lines({
        "void f()",
        "{",
        "    return;",
        "}",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({brace(1, 3)}));
}

TEST(FoldModel, OpeningBraceOnTheSignatureLineFolds)
{
    const auto found = computeFoldRegions(lines({
        "void f() {",
        "    return;",
        "}",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({brace(0, 2)}));
}

TEST(FoldModel, NestedBlocksAreListedOutermostFirst)
{
    const auto found = computeFoldRegions(lines({
        "void g() {",
        "    if (x) {",
        "        y();",
        "    }",
        "}",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({brace(0, 4), brace(1, 3)}));
}

TEST(FoldModel, BraceInsideAStringIsIgnored)
{
    // The "}" in the literal must not close the block opened on line 0, and
    // the "{" must not open one either.
    EXPECT_EQ(computeFoldRegions(lines({
                  "const char* s = \"{\";",
              })),
              std::vector<FoldRegion>{});

    EXPECT_EQ(computeFoldRegions(lines({
                  "void f() {",
                  "    const char* s = \"}\";",
                  "}",
              })),
              std::vector<FoldRegion>({brace(0, 2)}));
}

TEST(FoldModel, EscapedQuoteDoesNotEndAString)
{
    EXPECT_TRUE(computeFoldRegions(lines({
        "const char* s = \"\\\"{\";",
        "}",
    })).empty());
}

TEST(FoldModel, BracesInsideACharacterLiteralAreIgnored)
{
    EXPECT_TRUE(computeFoldRegions(lines({
        "char c = '{';",
        "char d = '}';",
    })).empty());
}

TEST(FoldModel, BracesInsideALineCommentAreIgnored)
{
    const auto found = computeFoldRegions(lines({
        "// void f() {",
        "void g() {",
        "}",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({brace(1, 2)}));
}

TEST(FoldModel, BracesInsideABlockCommentAreIgnored)
{
    // Without comment handling the "}" on line 1 would close the block from
    // line 0, and line 2 would close nothing.
    const auto found = computeFoldRegions(lines({
        "void f() {",
        "    /* } */",
        "}",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({brace(0, 2)}));
}

TEST(FoldModel, MultiLineBlockCommentKeepsBracesHiddenAcrossLines)
{
    const auto found = computeFoldRegions(lines({
        "void f() {",
        "    /*",
        "    }",
        "    */",
        "}",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({brace(0, 4)}));
}

TEST(FoldModel, BracesInsideARawStringAreIgnored)
{
    EXPECT_TRUE(computeFoldRegions(lines({
        "const char* s = R\"({",
        "})\";",
    })).empty());
}

TEST(FoldModel, RawStringWithACustomDelimiter)
{
    // The delimiter matters: )" alone does not end R"x( … )x".
    EXPECT_TRUE(computeFoldRegions(lines({
        "const char* s = R\"x({",
        "})\";",
        "})\";",
    })).empty());
}

TEST(FoldModel, BracesOnAPreprocessorLineAreNotABlock)
{
    EXPECT_EQ(computeFoldRegions(lines({
                  "#define BEGIN {",
                  "}",
              })),
              std::vector<FoldRegion>{});
    EXPECT_EQ(computeFoldRegions(lines({
                  "#define END }",
                  "void f() {",
                  "}",
              })),
              std::vector<FoldRegion>({brace(1, 2)}));
}

TEST(FoldModel, UnterminatedBracesDoNotInventRegions)
{
    const auto found = computeFoldRegions(lines({
        "void f() {",
        "    if (x) {",
        "}",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({brace(1, 2)}));
}

TEST(FoldModel, ConditionalBlockFoldsFromIfToEndif)
{
    const auto found = computeFoldRegions(lines({
        "#include <vector>",
        "#if defined(A)",
        "int x;",
        "#endif",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({conditional(1, 3)}));
}

TEST(FoldModel, ElseAndElifDoNotSplitTheConditional)
{
    const auto found = computeFoldRegions(lines({
        "#if A",
        "int a;",
        "#else",
        "int b;",
        "#endif",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({conditional(0, 4)}));
}

TEST(FoldModel, NestedConditionalsFoldIndependently)
{
    const auto found = computeFoldRegions(lines({
        "#if A",
        "#ifdef B",
        "int x;",
        "#endif",
        "#endif",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({conditional(0, 4), conditional(1, 3)}));
}

TEST(FoldModel, IndentedDirectivesAreStillDirectives)
{
    const auto found = computeFoldRegions(lines({
        "    #ifndef GUARD",
        "        int x;",
        "    #endif",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({conditional(0, 2)}));
}

TEST(FoldModel, EndifWithoutAnIfIsIgnored)
{
    EXPECT_TRUE(computeFoldRegions(lines({"#endif"})).empty());
}

TEST(FoldModel, DirectiveInsideABlockCommentIsNotADirective)
{
    // The "  #endif" on line 1 is comment text: it must not close anything,
    // and line 3 is not a foldable conditional on its own.
    EXPECT_TRUE(computeFoldRegions(lines({
        "/*",
        "   #endif",
        "*/",
    })).empty());
}

TEST(FoldModel, PragmaRegionFolds)
{
    const auto found = computeFoldRegions(lines({
        "#pragma region Setup",
        "int x;",
        "#pragma endregion",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({region(0, 2)}));
}

TEST(FoldModel, HashRegionSpellingIsAccepted)
{
    // Dev-C++/MSVC also accept the short form.
    const auto found = computeFoldRegions(lines({
        "#region Setup",
        "int x;",
        "#endregion",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({region(0, 2)}));
}

TEST(FoldModel, OtherPragmasAreNotRegions)
{
    EXPECT_TRUE(computeFoldRegions(lines({
        "#pragma once",
        "#pragma pack(push, 1)",
    })).empty());
}

TEST(FoldModel, RealisticTranslationUnit)
{
    const auto found = computeFoldRegions(lines({
        "#include <string>",
        "",
        "namespace demo {",
        "",
        "#ifdef _WIN32",
        "const char* kSep = \"\\\\\";",
        "#endif",
        "",
        "struct Thing {",
        "    int id = 0;",
        "};",
        "",
        "int run() {",
        "    Thing t;",
        "    if (t.id) {",
        "        return 1;",
        "    }",
        "    return 0;",
        "}",
        "",
        "}  // namespace demo",
    }));
    EXPECT_EQ(found, std::vector<FoldRegion>({
        brace(2, 20),
        conditional(4, 6),
        brace(8, 10),
        brace(12, 18),
        brace(14, 16),
    }));
}
