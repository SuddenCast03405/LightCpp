// LanguageServerClient — implementation.

#include "LightCpp/App/LanguageServerClient.hpp"

#include <cstring>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSet>
#include <QUrl>

namespace LightCpp::App {

namespace {

/// Parse the "range" object of a diagnostic into start/end positions.
struct LspRange {
    int startLine = 0, startColumn = 0, endLine = 0, endColumn = 0;
};

LspRange parseRange(const QJsonObject& range)
{
    LspRange r;
    const QJsonObject start = range.value("start").toObject();
    const QJsonObject end = range.value("end").toObject();
    r.startLine = start.value("line").toInt();
    r.startColumn = start.value("character").toInt();
    r.endLine = end.value("line").toInt();
    r.endColumn = end.value("character").toInt();
    return r;
}

/// Infer the LSP languageId from a file extension. Used when a document
/// must be auto-opened (the server requires didOpen before didChange).
QString languageIdForExtension(const QString& filePath)
{
    const QString suf = QFileInfo(filePath).suffix().toLower();
    if (suf == QLatin1String("c") || suf == QLatin1String("h")) {
        return QStringLiteral("c");
    }
    return QStringLiteral("cpp");
}

/// Turn one entry of a textDocument/definition result into an LspLocation.
/// Two shapes are legal: "Location" ({uri, range}) and "LocationLink"
/// ({targetUri, targetSelectionRange, targetRange}), the latter used by
/// servers that point at the identifier rather than the whole declaration.
/// Returns false when neither shape carries a usable local file URI.
bool parseLocation(const QJsonObject& object, LspLocation& out)
{
    QString uri = object.value("uri").toString();
    QJsonObject range = object.value("range").toObject();
    if (uri.isEmpty()) {
        uri = object.value("targetUri").toString();
        // Prefer the identifier itself: targetRange would put the caret on
        // the return type or a leading attribute instead of on the name.
        range = object.value("targetSelectionRange").toObject();
        if (range.isEmpty()) {
            range = object.value("targetRange").toObject();
        }
    }
    if (uri.isEmpty()) {
        return false;
    }
    // clangd answers non-file URIs (for builtins, say); there is nowhere to
    // jump, so an empty path from fromFileUri() is the correct answer.
    out.filePath = LanguageServerClient::fromFileUri(uri);
    if (out.filePath.isEmpty()) {
        return false;
    }
    const QJsonObject start = range.value("start").toObject();
    out.line = start.value("line").toInt();
    out.column = start.value("character").toInt();
    return true;
}

/// Parse one DocumentSymbol object (hierarchical shape) into an LspSymbol.
/// selectionRange's start is the jump target; range's end bounds the symbol.
bool parseDocumentSymbol(const QJsonObject& object, LspSymbol& out)
{
    const QString name = object.value("name").toString();
    if (name.isEmpty()) {
        return false;
    }
    out.name = name;
    out.detail = object.value("detail").toString();
    out.kind = object.value("kind").toInt(0);
    const QJsonObject range = object.value("range").toObject();
    const QJsonObject selection = object.value("selectionRange").toObject();
    const QJsonObject selectionStart =
        (selection.isEmpty() ? range : selection).value("start").toObject();
    out.line = selectionStart.value("line").toInt();
    out.column = selectionStart.value("character").toInt();
    out.endLine = range.value("end").toObject().value("line").toInt();
    const QJsonArray children = object.value("children").toArray();
    for (const QJsonValue& v : children) {
        LspSymbol child;
        if (parseDocumentSymbol(v.toObject(), child)) {
            out.children.append(child);
        }
    }
    return true;
}

/// Parse one SymbolInformation object (flat shape) into an LspSymbol.
bool parseSymbolInformation(const QJsonObject& object, LspSymbol& out)
{
    const QString name = object.value("name").toString();
    if (name.isEmpty()) {
        return false;
    }
    out.name = name;
    out.kind = object.value("kind").toInt(0);
    out.detail = object.value("containerName").toString();
    const QJsonObject location = object.value("location").toObject();
    const QJsonObject range = location.value("range").toObject();
    const QJsonObject start = range.value("start").toObject();
    out.line = start.value("line").toInt();
    out.column = start.value("character").toInt();
    out.endLine = range.value("end").toObject().value("line").toInt();
    return true;
}

/// Parse one LSP TextEdit {range, newText} into an LspTextEdit owned by
/// `filePath` (already a local path).
bool parseTextEdit(const QJsonObject& object, const QString& filePath,
                   LspTextEdit& out)
{
    const QJsonObject range = object.value("range").toObject();
    if (range.isEmpty()) {
        return false;
    }
    out.filePath = filePath;
    const QJsonObject start = range.value("start").toObject();
    const QJsonObject end = range.value("end").toObject();
    out.line = start.value("line").toInt();
    out.column = start.value("character").toInt();
    out.endLine = end.value("line").toInt();
    out.endColumn = end.value("character").toInt();
    out.newText = object.value("newText").toString();
    return true;
}

/// Flatten a WorkspaceEdit (rename / codeAction answers) into a plain list
/// of text edits. Handles both wire shapes: `changes` ({uri: [TextEdit]})
/// and `documentChanges` ([{textDocument:{uri}, edits:[...]}]).
QList<LspTextEdit> parseWorkspaceEdit(const QJsonValue& value)
{
    QList<LspTextEdit> edits;
    const QJsonObject we = value.toObject();
    const QJsonObject changes = we.value("changes").toObject();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const QString filePath = LanguageServerClient::fromFileUri(it.key());
        for (const QJsonValue& v : it.value().toArray()) {
            LspTextEdit edit;
            if (parseTextEdit(v.toObject(), filePath, edit)) {
                edits.append(edit);
            }
        }
    }
    for (const QJsonValue& v : we.value("documentChanges").toArray()) {
        const QJsonObject entry = v.toObject();
        const QString filePath = LanguageServerClient::fromFileUri(
            entry.value("textDocument").toObject().value("uri").toString());
        for (const QJsonValue& ev : entry.value("edits").toArray()) {
            LspTextEdit edit;
            if (parseTextEdit(ev.toObject(), filePath, edit)) {
                edits.append(edit);
            }
        }
    }
    return edits;
}

/// Directory holding the generated clangd flags file. Under the temp
/// directory on purpose: nothing about this is the user's project, and their
/// source tree must not be littered with build-system leftovers.
QString clangdFlagsDir()
{
    return QDir(QDir::tempPath()).filePath(QStringLiteral("lightcpp-clangd"));
}

/// Locate a compilation database the project itself owns — CMake's
/// CMAKE_EXPORT_COMPILE_COMMANDS output. Probes the project root and the
/// conventional build-directory layouts one and two levels below it
/// (build/, build/Debug, out/build/Debug, ...). Returns the directory that
/// holds compile_commands.json, or empty when the project has none.
QString findProjectCompileCommandsDir(const QString& rootPath)
{
    if (rootPath.isEmpty()) {
        return {};
    }
    const QDir root(rootPath);
    if (!root.exists()) {
        return {};
    }
    QStringList dirs;
    dirs.append(root.absolutePath());
    const QStringList topLevel =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : topLevel) {
        const QDir level1(root.filePath(entry));
        dirs.append(level1.absolutePath());
        dirs.append(level1.filePath(QStringLiteral("Debug")));
        dirs.append(level1.filePath(QStringLiteral("Release")));
        const QStringList nested =
            level1.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& nestedEntry : nested) {
            dirs.append(level1.filePath(nestedEntry));
        }
    }
    for (const QString& dir : dirs) {
        if (QFileInfo::exists(QDir(dir).filePath(
                QStringLiteral("compile_commands.json")))) {
            return QDir(dir).absolutePath();
        }
    }
    return {};
}

