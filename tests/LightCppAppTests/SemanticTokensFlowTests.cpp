// SemanticTokensFlowTests — the parts of the semantic-highlighting pipeline
// that are pure logic, exercised without a server.
//
// The pipeline is
//
//   initialize answer  -> SemanticLegend            (what the indices mean)
//   semanticTokens/full -> 5-tuple delta stream     (decodeSemanticTokens)
//   type index          -> TokenRole                (roleFor)
//   role                -> colour                   (CodeEditor)
//
// A real clangd is not available here, and would not be wanted: the failures
// this guards against are all in the mapping, not in the transport. The three
// that used to happen (and that a round-trip test through a GUI editor would
// have hidden behind "it looked wrong") are:
//
//   * the legend taken from the client's own request instead of the server's
//     answer, which paints the wrong role on every token when the two differ;
//   * a delta decoded against the wrong origin, which shifts a whole line;
//   * a role name the client does not know, silently leaving tokens to the
//     lexical highlighter even though the server answered.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "LightCpp/App/LanguageServerClient.hpp"
#include "LightCpp/Workbench/LspModel.hpp"

namespace {

using LightCpp::Workbench::Lsp::SemanticLegend;
using LightCpp::Workbench::Lsp::SemanticToken;
using LightCpp::Workbench::Lsp::TokenRole;

QJsonObject parseObject(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

SemanticLegend legendFrom(const char* json)
{
    return LightCpp::Workbench::Lsp::parseLegend(parseObject(json));
}

}  // namespace

// ---------------------------------------------------------------------------
// The legend comes from the server, not from what we asked for
// ---------------------------------------------------------------------------

TEST(SemanticLegend, TheServersOwnOrderIsWhatTheIndicesReferTo)
{
    // clangd collapses several LSP types into one and may reorder them; the
    // index in a token stream is a position in *this* list.
    const SemanticLegend legend = legendFrom(R"({
        "capabilities": {
            "semanticTokensProvider": {
                "legend": {
                    "tokenTypes": ["variable", "class", "function"],
                    "tokenModifiers": ["declaration", "readonly"]
                }
            }
        }
    })");
    ASSERT_EQ(legend.tokenTypes.size(), 3);
    EXPECT_EQ(legend.typeName(0), QStringLiteral("variable"));
    EXPECT_EQ(legend.typeName(1), QStringLiteral("class"));
    EXPECT_EQ(legend.typeName(2), QStringLiteral("function"));
    // Out of range is empty, not "the last one" — an out-of-range index is a
    // protocol error and must not silently inherit a neighbour's meaning.
    EXPECT_TRUE(legend.typeName(3).isEmpty());
    EXPECT_TRUE(legend.typeName(-1).isEmpty());
}

TEST(SemanticLegend, ANonNegativeModifierIndexMapsToItsBit)
{
    const SemanticLegend legend = legendFrom(R"({
        "capabilities": {
            "semanticTokensProvider": {
                "legend": {
                    "tokenTypes": ["variable"],
                    "tokenModifiers": ["declaration", "readonly", "static"]
                }
            }
        }
    })");
    EXPECT_EQ(legend.modifierBit(QStringLiteral("declaration")), 0);
    EXPECT_EQ(legend.modifierBit(QStringLiteral("static")), 2);
    EXPECT_EQ(legend.modifierBit(QStringLiteral("virtual")), -1);
    EXPECT_TRUE(legend.hasModifier(0b101, QStringLiteral("static")));
    EXPECT_FALSE(legend.hasModifier(0b101, QStringLiteral("readonly")));
}

