// LibClangCompleter — implementation. See the header for the contract.

#include "LightCpp/App/LibClangCompleter.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>

// The real C API declarations (CXString layout, chunk-kind enums, cursor
// kinds). Included only here; the DLL itself is loaded at run time through
// QLibrary, so the exe never links against clang — every function call below
// goes through a pointer resolved in load().
#include <clang-c/Index.h>

namespace {

using LightCpp::App::LspCompletionItem;

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
    // lib_ is deliberately leaked: unloading libclang.dll crashes the
    // process, and the completer lives as long as the IDE anyway.
}

bool LibClangCompleter::load(const QString& libraryPath)
{
    if (usable_) {
        return true;  // one engine per process; the DLL is never swapped
    }
    if (lib_ == nullptr) {
        lib_ = new QLibrary();  // leaked on purpose, see the header
    }
    lib_->setFileName(libraryPath);
    lib_->load();
    if (!lib_->isLoaded()) {
        emit logMessage(tr("libclang: cannot load %1: %2")
                            .arg(QDir::toNativeSeparators(libraryPath),
                                 lib_->errorString()));
        return false;
    }
    libPath_ = libraryPath;

    createIndex_ = reinterpret_cast<void* (*)(int, int)>(
        lib_->resolve("clang_createIndex"));
    disposeIndex_ = reinterpret_cast<void (*)(void*)>(
        lib_->resolve("clang_disposeIndex"));
    parseTU2_ = reinterpret_cast<int (*)(void*, const char*, const char* const*,
                                         int, void*, unsigned, unsigned, void**)>(
        lib_->resolve("clang_parseTranslationUnit2"));
    disposeTU_ = reinterpret_cast<void (*)(void*)>(
        lib_->resolve("clang_disposeTranslationUnit"));
    codeCompleteAt_ = reinterpret_cast<void* (*)(void*, const char*, unsigned,
                                                unsigned, void*, unsigned,
                                                unsigned)>(
        lib_->resolve("clang_codeCompleteAt"));
    disposeCodeCompleteResults_ = reinterpret_cast<void (*)(void*)>(
        lib_->resolve("clang_disposeCodeCompleteResults"));
    getNumCompletionChunks_ = reinterpret_cast<unsigned (*)(void*)>(
        lib_->resolve("clang_getNumCompletionChunks"));
    getCompletionChunkKind_ = reinterpret_cast<int (*)(void*, unsigned)>(
        lib_->resolve("clang_getCompletionChunkKind"));
    getCompletionChunkText_ = reinterpret_cast<ClangString (*)(void*, unsigned)>(
        lib_->resolve("clang_getCompletionChunkText"));
    getCompletionAvailability_ = reinterpret_cast<int (*)(void*)>(
        lib_->resolve("clang_getCompletionAvailability"));
    getCompletionBriefComment_ = reinterpret_cast<ClangString (*)(void*)>(
        lib_->resolve("clang_getCompletionBriefComment"));
    getCString_ = reinterpret_cast<const char* (*)(ClangString)>(
        lib_->resolve("clang_getCString"));
    disposeString_ = reinterpret_cast<void (*)(ClangString*)>(
        lib_->resolve("clang_disposeString"));
    getClangVersion_ = reinterpret_cast<ClangString (*)()>(
        lib_->resolve("clang_getClangVersion"));

    static constexpr const char* kRequired[] = {
        "clang_createIndex",            "clang_disposeIndex",
        "clang_parseTranslationUnit2",  "clang_disposeTranslationUnit",
        "clang_codeCompleteAt",         "clang_disposeCodeCompleteResults",
        "clang_getNumCompletionChunks", "clang_getCompletionChunkKind",
        "clang_getCompletionChunkText", "clang_getCString",
        "clang_disposeString",
    };
    for (const char* name : kRequired) {
        if (lib_->resolve(name) == nullptr) {
            emit logMessage(tr("libclang: %1 is missing export %2")
                                .arg(QDir::toNativeSeparators(libraryPath),
                                     QString::fromLatin1(name)));
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

    // 1-based line/column, unlike LSP.
    const unsigned options = CXCodeComplete_IncludeBriefComments;
    CXCodeCompleteResults* results = static_cast<CXCodeCompleteResults*>(codeCompleteAt_(
        tu, fileNameUtf8.constData(), unsigned(request.line + 1),
        unsigned(request.column + 1), &unsaved, 1, options));
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