/// Write the one-file compilation database clangd reads its include paths from.
///
/// compile_flags.txt takes one argument per line, applied to every file, which
/// is exactly the right granularity here — the choice is per toolchain, not per
/// file. Paths therefore need no quoting (and must not have any: quotes would
/// be taken as part of the path).
bool writeClangdFlags(const LightCpp::App::ToolchainHeaders& headers)
{
    const QString dir = clangdFlagsDir();
    if (!QDir().mkpath(dir)) {
        return false;
    }
    QStringList lines;
    if (!headers.target.isEmpty()) {
        lines.append(QStringLiteral("--target=%1").arg(headers.target));
    }
    lines += headers.extraFlags;
    for (const QString& include : headers.includeDirs) {
        lines.append(QStringLiteral("-isystem"));
        lines.append(QDir::fromNativeSeparators(include));
    }
    QFile file(QDir(dir).filePath(QStringLiteral("compile_flags.txt")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n') >= 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

LanguageServerClient::LanguageServerClient(const QString& serverPath,
                                           const QString& rootPath,
                                           QObject* parent)
    : QObject(parent)
    , serverPath_(serverPath)
    , rootPath_(rootPath)
{
}

LanguageServerClient::~LanguageServerClient()
{
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void LanguageServerClient::start()
{
    if (process_ != nullptr) {
        return;  // already running or starting up
    }
    if (!QFileInfo::exists(serverPath_)) {
        // Nothing to tear down; this only queues the notification.
        reportServerFailure(tr("language server not found: %1").arg(serverPath_));
        return;
    }

    initialized_ = false;
    everInitialized_ = false;
    // Drop, not close: callbacks queued for the previous handshake belong to a
    // request nobody is waiting for any more, and must not fire on this one.
    readyGate_.drop();
    documentVersions_.clear();
    buffer_.clear();

    process_ = new QProcess(this);
    // Keep stderr separate: clangd logs its progress there and the IDE shows
    // it in the "Clangd Log" channel.
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    // The server usually lives in a toolchain directory whose DLLs must be
    // on PATH for the process to start (MSYS2 clangd links against its
    // sibling DLLs). Extend the child environment with the server's own
    // directory.
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QString serverDir = QFileInfo(serverPath_).absolutePath();
        env.insert("PATH", serverDir + QDir::listSeparator() + env.value("PATH"));
        process_->setProcessEnvironment(env);
    }

    connect(process_, &QProcess::readyReadStandardOutput,
            this, &LanguageServerClient::onReadyRead);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        if (process_ == nullptr) return;
        // clangd/ccls write UTF-8 (their own source paths land there);
        // fromLocal8Bit garbled them on non-UTF-8 system code pages.
        const QString text = QString::fromUtf8(process_->readAllStandardError());
        // Emit line by line so the log pane never gets a half-written line.
        for (const QString& line : text.split(u'\n', Qt::SkipEmptyParts)) {
            emit serverLog(line.trimmed());
        }
    });
    connect(process_, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        // A stale process from an earlier start() must not take the
        // current one down with it.
        if (sender() != process_) return;
        // Never call stop() here: this signal is emitted from inside
        // QProcess::start(), so deleting the process now would resume
        // Qt's startProcess() on freed memory. reportServerFailure
        // detaches the process and queues the notification instead.
        reportServerFailure(tr("failed to start language server: %1")
                                .arg(serverPath_));
    });
    connect(process_, &QIODevice::aboutToClose, this, [this] {
        // Process ended (possibly on its own).
        if (process_ != nullptr) {
            initialized_ = false;
            readyGate_.drop();  // no handshake is coming from this process
        }
    });
    connect(process_, &QProcess::finished, this, [this] {
        // The process ended. Use sender() so we never touch the member
        // pointer — stop() may have already nulled it (the synchronous
        // waitForFinished below can deliver finished() to this slot
        // while stop() is mid-flight, which previously crashed on the
        // process_->deleteLater() line).
        auto* p = qobject_cast<QProcess*>(sender());
        if (p == nullptr) {
            return;
        }
        // Read everything off the process, then detach it, before any
        // emit: the serverFailed handler may synchronously walk into
        // stop() and free this very object.
        const int exitCode = p->exitCode();
        const bool duringStartup = !everInitialized_;
        const bool wasCurrent = process_ == p;
        if (wasCurrent) {
            process_ = nullptr;
            // Only the current process owns the handshake state. A stale
            // process finishing after a restart must not un-initialize the new
            // one (that would leave a live server the client cannot talk to)
            // nor drop the requests now queued for its handshake.
            initialized_ = false;
            readyGate_.drop();
        }
        p->deleteLater();
        if (duringStartup) {
            // A server that dies before the LSP handshake completes would
            // otherwise vanish silently: ccls rejects unknown command-line
            // arguments by printing to stderr and exiting with code 1, and
            // QProcess only reports FailedToStart when CreateProcess fails.
            QMetaObject::invokeMethod(
                this,
                [this, exitCode] {
                    emit serverFailed(
                        tr("language server exited during start-up "
                           "(exit code %1)")
                            .arg(exitCode));
                },
                Qt::QueuedConnection);
        } else {
            emit serverLog(tr("language server exited (exit code %1)")
                               .arg(exitCode));
        }
    });

    // Light options: no background indexing, no clang-tidy checks — this is
    // a plain syntax-check integration; full compilation-database support
    // can come later. These flags are clangd-specific: ccls understands the
    // LSP protocol but not clangd's command line — it rejects even
    // "--background-index" with "Unknown command line argument" and exits
    // right away (its options live behind --init=<json>), so it starts
    // with no flags at all.
    const bool isCcls = QFileInfo(serverPath_).fileName()
                            .contains(QLatin1String("ccls"),
                                      Qt::CaseInsensitive);
    QStringList args;
    if (!isCcls) {
        args << QStringLiteral("--background-index=false")
             << QStringLiteral("--clang-tidy=false")
             << QStringLiteral("--pch-storage=memory");
    }

    // The settings' clangd fallback style, when it names a real preset.
    // "file" would make clangd look for a .clang-format it usually cannot
    // find, which only changes its format-on-type answers for the worse.
    // ccls has no such flag.
    if (!isCcls && !fallbackStyle_.isEmpty()
        && fallbackStyle_.compare(QLatin1String("file"), Qt::CaseInsensitive) != 0) {
        args.append(QStringLiteral("--fallback-style=") + fallbackStyle_);
    }

    // Include paths reach clangd only through a compilation database. When
    // the project owns one (CMake's CMAKE_EXPORT_COMPILE_COMMANDS output in
    // a build directory), it is the authoritative source — it carries the
    // per-target -I flags for the project's own headers — so it wins over
    // the synthesized file. The compiler's builtin headers still resolve:
    // clangd queries the compiler named inside the database itself.
    const QString projectDbDir = findProjectCompileCommandsDir(rootPath_);
    if (!projectDbDir.isEmpty()) {
        // clangd takes a command-line flag; ccls has no such flag and reads
        // the database directory from its --init JSON instead. Forward
        // slashes keep the JSON escaping trivial.
        if (isCcls) {
            args.append(QStringLiteral(
                            "--init={\"compilationDatabaseDirectory\":\"")
                        + projectDbDir + QStringLiteral("\"}"));
        } else {
            args.append(QStringLiteral("--compile-commands-dir=") + projectDbDir);
        }
        emit serverLog(tr("using project compilation database: %1")
                           .arg(QDir::toNativeSeparators(QDir(projectDbDir)
                               .filePath(QStringLiteral("compile_commands.json")))));
    } else if (!isCcls && !headers_.isEmpty()) {
        // No project database: synthesise a one-file database pointing at the
        // selected toolchain's headers and start clangd against it, without
        // writing anything into the user's project.
        if (writeClangdFlags(headers_)) {
            args.append(QStringLiteral("--compile-commands-dir=") + clangdFlagsDir());
        } else {
            emit serverLog(tr("warning: could not write %1; the editor will "
                              "resolve includes against its own headers")
                               .arg(QDir::toNativeSeparators(
                                   QDir(clangdFlagsDir())
                                       .filePath(QStringLiteral("compile_flags.txt")))));
        }
    }
    process_->start(serverPath_, args);

    // --- LSP initialize request -----------------------------------------
    // Declare the capabilities the client supports: full-text document
    // synchronization and completion (clangd will push items on demand).
    QJsonObject completion;
    completion.insert("triggerCharacters",
                      QJsonArray{".", ":", ">", "<", "\""});
    completion.insert("resolveProvider", false);

    QJsonObject synchronization;
    synchronization.insert("dynamicRegistration", false);

    QJsonObject textDocument;
    textDocument.insert("synchronization", synchronization);
    textDocument.insert("completion", completion);

    QJsonObject capabilities;
    capabilities.insert("textDocument", textDocument);

    QJsonObject params;
    params.insert("processId", QJsonValue::Null);
    params.insert("rootUri", toFileUri(rootPath_));
    params.insert("capabilities", capabilities);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    // Remembered rather than assumed: after a restart the counter has moved on,
    // and matching the response against a hard-coded 1 would leave the client
    // permanently un-initialized (no didOpen, so no diagnostics ever again).
    initializeRequestId_ = nextRequestId_++;
    request.insert("id", initializeRequestId_);
    request.insert("method", "initialize");
    request.insert("params", params);
    sendJsonRpc(request);
}

