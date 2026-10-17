#include "LightCpp/App/LspResponse.hpp"

#include <QJsonValue>

namespace LightCpp::App::LspResponse {
namespace {

QString nameOf(Kind kind)
{
    switch (kind) {
    case Kind::Completion:
        return QStringLiteral("Completion");
    case Kind::Definition:
        return QStringLiteral("Go to Definition");
    case Kind::Declaration:
        return QStringLiteral("Go to Declaration");
    case Kind::Implementation:
        return QStringLiteral("Go to Implementation");
    case Kind::References:
        return QStringLiteral("Find References");
    case Kind::SignatureHelp:
        return QStringLiteral("Signature help");
    case Kind::DocumentSymbols:
        return QStringLiteral("Symbol list");
    case Kind::Rename:
        return QStringLiteral("Rename");
    case Kind::CodeActions:
        return QStringLiteral("Quick fixes");
    }
    return QStringLiteral("Request");
}

}  // namespace

Error errorOf(const QJsonObject& message)
{
    Error error;
    const QJsonValue value = message.value(QStringLiteral("error"));
    // `null` (and the member being absent) means the request did not fail.
    // Anything else is a failure even when it is not the object JSON-RPC
    // prescribes — a malformed error is still not a successful answer.
    if (value.isUndefined() || value.isNull()) {
        return error;
    }
    error.present = true;
    if (value.isString()) {
        error.message = value.toString().trimmed();
        if (error.message.isEmpty()) {
            error.message = QStringLiteral("no reason was given");
        }
        return error;
    }
    const QJsonObject object = value.toObject();
    error.message =
        object.value(QStringLiteral("message")).toString().trimmed();
    if (!error.message.isEmpty()) {
        return error;
    }
    // The code is all the server left: -32601 (method not found) and friends
    // still tell the user more than silence.
    const QJsonValue code = object.value(QStringLiteral("code"));
    if (code.isDouble()) {
        error.message = QStringLiteral("error %1").arg(code.toInt());
    } else if (code.isString() && !code.toString().trimmed().isEmpty()) {
        error.message = code.toString().trimmed();
    } else {
        error.message = QStringLiteral("no reason was given");
    }
    return error;
}

QString describe(Kind kind, const QString& serverMessage)
{
    const QString reason = serverMessage.trimmed();
    if (reason.isEmpty()) {
        return QStringLiteral("%1 failed").arg(nameOf(kind));
    }
    return QStringLiteral("%1 failed: %2").arg(nameOf(kind), reason);
}

}  // namespace LightCpp::App::LspResponse
