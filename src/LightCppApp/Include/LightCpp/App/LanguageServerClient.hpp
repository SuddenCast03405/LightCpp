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
    /// The item's textEdit range (0-based LSP units, UTF-16 columns), when
    /// the server sent one. When `hasTextEditRange` is true and the range
    /// sits on the caret's line, the editor replaces exactly that range —
    /// inserting only `newText` would leave the old prefix behind (e.g.
    /// "#incl" + "ude <...>" would produce "#incl#inclu..." doubled text).
    bool hasTextEditRange = false;
    int rangeStartLine = 0;
    int rangeStartColumn = 0;
    int rangeEndLine = 0;
    int rangeEndColumn = 0;
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

/// The active signature of a textDocument/signatureHelp answer: the call's
/// full label (e.g. "void foo(int a, const QString &b)") plus the UTF-16
/// label range of the parameter the caret sits in (-1 when clangd could not
/// narrow it down). Empty `label` = nothing to show at that position.
struct LspSignatureHelp {
    QString label;
    int paramStart = -1;
    int paramEnd = -1;
};

/// One symbol from a textDocument/documentSymbol answer, flattened to what
/// navigation needs. `line`/`column` (0-based LSP units) point at the
/// symbol's identifier (selectionRange); `endLine` bounds the whole symbol
/// (range end) — the outline/breadcrumb logic uses it to decide whether the
/// caret sits "inside" a symbol. Children are nested as the server sent
/// them (DocumentSymbol shape); a SymbolInformation answer yields a flat
/// list with empty children.
struct LspSymbol {
    QString name;
    QString detail;  // e.g. the prototype for functions
    int kind = 0;    // LSP SymbolKind (1=File..26=...; 12=Function, 6=Method)
    int line = 0;
    int column = 0;
    int endLine = 0;
    QList<LspSymbol> children;
};

/// One concrete text edit inside a workspace edit (rename / codeAction
/// answers), with the owning document resolved to a local path. Ranges are
/// 0-based UTF-16 positions, directly usable with QTextCursor.
struct LspTextEdit {
    QString filePath;
    int line = 0;
    int column = 0;
    int endLine = 0;
    int endColumn = 0;
    QString newText;
};

/// One quick fix returned by textDocument/codeAction. `edits` is the
/// flattened workspace edit to apply; it is empty when the action is a
/// command (clangd's applyFix commands) — those are skipped by the caller.
struct LspCodeAction {
    QString title;
    QString kind;  // "quickfix", "refactor", ...
    QList<LspTextEdit> edits;
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

    // --- declaration / implementation / references -------------------------

    /// textDocument/declaration — where the symbol is declared (usually the
    /// header prototype rather than the definition). Answer via
    /// locationsReady().
    int requestDeclaration(const QString& filePath, int line, int column);

    /// textDocument/implementation — where the symbol is implemented
    /// (vtable overrides, function bodies). Answer via locationsReady().
    int requestImplementation(const QString& filePath, int line, int column);

    /// textDocument/references — every usage of the symbol. Answer via
    /// locationsReady().
    int requestReferences(const QString& filePath, int line, int column,
                          bool includeDeclaration = true);

    /// textDocument/signatureHelp — the call signature at (line, column),
    /// requested after `(` or `,`. Answer via signatureHelpReady().
    int requestSignatureHelp(const QString& filePath, int line, int column);

    /// textDocument/documentSymbol — every symbol of the document, as a
    /// (possibly nested) list. Answer via documentSymbolsReady().
    int requestDocumentSymbols(const QString& filePath);

    /// textDocument/rename — rename the symbol at (line, column) to
    /// `newName`. The answer is a WorkspaceEdit flattened into a list of
    /// concrete text edits across all affected documents; the caller
    /// applies them to the open editors. Answer via renameReady().
    int requestRename(const QString& filePath, int line, int column,
                      const QString& newName);

    /// textDocument/codeAction — the quick fixes available for `diagnostics`
    /// at (line, column). The `edits` out-list gets one entry per returned
    /// action, in the server's order; an entry's edit list may be empty when
    /// the action is a command instead of a workspace edit.
    int requestCodeActions(const QString& filePath, int line, int column,
                           const QList<LspDiagnostic>& diagnostics);

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

    /// Declaration/implementation/references response arrived. The list may
    /// be empty (nothing found at that position). For declaration and
    /// implementation clangd usually returns one location; references can
    /// return hundreds — cap them at the call site if needed.
    void locationsReady(int requestId, const QString& filePath,
                        const QList<LspLocation>& locations);

    /// Signature help response arrived. An empty label means no call
    /// context at that position (hide whatever is showing).
    void signatureHelpReady(int requestId, const LspSignatureHelp& help);

    /// Document symbol response arrived. The list may be empty (no symbols,
    /// or the file is not a parseable language).
    void documentSymbolsReady(int requestId, const QString& filePath,
                              const QList<LspSymbol>& symbols);

    /// Rename response arrived. An empty list means the position had no
    /// renameable symbol (or the new name was rejected). Edits span every
    /// document the server touched — apply only the ones whose file is open.
    void renameReady(int requestId, const QList<LspTextEdit>& edits);

    /// Code action response arrived, in the server's order. An empty list
    /// means no fixes at that position.
    void codeActionsReady(int requestId,
                          const QList<LspCodeAction>& actions);

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

    /// Report a start-up/exit failure to the outside world.
    ///
    /// The serverFailed slot runs synchronously and may tear this client
    /// down: MainWindow reacts by unchecking Syntax Check, which calls
    /// stop(), which deletes the QProcess. Two rules make that safe:
    ///
    ///  * The process is detached (process_ = nullptr) and handed to the
    ///    event loop first, so a stop() from the slot is a no-op instead
    ///    of deleting the object that is emitting.
    ///  * The signal itself is queued, so the teardown never runs from
    ///    inside a QProcess signal. errorOccurred(FailedToStart) is
    ///    delivered from *within* QProcess::start(); destroying the
    ///    process there made Qt resume startProcess() on freed memory
    ///    (SIGSEGV, worth a hard crash of the whole IDE).
    void reportServerFailure(const QString& message);

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
    QMap<int, QString> declarationRequests_;      // requestId -> fileUri
    QMap<int, QString> implementationRequests_;   // requestId -> fileUri
    QMap<int, QString> referencesRequests_;       // requestId -> fileUri
    QMap<int, QString> signatureHelpRequests_;    // requestId -> fileUri
    QMap<int, QString> documentSymbolRequests_;   // requestId -> fileUri
    QMap<int, int> renameRequests_;               // requestId -> 0 (pending)
    QMap<int, int> codeActionRequests_;           // requestId -> 0 (pending)
    bool initialized_ = false;          // handshake done
    bool everInitialized_ = false;      // handshake completed at least once —
                                        // separates "exited during start-up"
                                        // (serverFailed) from a later exit
    int nextRequestId_ = 1;             // JSON-RPC request id counter
    /// Id of the outstanding "initialize" request. It cannot be assumed to be
    /// 1: this client is restarted (stop() + start()) whenever the toolchain
    /// changes, and the counter keeps running across restarts.
    int initializeRequestId_ = 0;
};

}  // namespace LightCpp::App