void LanguageServerClient::stop()
{
    if (process_ == nullptr) {
        return;
    }
    initialized_ = false;
    // Nobody is going to answer a queued request now: dropped, not left to be
    // run by whatever handshake happens next.
    readyGate_.drop();

    // Disconnect all our signal handlers first. waitForFinished() spins
    // a local event loop, which can deliver queued signals (finished,
    // aboutToClose, readyReadStandardOutput) to the lambdas below; those
    // lambdas used to access `process_` which we are about to null out.
    // disconnect(this) breaks every signal→this connection owned by the
    // process, so no lambda can fire while we tear the process down.
    process_->disconnect(this);

    // LSP shutdown: request "shutdown", then notification "exit".
    requestShutdown();

    QJsonObject exitNotification;
    exitNotification.insert("jsonrpc", "2.0");
    exitNotification.insert("method", "exit");
    sendJsonRpc(exitNotification);

    QProcess* proc = process_;
    process_ = nullptr;  // detach so the finished() lambda (if it slips in
                         // despite the disconnect) is a no-op via sender()
    if (!proc->waitForFinished(1500)) {
        proc->kill();
        proc->waitForFinished(500);
    }
    // The process is already gone — waitForFinished above either reaped it or
    // killed it and reaped that. deleteLater() would be the usual idiom, but
    // the deletion is only performed by the event loop, and stop() is also
    // called on the way out of the window, when there is no event loop left to
    // run it: the QProcess would then outlive its owner and Qt would abort
    // during destruction with "called object is not of the correct type".
    // Deleting directly is safe precisely because nothing can still be
    // connected to it (disconnect(this) above) and it has already exited.
    delete proc;
    documentVersions_.clear();
    completionRequests_.clear();
    definitionRequests_.clear();
    declarationRequests_.clear();
    implementationRequests_.clear();
    referencesRequests_.clear();
    signatureHelpRequests_.clear();
    documentSymbolRequests_.clear();
    renameRequests_.clear();
    codeActionRequests_.clear();
}