TEST(SemanticLegend, AServerWithoutTheProviderYieldsAnEmptyLegend)
{
    // The client must not send semanticTokens requests at all in this case:
    // the answer would be "method not found", logged as a failure every time
    // the user types.
    EXPECT_TRUE(legendFrom(R"({"capabilities": {}})").isEmpty());
    EXPECT_TRUE(legendFrom(R"({"capabilities": {"semanticTokensProvider": null}})").isEmpty());
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

TEST(SemanticDecode, ASecondLineTokenStartsAtAnAbsoluteColumn)
{
    // The rule that is easy to get wrong: deltaStartChar is relative to the
    // previous token's start *when deltaLine is 0*, and absolute when it is
    // not. A decoder that always adds produces a token at column 8 instead of
    // 4 on the second line — off by the first line's last column.
    QJsonArray data;
    for (const int value : {0, 4, 3, 0, 0,   // line 0, col 4, len 3, type 0
                            1, 4, 5, 2, 0}) { // line 1, col 4 (absolute)
        data.append(value);
    }
    QString error;
    const QList<SemanticToken> tokens =
        LightCpp::Workbench::Lsp::decodeSemanticTokens(data, &error);
    EXPECT_TRUE(error.isEmpty());
    ASSERT_EQ(tokens.size(), 2);
    EXPECT_EQ(tokens.at(0).line, 0);
    EXPECT_EQ(tokens.at(0).column, 4);
    EXPECT_EQ(tokens.at(0).length, 3);
    ASSERT_EQ(tokens.at(1).line, 1);
    EXPECT_EQ(tokens.at(1).column, 4);
    EXPECT_EQ(tokens.at(1).length, 5);
    EXPECT_EQ(tokens.at(1).type, 2);
}

TEST(SemanticDecode, AStreamThatIsNotAFiveTupleIsRejectedWholesale)
{
    // Decoding a truncated stream paints the first tokens correctly and the
    // rest at wrong offsets, which looks like a corrupted file. Refusing the
    // whole answer leaves the lexical colours — wrong but readable.
    QJsonArray data;
    for (const int value : {0, 0, 3, 0, 0, 0, 5}) {
        data.append(value);
    }
    QString error;
    const QList<SemanticToken> tokens =
        LightCpp::Workbench::Lsp::decodeSemanticTokens(data, &error);
    EXPECT_TRUE(tokens.isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(SemanticDecode, ANegativeDeltaIsRejectedInsteadOfWrappingTheColumn)
{
    // A negative column would index before the line start. Qt would clamp it,
    // so the IDE would underline the wrong token rather than notice.
    QJsonArray data;
    for (const int value : {0, 10, 3, 0, 0, 0, -4, 3, 0, 0}) {
        data.append(value);
    }
    QString error;
    const QList<SemanticToken> tokens =
        LightCpp::Workbench::Lsp::decodeSemanticTokens(data, &error);
    EXPECT_TRUE(tokens.isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(SemanticDecode, AnEmptyAnswerIsAnEmptyResultAndNotAnError)
{
    // "No tokens" is a legitimate answer (a file of only comments, or a
    // position clangd gave up on) and must not be logged as a failure.
    QString error;
    const QList<SemanticToken> tokens =
        LightCpp::Workbench::Lsp::decodeSemanticTokens(QJsonArray(), &error);
    EXPECT_TRUE(tokens.isEmpty());
    EXPECT_TRUE(error.isEmpty());
}

// ---------------------------------------------------------------------------
// Role mapping
// ---------------------------------------------------------------------------

TEST(SemanticRoles, TheTypesThatCarryInformationGetARole)
{
    using LightCpp::Workbench::Lsp::roleFor;
    EXPECT_EQ(roleFor(QStringLiteral("class")), TokenRole::Type);
    EXPECT_EQ(roleFor(QStringLiteral("struct")), TokenRole::Type);
    EXPECT_EQ(roleFor(QStringLiteral("type")), TokenRole::Type);
    EXPECT_EQ(roleFor(QStringLiteral("typeParameter")), TokenRole::TypeParameter);
    EXPECT_EQ(roleFor(QStringLiteral("function")), TokenRole::Function);
    EXPECT_EQ(roleFor(QStringLiteral("method")), TokenRole::Function);
    EXPECT_EQ(roleFor(QStringLiteral("namespace")), TokenRole::Namespace);
    EXPECT_EQ(roleFor(QStringLiteral("macros")), TokenRole::Unknown);
    EXPECT_EQ(roleFor(QStringLiteral("macro")), TokenRole::Macro);
    EXPECT_EQ(roleFor(QStringLiteral("parameter")), TokenRole::Parameter);
    EXPECT_EQ(roleFor(QStringLiteral("enumerator")), TokenRole::EnumMember);
}

TEST(SemanticRoles, AnUnknownNameFallsBackToTheLexicalColour)
{
    // A newer clangd may send a type the client has never heard of. Leaving it
    // Unknown keeps the syntax colour the lexical highlighter already applied
    // instead of painting it with whatever role 0 happens to be.
    EXPECT_EQ(LightCpp::Workbench::Lsp::roleFor(QStringLiteral("concept-lite")),
              TokenRole::Unknown);
    EXPECT_EQ(LightCpp::Workbench::Lsp::roleFor(QString()), TokenRole::Unknown);
}
