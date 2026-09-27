// LanguageServerClient — a minimal Language Server Protocol (LSP) client
// that talks to clangd over stdio (JSON-RPC 2.0 framed with Content-Length
// headers). Supports:
//   initialize / initialized handshake,
//   textDocument/didOpen / didChange (full-text) / didClose,
//   textDocument/publishDiagnostics notifications (push from server),
//   textDocument/completion request/response (manual + trigger-char),
//   textDocument/definition request/response (go-to-definition),
//   and telling clangd which compiler's headers to resolve includes against.

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QList>
#include <QMap>

#include "LightCpp/App/ToolchainHeaders.hpp"

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

/// A source position returned by a "go to definition" answer.
/// Line/column are 0-based LSP units (UTF-16 code units), i.e. directly
/// usable with QTextCursor. An empty `filePath` means the server found
/// nothing at the requested position — check isValid() before jumping.
struct LspLocation {
    QString filePath;
    int line = 0;
    int column = 0;

    [[nodiscard]] bool isValid() const noexcept { return !filePath.isEmpty(); }
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

    /// Change the executable to launch. Takes effect at the next start();
    /// used when the settings name a clangd other than the compiled-in one.
    void setServerPath(const QString& path) { serverPath_ = path; }
    [[nodiscard]] const QString& serverPath() const noexcept
    {
        return serverPath_;
    }

    /// The --fallback-style passed to clangd (used by its built-in
    /// formatting). "file" and empty keep clangd's own default. Only read
    /// at start(), like the toolchain headers.
    void setFallbackStyle(const QString& style) { fallbackStyle_ = style; }

    /// Launch the server and perform the initialize handshake.
    /// Safe to call again after stop(); re-starts a fresh process.
    ///
    /// clangd reads its include paths once, when it starts, so a change made
    /// through setToolchainHeaders() only takes effect after stop() + start().
    void start();

    /// Graceful shutdown: LSP "shutdown" request + "exit" notification,
    /// then kill the process if it does not exit on its own.
    void stop();

    [[nodiscard]] bool isRunning() const;

    /// Choose which compiler's headers the server resolves includes against.
    ///
    /// clangd has no other way to be told this: include paths reach it only
    /// through a compilation database. When the project owns one (a
    /// compile_commands.json under the project root or a build directory),
    /// start() points clangd at it and it takes precedence — it carries the
    /// project's own -I flags. Otherwise this client writes a single-file
    /// database (compile_flags.txt) inside the temp directory — never in the
    /// user's source tree — and starts clangd with --compile-commands-dir
    /// pointing at it. An empty `headers` skips all of that and leaves
    /// clangd's own defaults in place.
    void setToolchainHeaders(const ToolchainHeaders& headers);

    /// The headers the server is currently configured with. Empty means it is
    /// using its own.
    [[nodiscard]] const ToolchainHeaders& toolchainHeaders() const noexcept
    {
        return headers_;
    }

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

    // --- go to definition -------------------------------------------------

    /// Ask the server where the symbol at (line, column) is defined.
    /// Returns the JSON-RPC request id, or 0 if not sent; the answer
    /// arrives asynchronously via definitionReady().
    int requestDefinition(const QString& filePath, int line, int column);

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

    /// Definition response arrived. `requestId` is the id returned by
    /// requestDefinition. `location` may be invalid (no definition at the
    /// requested position, e.g. the caret was on whitespace or a keyword).
    void definitionReady(int requestId, const QString& filePath,
                         const LspLocation& location);

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
    /// Style passed as clangd's --fallback-style; empty/"file" = its own.
    QString fallbackStyle_;
    /// Headers the server should resolve against; empty means "its own".
    ToolchainHeaders headers_;
    QProcess* process_ = nullptr;
    QByteArray buffer_;                  // partial frame accumulation
    QMap<QString, int> documentVersions_;  // fileUri -> last version sent
    QMap<int, QString> completionRequests_;  // requestId -> fileUri
    QMap<int, QString> definitionRequests_;  // requestId -> fileUri
    bool initialized_ = false;          // handshake done
    int nextRequestId_ = 1;             // JSON-RPC request id counter
    /// Id of the outstanding "initialize" request. It cannot be assumed to be
    /// 1: this client is restarted (stop() + start()) whenever the toolchain
    /// changes, and the counter keeps running across restarts.
    int initializeRequestId_ = 0;
};

}  // namespace LightCpp::App