void LanguageServerClient::reportServerFailure(const QString& message)
{
    QProcess* proc = process_;
    process_ = nullptr;
    initialized_ = false;
    readyGate_.drop();  // the handshake this queue was waiting for just failed
    if (proc != nullptr) {
        // Mute our own slots first: readyRead would keep parsing a dead
        // handshake, and errorOccurred/finished would re-enter this
        // method for a process that is already being discarded.
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
        }
        // Never delete directly. errorOccurred(FailedToStart) arrives
        // from inside QProcess::start(), and the process must stay
        // alive until that call has returned — the event loop deletes
        // it one iteration later.
        proc->deleteLater();
    }
    // Queued on purpose: the receiver (MainWindow) reacts by unchecking
    // Syntax Check, which re-enters stop(). Running that now would
    // delete the process while Qt is still inside start()/the finished
    // slot, and would also clear the user's check state before the
    // toggle handler had a chance to finish.
    QMetaObject::invokeMethod(
        this, [this, message] { emit serverFailed(message); },
        Qt::QueuedConnection);
}

bool LanguageServerClient::isRunning() const
{
    return process_ != nullptr && process_->state() == QProcess::Running
           && initialized_;
}

bool LanguageServerClient::isStarting() const
{
    // A process that exists but is not initialized: the handshake is in flight,
    // or about to fail. Either way "start it" is the wrong answer — start()
    // returns early while process_ is set, but a caller that saw Running here
    // would think the server is usable.
    //
    // Deliberately not `state() == Running`: start() does not wait for the
    // process, so for the first event-loop turns the state is still Starting —
    // exactly the window this predicate exists for.
    return process_ != nullptr && !initialized_;
}

void LanguageServerClient::whenReady(LightCpp::Core::ReadyGate::Callback callback)
{
    readyGate_.whenReady(std::move(callback));
}

void LanguageServerClient::setToolchainHeaders(const ToolchainHeaders& headers)
{
    headers_ = headers;
}

void LanguageServerClient::requestShutdown()
{
    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", nextRequestId_++);
    request.insert("method", "shutdown");
    sendJsonRpc(request);
}

// ---------------------------------------------------------------------------
// Document synchronization
// ---------------------------------------------------------------------------

void LanguageServerClient::openDocument(const QString& filePath,
                                         const QString& languageId,
                                         const QString& text)
{
    if (process_ == nullptr || !initialized_) {
        return;
    }
    const QString uri = toFileUri(filePath);
    const int version = documentVersions_.value(uri, 0) + 1;
    documentVersions_.insert(uri, version);

    QJsonObject textDocument;
    textDocument.insert("uri", uri);
    textDocument.insert("languageId", languageId);
    textDocument.insert("version", version);
    textDocument.insert("text", text);

    QJsonObject params;
    params.insert("textDocument", textDocument);

    QJsonObject notification;
    notification.insert("jsonrpc", "2.0");
    notification.insert("method", "textDocument/didOpen");
    notification.insert("params", params);
    sendJsonRpc(notification);
}

void LanguageServerClient::changeDocument(const QString& filePath,
                                          const QString& text)
{
    if (process_ == nullptr || !initialized_) {
        return;
    }
    const QString uri = toFileUri(filePath);
    if (!documentVersions_.contains(uri)) {
        // The server requires didOpen before didChange; auto-open with a
        // language inferred from the file extension.
        openDocument(filePath, languageIdForExtension(filePath), text);
        if (!documentVersions_.contains(uri)) {
            return;  // openDocument early-returned (not running anymore)
        }
    }
    const int version = documentVersions_.value(uri) + 1;
    documentVersions_.insert(uri, version);

    QJsonObject textDocument;
    textDocument.insert("uri", uri);
    textDocument.insert("version", version);

    QJsonObject change;
    change.insert("text", text);  // full-text sync: single change, no range
    QJsonArray contentChanges;
    contentChanges.append(change);

    QJsonObject params;
    params.insert("textDocument", textDocument);
    params.insert("contentChanges", contentChanges);

    QJsonObject notification;
    notification.insert("jsonrpc", "2.0");
    notification.insert("method", "textDocument/didChange");
    notification.insert("params", params);
    sendJsonRpc(notification);
}

