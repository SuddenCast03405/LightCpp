// LspResponseTests — telling "the server said no" from "the server said nothing
// there".
//
// R2-22: every response handler read only `result`. A JSON-RPC error response
// has no `result`, so it parsed as an empty answer — the IDE printed "Nothing
// found at that position.", opened an empty completion box, or said nothing
// about a rename, in exactly the shape a successful empty answer has. The two
// are different facts and only the response can separate them.

#include "LightCpp/App/LspResponse.hpp"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>

using namespace LightCpp::App::LspResponse;

namespace {

QJsonObject parse(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

}  // namespace

TEST(LspResponse, ASuccessfulRequestHasNoError)
{
    const Error error = errorOf(parse(R"({"jsonrpc":"2.0","id":1,"result":[]})"));
    EXPECT_FALSE(error.present);
    EXPECT_TRUE(error.message.isEmpty());
}

TEST(LspResponse, ANullResultIsAnAnswerNotAFailure)
{
    // The distinction this whole module exists for: `result: null` is the
    // server saying "nothing found", and must keep meaning that.
    const Error error =
        errorOf(parse(R"({"jsonrpc":"2.0","id":7,"result":null})"));
    EXPECT_FALSE(error.present);
}

TEST(LspResponse, AnErrorCarriesTheServersOwnText)
{
    const Error error = errorOf(parse(
        R"({"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"method not found"}})"));
    EXPECT_TRUE(error.present);
    EXPECT_EQ(error.message, QStringLiteral("method not found"));
}

TEST(LspResponse, AnErrorWithOnlyACodeStillSaysWhich)
{
    const Error error =
        errorOf(parse(R"({"id":3,"error":{"code":-32601}})"));
    EXPECT_TRUE(error.present);
    EXPECT_NE(error.message.indexOf(QStringLiteral("32601")), -1);
}

TEST(LspResponse, AnEmptyErrorObjectStillSaysSomething)
{
    const Error error = errorOf(parse(R"({"id":4,"error":{}})"));
    EXPECT_TRUE(error.present);
    EXPECT_FALSE(error.message.isEmpty());
    EXPECT_EQ(error.message, QStringLiteral("no reason was given"));
}

TEST(LspResponse, AMalformedErrorIsStillAFailure)
{
    // Not what JSON-RPC prescribes, but a response with an error member is not
    // a successful answer, and treating it as one loses the only evidence.
    const Error error = errorOf(parse(R"({"id":5,"error":"boom"})"));
    EXPECT_TRUE(error.present);
    EXPECT_EQ(error.message, QStringLiteral("boom"));
}

TEST(LspResponse, TheDescriptionNamesTheRequestAndTheReason)
{
    EXPECT_EQ(describe(Kind::Definition, QStringLiteral("method not found")),
              QStringLiteral("Go to Definition failed: method not found"));
}

TEST(LspResponse, EveryRequestKindHasItsOwnName)
{
    const Kind kinds[] = {
        Kind::Completion,      Kind::Definition,    Kind::Declaration,
        Kind::Implementation,  Kind::References,    Kind::SignatureHelp,
        Kind::DocumentSymbols, Kind::Rename,        Kind::CodeActions,
    };
    QSet<QString> names;
    for (Kind kind : kinds) {
        const QString text = describe(kind, QString());
        EXPECT_FALSE(text.isEmpty());
        names.insert(text);
    }
    // Nine requests, nine different sentences: two collapsed names would tell
    // the user the wrong feature failed.
    EXPECT_EQ(names.size(), 9);
}

TEST(LspResponse, AnEmptyReasonDoesNotLeaveATrailingColon)
{
    EXPECT_EQ(describe(Kind::Rename, QStringLiteral("   ")),
              QStringLiteral("Rename failed"));
}
