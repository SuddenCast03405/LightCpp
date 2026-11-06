#pragma once

#include <QJsonObject>
#include <QString>

namespace LightCpp::App::LspResponse {

/// Which request a response answers. Only used to name the failure: the
/// handlers each know their own kind, and the name is what the user reads.
enum class Kind {
    Completion,
    Definition,
    Declaration,
    Implementation,
    References,
    SignatureHelp,
    DocumentSymbols,
    Rename,
    CodeActions,
    /// textDocument/semanticTokens/* — what the semantic highlighter asks for.
    SemanticTokens,
    /// textDocument/prepareCallHierarchy and callHierarchy/incomingCalls,
    /// callHierarchy/outgoingCalls.
    CallHierarchy,
    /// textDocument/prepareTypeHierarchy and typeHierarchy/supertypes,
    /// typeHierarchy/subtypes.
    TypeHierarchy,
    /// textDocument/hover — the type / doc comment under the caret.
    Hover,
};

/// What a response says when it reports a failure.
struct Error {
    /// False for an ordinary answer, including a null result — "nothing found"
    /// is an answer, and must keep meaning that.
    bool present = false;
    /// The server's own text. When it sent none, this still says something:
    /// its error code, or "no reason was given".
    QString message;
};

/// Read the `error` member of a JSON-RPC response.
///
/// R2-22: every handler read only `result`, so a response carrying an error
/// parsed as an empty result — "Nothing found at that position.", an empty
/// completion box, an empty quick-fix menu — which is the same screen a
/// successful answer with no hits produces. The two are not the same thing,
/// and only the response itself can tell them apart.
[[nodiscard]] Error errorOf(const QJsonObject& message);

/// The line shown when request `kind` failed: "<what was asked> failed: <why>".
[[nodiscard]] QString describe(Kind kind, const QString& serverMessage);

}  // namespace LightCpp::App::LspResponse
