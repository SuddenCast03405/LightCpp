// LibClangCompleter — implementation. See the header for the contract.

#include "LightCpp/App/LibClangCompleter.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The real C API declarations (CXString layout, chunk-kind enums, cursor
// kinds). Included only here; the DLL itself is loaded at run time through
// LoadLibraryExW, so the exe never links against clang — every function call
// below goes through a pointer resolved in load().
#include <clang-c/Index.h>

namespace {

using LightCpp::App::LspCompletionItem;

/// FormatMessageW text for a Win32 error, or an empty string when the system
/// has no description (loader errors are often terse but still useful).
QString windowsErrorText(unsigned long error)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text;
    if (length > 0 && buffer != nullptr) {
        text = QString::fromWCharArray(buffer, int(length)).trimmed();
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return text;
}

/// Clang's columns are 1-based and counted in UTF-8 bytes from the line
/// start, while QTextCursor columns (what the editor sends, and what LSP
/// positions use) count UTF-16 code units. A line with Chinese comments or
/// string literals makes the two diverge — passing the UTF-16 unit count
/// straight through lands the completion point to the right of the caret.
/// Convert by re-encoding the caret's line prefix as UTF-8.
unsigned byteColumnFor(const QString& content, int line, int column)
{
    // Walk to the start of `line` (0-based) without materialising a copy of
    // the whole document.
    qsizetype lineStart = 0;
    for (int l = 0; l < line; ++l) {
        const qsizetype newline = content.indexOf(u'\n', lineStart);
        if (newline < 0) {
            return unsigned(qMax(column, 0)) + 1;  // line not in content
        }
        lineStart = newline + 1;
    }
    const qsizetype newline = content.indexOf(u'\n', lineStart);
    const qsizetype lineEnd = newline < 0 ? content.size() : newline;
    qsizetype units = column;
    units = qBound<qsizetype>(qsizetype(0), units, lineEnd - lineStart);
    const QByteArray prefix =
        content.mid(lineStart, units).toUtf8();
    return unsigned(prefix.size()) + 1;
}

/// Lives on the libclang worker thread and runs the translation-unit parse.
/// A QObject (rather than a bare lambda) is what lets the queued invocation
/// carry the thread affinity with it; the completer itself outlives the
/// worker (the destructor waits for it) and is passed along by pointer.
class ParseRunner : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

public slots:
    void parse(LightCpp::App::LibClangCompleter* completer,
               const LightCpp::App::LibClangCompleter::Request& request,
               quint64 token)
    {
        QString error;
        const QList<LspCompletionItem> items =
            completer->complete(request, &error);
        emit parsed(token, items, error);
    }

signals:
    void parsed(quint64 token, const QList<LspCompletionItem>& items,
                const QString& error);
};

/// Map a CXCursorKind (the entity the completion names) to the LSP
/// CompletionItemKind the popup already knows how to render.
int lspKindFor(CXCursorKind kind)
{
    switch (kind) {
    case CXCursor_CXXMethod:            return 2;   // Method
    case CXCursor_FunctionDecl:
    case CXCursor_Destructor:
    case CXCursor_ConversionFunction:
    case CXCursor_FunctionTemplate:     return 3;   // Function
    case CXCursor_Constructor:          return 4;   // Constructor
    case CXCursor_FieldDecl:            return 5;   // Field
    case CXCursor_VarDecl:
    case CXCursor_ParmDecl:             return 6;   // Variable
    case CXCursor_StructDecl:
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:        return 7;   // Class
    case CXCursor_EnumDecl:             return 13;  // Enum
    case CXCursor_EnumConstantDecl:     return 20;  // EnumMember
    case CXCursor_Namespace:            return 9;   // Module
    case CXCursor_MacroDefinition:      return 3;   // shows like a callable
    case CXCursor_TypedefDecl:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypeRef:              return 7;   // Class
    default:                            return 1;   // Text
    }
}

}  // namespace