void LanguageServerClient::closeDocument(const QString& filePath)
{
    if (process_ == nullptr || !initialized_) {
        return;
    }
    const QString uri = toFileUri(filePath);
    if (!documentVersions_.contains(uri)) {
        return;
    }
    documentVersions_.remove(uri);

    QJsonObject textDocument;
    textDocument.insert("uri", uri);

    QJsonObject params;
    params.insert("textDocument", textDocument);

    QJsonObject notification;
    notification.insert("jsonrpc", "2.0");
    notification.insert("method", "textDocument/didClose");
    notification.insert("params", params);
    sendJsonRpc(notification);
}

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

int LanguageServerClient::requestCompletion(const QString& filePath, int line,
                                            int column,
                                            const QString& triggerChar)
{
    if (process_ == nullptr || !initialized_) {
        return 0;
    }
    const QString uri = toFileUri(filePath);
    if (!documentVersions_.contains(uri)) {
        // Document is not open on the server yet — clangd requires didOpen.
        return 0;
    }
    const int id = nextRequestId_++;
    completionRequests_.insert(id, filePath);

    QJsonObject position;
    position.insert("line", line);
    position.insert("character", column);

    QJsonObject textDocument;
    textDocument.insert("uri", uri);

    QJsonObject context;
    QJsonArray triggers;
    if (!triggerChar.isEmpty()) {
        triggers.append(triggerChar);
    }
    context.insert("triggerCharacters", triggers);
    context.insert("triggerKind", triggerChar.isEmpty() ? 1 : 2);  // 1=Invoked, 2=TriggerCharacter

    QJsonObject params;
    params.insert("textDocument", textDocument);
    params.insert("position", position);
    params.insert("context", context);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", "textDocument/completion");
    request.insert("params", params);
    sendJsonRpc(request);
    return id;
}

// ---------------------------------------------------------------------------
// Go to definition
// ---------------------------------------------------------------------------

int LanguageServerClient::requestDefinition(const QString& filePath, int line,
                                            int column)
{
    if (process_ == nullptr || !initialized_) {
        return 0;
    }
    const QString uri = toFileUri(filePath);
    if (!documentVersions_.contains(uri)) {
        // Not open on the server, so clangd has no AST to resolve against.
        return 0;
    }
    const int id = nextRequestId_++;
    definitionRequests_.insert(id, filePath);

    QJsonObject position;
    position.insert("line", line);
    position.insert("character", column);

    QJsonObject textDocument;
    textDocument.insert("uri", uri);

    QJsonObject params;
    params.insert("textDocument", textDocument);
    params.insert("position", position);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", "textDocument/definition");
    request.insert("params", params);
    sendJsonRpc(request);
    return id;
}

// ---------------------------------------------------------------------------
// Declaration / implementation / references
// ---------------------------------------------------------------------------

int LanguageServerClient::requestDeclaration(const QString& filePath,
                                             int line, int column)
{
    if (process_ == nullptr || !initialized_
        || !documentVersions_.contains(toFileUri(filePath))) {
        return 0;
    }
    const int id = nextRequestId_++;
    declarationRequests_.insert(id, filePath);

    QJsonObject position;
    position.insert("line", line);
    position.insert("character", column);
    QJsonObject textDocument;
    textDocument.insert("uri", toFileUri(filePath));
    QJsonObject params;
    params.insert("textDocument", textDocument);
    params.insert("position", position);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", "textDocument/declaration");
    request.insert("params", params);
    sendJsonRpc(request);
    return id;
}

int LanguageServerClient::requestImplementation(const QString& filePath,
                                                int line, int column)
{
    if (process_ == nullptr || !initialized_
        || !documentVersions_.contains(toFileUri(filePath))) {
        return 0;
    }
    const int id = nextRequestId_++;
    implementationRequests_.insert(id, filePath);

    QJsonObject position;
    position.insert("line", line);
    position.insert("character", column);
    QJsonObject textDocument;
    textDocument.insert("uri", toFileUri(filePath));
    QJsonObject params;
    params.insert("textDocument", textDocument);
    params.insert("position", position);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", "textDocument/implementation");
    request.insert("params", params);
    sendJsonRpc(request);
    return id;
}

int LanguageServerClient::requestReferences(const QString& filePath, int line,
                                            int column,
                                            bool includeDeclaration)
{
    if (process_ == nullptr || !initialized_
        || !documentVersions_.contains(toFileUri(filePath))) {
        return 0;
    }
    const int id = nextRequestId_++;
    referencesRequests_.insert(id, filePath);

    QJsonObject position;
    position.insert("line", line);
    position.insert("character", column);
    QJsonObject textDocument;
    textDocument.insert("uri", toFileUri(filePath));
    QJsonObject context;
    context.insert("includeDeclaration", includeDeclaration);
    QJsonObject params;
    params.insert("textDocument", textDocument);
    params.insert("position", position);
    params.insert("context", context);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", "textDocument/references");
    request.insert("params", params);
    sendJsonRpc(request);
    return id;
}

