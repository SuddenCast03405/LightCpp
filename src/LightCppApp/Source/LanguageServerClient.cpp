// LanguageServerClient — implementation.

#include "LightCpp/App/LanguageServerClient.hpp"

#include <cstring>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
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
        emit serverFailed(tr("language server not found: %1").arg(serverPath_));
        return;
    }

    initialized_ = false;
    documentVersions_.clear();
    buffer_.clear();

    process_ = new QProcess(this);
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
    connect(process_, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError error) {
        if (process_ == nullptr) return;  // already being torn down
        if (error == QProcess::FailedToStart) {
            emit serverFailed(tr("failed to start language server: %1")
                                  .arg(serverPath_));
            stop();
        }
    });
    connect(process_, &QIODevice::aboutToClose, this, [this] {
        // Process ended (possibly on its own).
        if (process_ != nullptr) {
            initialized_ = false;
        }
    });
    connect(process_, &QProcess::finished, this, [this] {
        // The process ended. Use sender() so we never touch the member
        // pointer — stop() may have already nulled it (the synchronous
        // waitForFinished below can deliver finished() to this slot
        // while stop() is mid-flight, which previously crashed on the
        // process_->deleteLater() line).
        if (auto* p = qobject_cast<QProcess*>(sender())) {
            p->deleteLater();
        }
        if (process_ == sender()) {
            process_ = nullptr;
        }
        initialized_ = false;
    });

    // Light options: no background indexing, no clang-tidy checks — this is
    // a plain syntax-check integration; full compilation-database support
    // can come later.
    process_->start(serverPath_, {"--background-index=false",
                                   "--clang-tidy=false",
                                   "--pch-storage=memory"});

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
    request.insert("id", nextRequestId_++);
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
    proc->deleteLater();
    documentVersions_.clear();
}

bool LanguageServerClient::isRunning() const
{
    return process_ != nullptr && process_->state() == QProcess::Running
           && initialized_;
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
        if (id == 1 && !initialized_) {
            // initialize response — complete the handshake.
            initialized_ = true;
            QJsonObject initialized;
            initialized.insert("jsonrpc", "2.0");
            initialized.insert("method", "initialized");
            initialized.insert("params", QJsonObject());
            sendJsonRpc(initialized);
            emit serverStarted();
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
            for (const QJsonValue& v : std::as_const(rawItems)) {
                const QJsonObject item = v.toObject();
                LspCompletionItem ci;
                ci.label = item.value("label").toString();
                ci.detail = item.value("detail").toString();
                ci.kind = item.value("kind").toInt();
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
                }
                items.append(ci);
            }
            emit completionReady(id, filePath, items);
            return;
        }
        return;
    }

    if (method == "textDocument/publishDiagnostics") {
        handlePublishDiagnostics(message.value("params").toObject());
    }
    // Other notifications (window/logMessage, $/progress, ...) are ignored —
    // this client only cares about diagnostics and completion.
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