namespace LightCpp::App {

LibClangCompleter::LibClangCompleter(QObject* parent) : QObject(parent) {}

LibClangCompleter::~LibClangCompleter()
{
    // The worker parses whole translation units and can sit in a blocking read
    // when the disk is loaded, so it is waited for rather than merely asked to
    // stop — leaving it running would call into a half-destroyed completer.
    if (worker_ != nullptr) {
        {
            QMutexLocker locker(&mutex_);
            queue_.clear();
        }
        worker_->quit();
        worker_->wait();
        delete worker_;
        worker_ = nullptr;
    }
    // index_ belongs to the worker thread and is disposed with it: clang's
    // global state must not be torn down on a different thread than the one
    // that created it.
    index_ = nullptr;
    // module_ is deliberately never freed: unloading libclang.dll crashes
    // the process, and the completer lives as long as the IDE anyway.
}

bool LibClangCompleter::load(const QString& libraryPath)
{
    if (usable_) {
        return true;  // one engine per process; the DLL is never swapped
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH is the load that makes a DLL directory
    // self-contained: libclang.dll's siblings (libLLVM-*.dll, libclang*.dll,
    // the MinGW runtime) are looked up next to libclang.dll rather than only
    // in the executable's directory and PATH. Plain LoadLibrary cannot find
    // them when the IDE is started from Explorer and the toolchain is not on
    // PATH.
    const QString nativePath = QDir::toNativeSeparators(libraryPath);
    module_ = reinterpret_cast<void*>(LoadLibraryExW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr,
        LOAD_WITH_ALTERED_SEARCH_PATH));
    if (module_ == nullptr) {
        const unsigned long error = GetLastError();
        emit logMessage(tr("libclang: cannot load %1: %2 (error %3)")
                            .arg(nativePath, windowsErrorText(error))
                            .arg(error));
        return false;
    }
    libPath_ = libraryPath;

    const HMODULE module = static_cast<HMODULE>(module_);
    const auto resolve = [module](const char* name) -> void (*)() {
        return reinterpret_cast<void (*)()>(GetProcAddress(module, name));
    };

    createIndex_ = reinterpret_cast<decltype(createIndex_)>(
        resolve("clang_createIndex"));
    disposeIndex_ = reinterpret_cast<decltype(disposeIndex_)>(
        resolve("clang_disposeIndex"));
    parseTU2_ = reinterpret_cast<decltype(parseTU2_)>(
        resolve("clang_parseTranslationUnit2"));
    disposeTU_ = reinterpret_cast<decltype(disposeTU_)>(
        resolve("clang_disposeTranslationUnit"));
    codeCompleteAt_ = reinterpret_cast<decltype(codeCompleteAt_)>(
        resolve("clang_codeCompleteAt"));
    disposeCodeCompleteResults_ = reinterpret_cast<decltype(disposeCodeCompleteResults_)>(
        resolve("clang_disposeCodeCompleteResults"));
    getNumCompletionChunks_ = reinterpret_cast<decltype(getNumCompletionChunks_)>(
        resolve("clang_getNumCompletionChunks"));
    getCompletionChunkKind_ = reinterpret_cast<decltype(getCompletionChunkKind_)>(
        resolve("clang_getCompletionChunkKind"));
    getCompletionChunkText_ = reinterpret_cast<decltype(getCompletionChunkText_)>(
        resolve("clang_getCompletionChunkText"));
    getCompletionAvailability_ = reinterpret_cast<decltype(getCompletionAvailability_)>(
        resolve("clang_getCompletionAvailability"));
    getCompletionBriefComment_ = reinterpret_cast<decltype(getCompletionBriefComment_)>(
        resolve("clang_getCompletionBriefComment"));
    getCString_ = reinterpret_cast<decltype(getCString_)>(
        resolve("clang_getCString"));
    disposeString_ = reinterpret_cast<decltype(disposeString_)>(
        resolve("clang_disposeString"));
    getClangVersion_ = reinterpret_cast<decltype(getClangVersion_)>(
        resolve("clang_getClangVersion"));

    static constexpr const char* kRequired[] = {
        "clang_createIndex",            "clang_disposeIndex",
        "clang_parseTranslationUnit2",  "clang_disposeTranslationUnit",
        "clang_codeCompleteAt",         "clang_disposeCodeCompleteResults",
        "clang_getNumCompletionChunks", "clang_getCompletionChunkKind",
        "clang_getCompletionChunkText", "clang_getCString",
        "clang_disposeString",
    };
    for (const char* name : kRequired) {
        if (resolve(name) == nullptr) {
            emit logMessage(tr("libclang: %1 is missing export %2")
                                .arg(nativePath, QString::fromLatin1(name)));
            // Keep the DLL resident (unloading it would crash) but mark the
            // completer unusable, so callers fall back to the language server.
            return false;
        }
    }

    if (index_ == nullptr && createIndex_ != nullptr) {
        // excludeDeclarationsFromPCH=0, displayDiagnostics=0 — the completer
        // never surfaces parse diagnostics of its own.
        QMutexLocker locker(&mutex_);
        if (index_ == nullptr) {
            index_ = createIndex_(0, 0);
        }
    }
    if (index_ == nullptr) {
        emit logMessage(tr("libclang: could not create the completion index"));
        return false;
    }
    usable_ = true;
    emit logMessage(tr("libclang loaded: %1")
                        .arg(QDir::toNativeSeparators(libraryPath)));
    return true;
}

QString LibClangCompleter::version() const
{
    if (getClangVersion_ == nullptr || getCString_ == nullptr
        || disposeString_ == nullptr) {
        return QString();
    }
    ClangString v = getClangVersion_();
    const QString text = QString::fromUtf8(getCString_(v));
    disposeString_(&v);
    return text;
}

// ---------------------------------------------------------------------------
// Asynchronous completion
//
// clang answers a request by parsing the entire translation unit, which reads
// every included header off disk. Doing that on the GUI thread is what froze
// the editor while the disk was busy: a parse that takes 40ms on an idle
// machine takes seconds when the disk is saturated, and the UI cannot repaint
// until it returns. The parse therefore runs on a dedicated worker thread and
// the answer comes back through completed().
// ---------------------------------------------------------------------------
void LibClangCompleter::completeAsync(Request request, quint64 token)
{
    if (!isLoaded()) {
        emit completed(token, {}, tr("libclang is not loaded"));
        return;
    }
    {
        QMutexLocker locker(&mutex_);
        // Keep the queue short: a burst of keystrokes only cares about the last
        // position, and parsing the stale ones would just delay it.
        if (queue_.size() >= 4) {
            queue_.dequeue();
        }
        queue_.enqueue(AsyncRequest{std::move(request), token});
    }
    ensureWorker();
    // A direct call when the worker is idle: there is nothing to wait for and
    // the answer should not cost a thread round-trip on the common path.
    QMetaObject::invokeMethod(this, &LibClangCompleter::drainQueue,
                              Qt::QueuedConnection);
}

bool LibClangCompleter::isBusy() const
{
    QMutexLocker locker(&mutex_);
    return parseRunning_ || !queue_.isEmpty();
}

void LibClangCompleter::ensureWorker()
{
    if (worker_ != nullptr) {
        return;
    }
    worker_ = new QThread(this);
    worker_->setObjectName(QStringLiteral("lightcpp-libclang"));

    // A QObject whose thread affinity IS the worker, so the queued parse
    // below really executes off the GUI thread.
    auto* runner = new ParseRunner;
    runner->moveToThread(worker_);
    connect(this, &LibClangCompleter::parseRequested, runner,
            &ParseRunner::parse, Qt::QueuedConnection);
    connect(runner, &ParseRunner::parsed, this,
            &LibClangCompleter::onParseFinished, Qt::QueuedConnection);
    connect(worker_, &QThread::finished, runner, &QObject::deleteLater);
    connect(worker_, &QThread::finished, this, [this] {
        QMutexLocker locker(&mutex_);
        // The index died with the thread; a later start must make a new one.
        index_ = nullptr;
    });
    worker_->start();
}

void LibClangCompleter::drainQueue()
{
    AsyncRequest job;
    {
        QMutexLocker locker(&mutex_);
        if (parseRunning_ || queue_.isEmpty()) {
            return;
        }
        job = queue_.dequeue();
        parseRunning_ = true;
    }
    // Queued, not direct: ParseRunner lives on the worker thread.
    emit parseRequested(this, job.request, job.token);
}

void LibClangCompleter::onParseFinished(quint64 token,
                                        const QList<LspCompletionItem>& items,
                                        const QString& error)
{
    {
        QMutexLocker locker(&mutex_);
        parseRunning_ = false;
    }
    emit completed(token, items, error);
    // Whatever queued up while this one ran goes next.
    drainQueue();
}

QList<LspCompletionItem> LibClangCompleter::complete(const Request& request,
                                                     QString* errorOut)
{
    QList<LspCompletionItem> items;
    const auto fail = [errorOut, &items](const QString& message) {
        if (errorOut != nullptr) {
            *errorOut = message;
        }
        return items;
    };

    if (!isLoaded() || index_ == nullptr || parseTU2_ == nullptr
        || codeCompleteAt_ == nullptr || disposeTU_ == nullptr
        || disposeCodeCompleteResults_ == nullptr) {
        return fail(tr("libclang is not loaded"));
    }

    // clang wants UTF-8 everywhere; the filename drives language detection,
    // so unsaved buffers keep their virtual ".cpp" lint path.
    const QByteArray fileNameUtf8 = QFile::encodeName(
        QFileInfo(request.filePath).absoluteFilePath());
    const QByteArray contentUtf8 = request.content.toUtf8();

    CXUnsavedFile unsaved;
    unsaved.Filename = fileNameUtf8.constData();
    unsaved.Contents = contentUtf8.constData();
    unsaved.Length = static_cast<unsigned long>(contentUtf8.size());

    // Translate the compiler args to UTF-8 once; they stay alive until the
    // parse below returns.
    QList<QByteArray> argBytes;
    for (const QString& arg : std::as_const(compilerArgs_)) {
        argBytes.append(arg.toUtf8());
    }
    QList<const char*> argv;
    argv.reserve(argBytes.size());
    for (const QByteArray& bytes : argBytes) {
        argv.append(bytes.constData());
    }

    void* tu = nullptr;
    const int parseError = parseTU2_(
        index_, fileNameUtf8.constData(), argv.data(), int(argv.size()),
        &unsaved, 1, CXTranslationUnit_None, &tu);
    if (parseError != CXError_Success || tu == nullptr) {
        return fail(tr("libclang: parse failed with error %1").arg(parseError));
    }

    // 1-based line/column for clang; the column is byte-based (see
    // byteColumnFor above — the request carries UTF-16 units).
    const unsigned options = CXCodeComplete_IncludeBriefComments;
    CXCodeCompleteResults* results = static_cast<CXCodeCompleteResults*>(codeCompleteAt_(
        tu, fileNameUtf8.constData(), unsigned(request.line + 1),
        byteColumnFor(request.content, request.line, request.column),
        &unsaved, 1, options));
    if (results == nullptr) {
        disposeTU_(tu);
        return fail(tr("libclang: completion failed"));
    }

    const auto chunkText = [this](void* completionString, unsigned chunk) {
        ClangString cx = getCompletionChunkText_(
            static_cast<CXCompletionString>(completionString), chunk);
        const QString text = QString::fromUtf8(getCString_(cx));
        disposeString_(&cx);
        return text;
    };

    items.reserve(int(results->NumResults));
    // libclang hands over its raw candidate list, which contains literal
    // duplicates in preprocessor contexts (the same directive once as a
    // directive and once as a macro-like entry). Real overloads differ in
    // their prototype chunk, so "label + prototype" dedupes the noise
    // without collapsing distinct overloads.
    QSet<QString> seen;
    for (unsigned i = 0; i < results->NumResults; ++i) {
        const CXCompletionResult& result = results->Results[i];
        void* cs = result.CompletionString;
        if (cs == nullptr
            || getCompletionAvailability_(cs) < CXAvailability_Available) {
            continue;
        }
        const unsigned numChunks = getNumCompletionChunks_(cs);

        // TypedText chunk = what gets inserted; the remaining chunks make
        // the prototype shown in the popup's detail column. Result types and
        // optional-parameter groups are left out of both.
        QString typedText;
        QString prototype;
        for (unsigned c = 0; c < numChunks; ++c) {
            const CXCompletionChunkKind kind = static_cast<CXCompletionChunkKind>(
                getCompletionChunkKind_(
                    static_cast<CXCompletionString>(cs), c));
            if (kind == CXCompletionChunk_Optional
                || kind == CXCompletionChunk_ResultType) {
                if (kind == CXCompletionChunk_ResultType) {
                    prototype += chunkText(cs, c) + QLatin1Char(' ');
                }
                continue;
            }
            const QString text = chunkText(cs, c);
            if (kind == CXCompletionChunk_TypedText) {
                typedText = text;
            } else if (!text.isEmpty()) {
                prototype += text;
            }
        }
        if (typedText.isEmpty()) {
            continue;
        }

        LspCompletionItem item;
        item.label = typedText;
        item.insertText = typedText;  // no parens — the editor adds "()" itself
        item.detail = prototype.trimmed();
        item.kind = lspKindFor(result.CursorKind);
        if (getCompletionBriefComment_ != nullptr) {
            ClangString brief = getCompletionBriefComment_(
                static_cast<CXCompletionString>(cs));
            item.documentation = QString::fromUtf8(getCString_(brief));
            disposeString_(&brief);
        }
        // Preprocessor candidates come twice — `include "header"` and
        // `include <header>` — with an identical TypedText, so the popup
        // shows the same entry two times (bug report 2026-09-29). Folding
        // the quote/bracket placeholders out of the detail makes those two
        // share one key, while real overloads keep their distinct
        // parameter lists.
        QString normalizedDetail = item.detail;
        normalizedDetail.remove(QLatin1Char('"'));
        normalizedDetail.remove(QLatin1Char('<'));
        normalizedDetail.remove(QLatin1Char('>'));
        normalizedDetail.remove(QLatin1Char(' '));
        const QString dedupKey =
            item.label + QLatin1Char('\x1F') + normalizedDetail;
        if (!seen.contains(dedupKey)) {
            seen.insert(dedupKey);
            items.append(item);
        }
    }

    disposeCodeCompleteResults_(results);
    disposeTU_(tu);
    return items;
}

}  // namespace LightCpp::App

// moc for the ParseRunner helper defined above (AUTOMOC).
#include "LibClangCompleter.moc"