int LanguageServerClient::requestSignatureHelp(const QString& filePath,
                                               int line, int column)
{
    if (process_ == nullptr || !initialized_
        || !documentVersions_.contains(toFileUri(filePath))) {
        return 0;
    }
    const int id = nextRequestId_++;
    signatureHelpRequests_.insert(id, filePath);

    QJsonObject position;
    position.insert("line", line);
    position.insert("character", column);
    QJsonObject textDocument;
    textDocument.insert("uri", toFileUri(filePath));
    QJsonObject params;
    params.insert("textDocument", textDocument);
    params.insert("position", position);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", "textDocument/signatureHelp");
    request.insert("params", params);
    sendJsonRpc(request);
    return id;
}

int LanguageServerClient::requestDocumentSymbols(const QString& filePath)
{
    if (process_ == nullptr || !initialized_
        || !documentVersions_.contains(toFileUri(filePath))) {
        return 0;
    }
    const int id = nextRequestId_++;
    documentSymbolRequests_.insert(id, filePath);

    QJsonObject textDocument;
    textDocument.insert("uri", toFileUri(filePath));
    QJsonObject params;
    params.insert("textDocument", textDocument);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", "textDocument/documentSymbol");
    request.insert("params", params);
    sendJsonRpc(request);
    return id;
}

int LanguageServerClient::requestRename(const QString& filePath, int line,
                                        int column, const QString& newName)
{
    if (process_ == nullptr || !initialized_
        || !documentVersions_.contains(toFileUri(filePath))
        || newName.isEmpty()) {
        return 0;
    }
    const int id = nextRequestId_++;
    renameRequests_.insert(id, 0);

    QJsonObject textDocument;
    textDocument.insert("uri", toFileUri(filePath));
    QJsonObject position;
    position.insert("line", line);
    position.insert("character", column);
    QJsonObject params;
    params.insert("textDocument", textDocument);
    params.insert("position", position);
    params.insert("newName", newName);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", "textDocument/rename");
    request.insert("params", params);
    sendJsonRpc(request);
    return id;
}

int LanguageServerClient::requestCodeActions(
    const QString& filePath, int line, int column,
    const QList<LspDiagnostic>& diagnostics)
{
    if (process_ == nullptr || !initialized_
        || !documentVersions_.contains(toFileUri(filePath))) {
        return 0;
    }
    const int id = nextRequestId_++;
    codeActionRequests_.insert(id, 0);

    QJsonObject textDocument;
    textDocument.insert("uri", toFileUri(filePath));
    QJsonObject start;
    start.insert("line", line);
    start.insert("character", column);
    QJsonObject range;
    range.insert("start", start);
    range.insert("end", start);  // degenerate range: caret position only
    QJsonArray contextDiagnostics;
    for (const auto& d : diagnostics) {
        QJsonObject djStart;
        djStart.insert("line", d.startLine);
        djStart.insert("character", d.startColumn);
        QJsonObject djEnd;
        djEnd.insert("line", d.endLine);
        djEnd.insert("character", d.endColumn);
        QJsonObject djRange;
        djRange.insert("start", djStart);
        djRange.insert("end", djEnd);
        QJsonObject dj;
        dj.insert("range", djRange);
        dj.insert("message", d.message);
        dj.insert("severity", d.severity);
        contextDiagnostics.append(dj);
    }
    QJsonObject context;
    context.insert("diagnostics", contextDiagnostics);
    QJsonObject params;
    params.insert("textDocument", textDocument);
    params.insert("range", range);
    params.insert("context", context);

    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", "textDocument/codeAction");
    request.insert("params", params);
    sendJsonRpc(request);
    return id;
}

// ---------------------------------------------------------------------------
// URI helpers
// ---------------------------------------------------------------------------

QString LanguageServerClient::toFileUri(const QString& filePath)
{
    return QUrl::fromLocalFile(QFileInfo(filePath).absoluteFilePath()).toString();
}

QString LanguageServerClient::fromFileUri(const QString& uri)
{
    return QUrl(uri).toLocalFile();
}

// ---------------------------------------------------------------------------
// Wire protocol
// ---------------------------------------------------------------------------

void LanguageServerClient::sendJsonRpc(const QJsonObject& message)
{
    if (process_ == nullptr || process_->state() != QProcess::Running) {
        return;
    }
    const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
    const QByteArray frame = "Content-Length: "
                             + QByteArray::number(json.size()) + "\r\n"
                             + "\r\n" + json;
    process_->write(frame);
}

void LanguageServerClient::onReadyRead()
{
    if (process_ == nullptr) {
        return;
    }
    buffer_.append(process_->readAllStandardOutput());
    tryParseFrames();
}

void LanguageServerClient::tryParseFrames()
{
    // Each frame: "Content-Length: N\r\n[optional headers]\r\n\r\n{json}".
    forever {
        const int headerEnd = buffer_.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;  // incomplete header; wait for more data
        }
        const QByteArray headerBlock = buffer_.left(headerEnd);
        int contentLength = -1;
        for (const QByteArray& line : headerBlock.split('\n')) {
            const QByteArray l = line.trimmed();
            // Qt 6 QByteArray::startsWith has no CaseInsensitive form;
            // compare against a lowercased copy instead.
            const QByteArray lower = l.toLower();
            if (lower.startsWith("content-length:")) {
                const QByteArray value = l.mid(int(strlen("Content-Length:"))).trimmed();
                bool ok = false;
                contentLength = value.toInt(&ok);
                if (!ok) contentLength = -1;
            }
        }
        if (contentLength < 0) {
            // Corrupt header — drop the frame header and try to resync.
            buffer_.remove(0, headerEnd + 4);
            continue;
        }
        const int bodyStart = headerEnd + 4;
        if (buffer_.size() - bodyStart < contentLength) {
            return;  // incomplete body; wait for more data
        }
        const QByteArray body = buffer_.mid(bodyStart, contentLength);
        buffer_.remove(0, bodyStart + contentLength);

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            handleMessage(doc.object());
        }
    }
}

