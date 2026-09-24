// LanguageServerClient — a minimal Language Server Protocol (LSP) client
// that talks to clangd over stdio (JSON-RPC 2.0 framed with Content-Length
// headers). Supports:
//   initialize / initialized handshake,
//   textDocument/didOpen / didChange (full-text) / didClose,
//   textDocument/publishDiagnostics notifications (push from server),
//   textDocument/completion request/response (manual + trigger-char).

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QList>
#include <QMap>

namespace LightCpp::App {

/// One diagnostic published by the language server.
/// Line/column values are 0-based, as mandated by LSP; characters are
/// UTF-16 code units (also LSP-mandated, and what QTextDocument uses
/// internally, so they can index a QTextDocument directly).
struct LspDiagnostic {
    int startLine = 0;
    int startColumn = 0;
    int endLine = 0;
    int endColumn = 0;
    /// 1 = Error, 2 = Warning, 3 = Information, 4 = Hint.
    int severity = 1;
    QString message;
    QString code;  // diagnostic code, when the server sends one
};

/// One completion item returned by the server. LSP CompletionItem.
struct LspCompletionItem {
    QString label;          // human-readable, shown in the popup
    QString detail;         // short extra info, e.g. "const int"
    QString documentation;  // longer description (markdown or plain)
    /// Text to insert when the item is accepted. Falls back to `label`
    /// when the server omits insertText (clangd often does this).
    QString insertText;
    /// LSP CompletionItemKind (1=Text, 2=Method, 3=Function, 4=Constructor,
    /// 5=Field, 6=Variable, 7=Class, 8=Interface, 9=Module, 10=Property,
    /// 11=Unit, 12=Value, 13=Enum, 14=Keyword, ...).
    int kind = 0;
};

class LanguageServerClient : public QObject {
    Q_OBJECT
public:
    /// `serverPath` is the executable to launch (e.g. clangd.exe);
    /// `rootPath` is the project root sent as rootUri.
    explicit LanguageServerClient(const QString& serverPath,
                                  const QString& rootPath,
                                  QObject* parent = nullptr);
    ~LanguageServerClient() override;

    /// Launch the server and perform the initialize handshake.
    /// Safe to call again after stop(); re-starts a fresh process.
    void start();

    /// Graceful shutdown: LSP "shutdown" request + "exit" notification,
    /// then kill the process if it does not exit on its own.
    void stop();

    [[nodiscard]] bool isRunning() const;

    // --- document synchronization (full-text sync, textDocumentSync=1) ---

    /// Notify the server that a document was opened. `languageId` is e.g.
    /// "cpp" / "c". Only meaningful while the server is running.
    void openDocument(const QString& filePath, const QString& languageId,
                      const QString& text);

    /// Send the full new text of a previously opened document.
    void changeDocument(const QString& filePath, const QString& text);

    /// Notify the server that a document was closed.
    void closeDocument(const QString& filePath);

    // --- completion -------------------------------------------------------

    /// Ask the server for completion items at (line, column). `triggerChar`
    /// is the character that triggered the request (e.g. ".", ":", ">")
    /// — pass an empty string for manual (Ctrl+Space) requests.
    /// Returns the JSON-RPC request id, or 0 if not sent.
    int requestCompletion(const QString& filePath, int line, int column,
                          const QString& triggerChar = QString());

    // --- URI helpers (LSP uses file:// URIs with percent encoding) ---

    [[nodiscard]] static QString toFileUri(const QString& filePath);
    [[nodiscard]] static QString fromFileUri(const QString& uri);

signals:
    /// Pushed by the server after didOpen/didChange. `filePath` is the
    /// local path (URI already resolved). An empty list clears diagnostics.
    void diagnosticsReady(const QString& filePath,
                           const QList<LspDiagnostic>& diagnostics);

    /// Completion response arrived. `requestId` is the id returned by
    /// requestCompletion; callers usually track the pending request
    /// themselves and ignore stale results.
    void completionReady(int requestId, const QString& filePath,
                         const QList<LspCompletionItem>& items);

    /// The initialize handshake completed; documents can now be opened.
    void serverStarted();

    /// A line of the server's own log: its stderr output plus any
    /// "window/logMessage" / "window/showMessage" notifications it pushes.
    /// Intended for the IDE's clangd log channel.
    void serverLog(const QString& message);

    /// The server process failed to start or died unexpectedly.
    void serverFailed(const QString& message);

private:
    void sendJsonRpc(const QJsonObject& message);
    void onReadyRead();
    void tryParseFrames();
    void handleMessage(const QJsonObject& message);
    void handlePublishDiagnostics(const QJsonObject& params);
    void requestShutdown();

    QString serverPath_;
    QString rootPath_;
    QProcess* process_ = nullptr;
    QByteArray buffer_;                  // partial frame accumulation
    QMap<QString, int> documentVersions_;  // fileUri -> last version sent
    QMap<int, QString> completionRequests_;  // requestId -> fileUri
    bool initialized_ = false;          // handshake done
    int nextRequestId_ = 1;             // JSON-RPC request id counter
};

}  // namespace LightCpp::App