void LanguageServerClient::handleMessage(const QJsonObject& message)
{
    const QString method = message.value("method").toString();

    // Response to a request (no "method", has "id")?
    if (message.contains("id") && !message.contains("method")) {
        const QJsonValue idVal = message.value("id");
        const int id = idVal.toInt();
        if (id == initializeRequestId_ && !initialized_) {
            if (message.contains("error")) {
                // The server rejected the handshake (unknown options,
                // version mismatch...). Pretending this was success left a
                // running-but-dead server: no capabilities, every request
                // silently unanswered. Report it and drop the process —
                // reportServerFailure kills, detaches and queues the
                // notification, so nothing here may touch process_ again
                // (the handler synchronously calls stop()).
                const QJsonObject err = message.value("error").toObject();
                reportServerFailure(tr("language server rejected initialize: %1")
                                        .arg(err.value("message").toString()));
                return;
            }
            // initialize response — complete the handshake.
            initialized_ = true;
            everInitialized_ = true;
            QJsonObject initialized;
            initialized.insert("jsonrpc", "2.0");
            initialized.insert("method", "initialized");
            initialized.insert("params", QJsonObject());
            sendJsonRpc(initialized);
            emit serverStarted();
            // Serve the callers that were waiting instead of polling: they run
            // after the ordinary "started" bookkeeping (this signal opens the
            // documents already in tabs), so a queued request sees a server
            // that has been told about the buffer it is asking about.
            readyGate_.open();
            return;
        }
        if (completionRequests_.contains(id)) {
            const QString filePath = completionRequests_.take(id);
            QList<LspCompletionItem> items;
            QJsonArray rawItems;
            const QJsonValue result = message.value("result");
            if (result.isArray()) {
                rawItems = result.toArray();
            } else if (result.isObject()) {
                rawItems = result.toObject().value("items").toArray();
            }
            // clangd echoes the same candidate several times in
            // preprocessor contexts (ifdef/include arrive twice and more).
            // The label+kind+detail key folds those together without
            // collapsing distinct overloads (their prototypes differ).
            QSet<QString> seenItems;
            for (const QJsonValue& v : std::as_const(rawItems)) {
                const QJsonObject item = v.toObject();
                LspCompletionItem ci;
                ci.label = item.value("label").toString();
                ci.detail = item.value("detail").toString();
                ci.kind = item.value("kind").toInt();
                const QString dedupKey = ci.label + QLatin1Char('\x1F')
                                         + QString::number(ci.kind)
                                         + QLatin1Char('\x1F') + ci.detail;
                if (!seenItems.contains(dedupKey)) {
                    seenItems.insert(dedupKey);
                } else {
                    continue;
                }
                const QJsonValue doc = item.value("documentation");
                if (doc.isString()) {
                    ci.documentation = doc.toString();
                } else if (doc.isObject()) {
                    ci.documentation = doc.toObject().value("value").toString();
                }
                ci.insertText = item.value("insertText").toString();
                if (ci.insertText.isEmpty()) {
                    ci.insertText = ci.label;
                }
                const QJsonObject textEdit = item.value("textEdit").toObject();
                if (!textEdit.isEmpty()) {
                    const QString newText = textEdit.value("newText").toString();
                    if (!newText.isEmpty()) {
                        ci.insertText = newText;
                    }
                    // Keep the replace range: the editor must swap out the
                    // exact span the server chose, not guess the prefix.
                    const QJsonObject range =
                        textEdit.value("range").toObject();
                    if (!range.isEmpty()) {
                        const QJsonObject start =
                            range.value("start").toObject();
                        const QJsonObject end = range.value("end").toObject();
                        ci.hasTextEditRange = true;
                        ci.rangeStartLine =
                            start.value("line").toInt();
                        ci.rangeStartColumn =
                            start.value("character").toInt();
                        ci.rangeEndLine = end.value("line").toInt();
                        ci.rangeEndColumn =
                            end.value("character").toInt();
                    }
                }
                items.append(ci);
            }
            emit completionReady(id, filePath, items);
            return;
        }
        if (definitionRequests_.contains(id)) {
            const QString filePath = definitionRequests_.take(id);
            // The result is Location | Location[] | LocationLink[] | null.
            // One destination is all the IDE can act on, so the first entry
            // wins; anything unparseable stays invalid ("not found").
            LspLocation location;
            const QJsonValue result = message.value("result");
            if (result.isObject()) {
                parseLocation(result.toObject(), location);
            } else if (result.isArray()) {
                const QJsonArray arr = result.toArray();
                for (const QJsonValue& v : arr) {
                    if (parseLocation(v.toObject(), location)) {
                        break;
                    }
                }
            }
            emit definitionReady(id, filePath, location);
            return;
        }
        const QString* locationOwner = nullptr;
        if (declarationRequests_.contains(id)) {
            locationOwner = &declarationRequests_[id];
        } else if (implementationRequests_.contains(id)) {
            locationOwner = &implementationRequests_[id];
        } else if (referencesRequests_.contains(id)) {
            locationOwner = &referencesRequests_[id];
        }
        if (locationOwner != nullptr) {
            const QString filePath = *locationOwner;
            declarationRequests_.remove(id);
            implementationRequests_.remove(id);
            referencesRequests_.remove(id);
            // Declaration/implementation: Location | Location[] | LocationLink[]
            // | null. References: Location[] | null.
            QList<LspLocation> locations;
            const QJsonValue result = message.value("result");
            if (result.isObject()) {
                LspLocation location;
                if (parseLocation(result.toObject(), location)) {
                    locations.append(location);
                }
            } else if (result.isArray()) {
                const QJsonArray arr = result.toArray();
                locations.reserve(arr.size());
                for (const QJsonValue& v : arr) {
                    LspLocation location;
                    if (parseLocation(v.toObject(), location)) {
                        locations.append(location);
                    }
                }
            }
            emit locationsReady(id, filePath, locations);
            return;
        }
        if (signatureHelpRequests_.contains(id)) {
            signatureHelpRequests_.remove(id);
            // result: SignatureHelp | null. Only the ACTIVE signature is
            // shown; its activeParameter maps into the label if clangd
            // names the range as [start,end] (it does; a plain string is
            // the fallback and gets a -1 range = no bolding).
            LspSignatureHelp help;
            const QJsonObject result = message.value("result").toObject();
            const QJsonArray signatures =
                result.value("signatures").toArray();
            const int activeSignature =
                result.value("activeSignature").toInt(0);
            if (activeSignature >= 0
                && activeSignature < signatures.size()) {
                const QJsonObject signature =
                    signatures.at(activeSignature).toObject();
                help.label = signature.value("label").toString();
                const int activeParameter =
                    result.value("activeParameter").toInt(0);
                const QJsonArray parameters =
                    signature.value("parameters").toArray();
                if (activeParameter >= 0
                    && activeParameter < parameters.size()) {
                    const QJsonValue labelValue =
                        parameters.at(activeParameter).toObject()
                            .value("label");
                    if (labelValue.isArray()) {
                        help.paramStart =
                            labelValue.toArray().at(0).toInt(-1);
                        help.paramEnd =
                            labelValue.toArray().at(1).toInt(-1);
                    } else if (labelValue.isString()) {
                        help.paramStart = help.label.indexOf(
                            labelValue.toString());
                        if (help.paramStart >= 0) {
                            help.paramEnd =
                                help.paramStart
                                + labelValue.toString().size();
                        }
                    }
                }
            }
            emit signatureHelpReady(id, help);
            return;
        }
        if (documentSymbolRequests_.contains(id)) {
            const QString filePath = documentSymbolRequests_.take(id);
            // result: DocumentSymbol[] (hierarchical) | SymbolInformation[]
            // (flat) | null — both parsed into LspSymbol trees.
            QList<LspSymbol> symbols;
            const QJsonValue result = message.value("result");
            if (result.isArray()) {
                const QJsonArray arr = result.toArray();
                for (const QJsonValue& v : arr) {
                    LspSymbol symbol;
                    const QJsonObject obj = v.toObject();
                    // DocumentSymbol has `range`; SymbolInformation has
                    // `location`. Probe both, prefer the hierarchical shape.
                    if (obj.contains("range")
                        ? parseDocumentSymbol(obj, symbol)
                        : parseSymbolInformation(obj, symbol)) {
                        symbols.append(symbol);
                    }
                }
            }
            emit documentSymbolsReady(id, filePath, symbols);
            return;
        }
        if (renameRequests_.contains(id)) {
            renameRequests_.remove(id);
            // result: WorkspaceEdit | null (null = nothing renameable).
            emit renameReady(id, parseWorkspaceEdit(message.value("result")));
            return;
        }
        if (codeActionRequests_.contains(id)) {
            codeActionRequests_.remove(id);
            // result: (Command | CodeAction)[] | null. Commands carry no
            // edit; only actions with a workspace edit are surfaced.
            QList<LspCodeAction> actions;
            for (const QJsonValue& v :
                 message.value("result").toArray()) {
                const QJsonObject obj = v.toObject();
                LspCodeAction action;
                action.title = obj.value("title").toString();
                action.kind = obj.value("kind").toString();
                if (action.title.isEmpty()) {
                    continue;
                }
                action.edits = parseWorkspaceEdit(obj.value("edit"));
                actions.append(action);
            }
            emit codeActionsReady(id, actions);
            return;
        }
        return;
    }

    if (method == "textDocument/publishDiagnostics") {
        handlePublishDiagnostics(message.value("params").toObject());
        return;
    }
    if (method == "window/logMessage" || method == "window/showMessage") {
        // Server-side chatter: surface it in the clangd log channel.
        const QJsonObject params = message.value("params").toObject();
        const QString text = params.value("message").toString().trimmed();
        if (!text.isEmpty()) {
            QString prefix;
            switch (params.value("type").toInt(4)) {
            case 1: prefix = QStringLiteral("[error] "); break;
            case 2: prefix = QStringLiteral("[warn] "); break;
            case 3: prefix = QStringLiteral("[info] "); break;
            default: break;
            }
            emit serverLog(prefix + text);
        }
        return;
    }
    // Other notifications ($/progress, telemetry, ...) are ignored — this
    // client only cares about diagnostics, completion, go-to-definition
    // and the server log.
}

void LanguageServerClient::handlePublishDiagnostics(const QJsonObject& params)
{
    const QString uri = params.value("uri").toString();
    if (uri.isEmpty()) {
        return;
    }
    const QString filePath = fromFileUri(uri);

    QList<LspDiagnostic> diagnostics;
    const QJsonArray rawDiags = params.value("diagnostics").toArray();
    for (const QJsonValue& v : rawDiags) {
        const QJsonObject d = v.toObject();
        LspDiagnostic diag;
        const LspRange range = parseRange(d.value("range").toObject());
        diag.startLine = range.startLine;
        diag.startColumn = range.startColumn;
        diag.endLine = range.endLine;
        diag.endColumn = range.endColumn;
        diag.severity = d.value("severity").toInt(1);
        diag.message = d.value("message").toString();
        diag.code = d.value("code").toVariant().toString();
        diagnostics.append(diag);
    }
    emit diagnosticsReady(filePath, diagnostics);
}

}  // namespace LightCpp::App
