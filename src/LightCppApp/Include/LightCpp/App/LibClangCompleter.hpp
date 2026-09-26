// LibClangCompleter — code completion through libclang's C API, loaded at
// run time (QLibrary) so the IDE has no link-time dependency on clang.
//
// libclang answers a completion request by parsing the buffer in-process:
// slower than clangd for repeated queries (no persistent index) but it needs
// nothing but the DLL itself, which makes it the fallback engine when no
// language server is installed. One translation unit per request keeps the
// memory profile flat; files above the large-file limits should not be routed
// here.

#pragma once

#include <QLibrary>
#include <QObject>
#include <QStringList>

#include "LightCpp/App/LanguageServerClient.hpp"

namespace LightCpp::App {

class LibClangCompleter : public QObject {
    Q_OBJECT
public:
    explicit LibClangCompleter(QObject* parent = nullptr);
    ~LibClangCompleter() override;

    /// Load `libraryPath` (e.g. ...\libclang.dll) and resolve the C API entry
    /// points this completer uses. Returns false and logs the reason when the
    /// library cannot be loaded or is missing a required export. Once loaded,
    /// the DLL stays resident for the process lifetime (see lib_).
    bool load(const QString& libraryPath);
    [[nodiscard]] bool isLoaded() const { return usable_; }
    [[nodiscard]] const QString& libraryPath() const { return libPath_; }

    /// Extra compiler arguments the parse step should see (-isystem, -std,
    /// -target ...). Same shape as a compile_flags.txt, one argument per item.
    void setCompilerArgs(const QStringList& args) { compilerArgs_ = args; }
    [[nodiscard]] const QStringList& compilerArgs() const { return compilerArgs_; }

    /// One completion request.
    struct Request {
        /// The path the buffer belongs to (drives language detection and
        /// include resolution). Unsaved buffers use their virtual lint path.
        QString filePath;
        /// The full buffer text.
        QString content;
        /// 0-based caret line and column, as used by LSP.
        int line = 0;
        int column = 0;
    };

    /// Synchronous completion. Returns the items clang produced; an empty
    /// list with a non-empty `*errorOut` means the request failed (parse
    /// error, library trouble) rather than "no suggestions".
    QList<LspCompletionItem> complete(const Request& request,
                                      QString* errorOut = nullptr);

    /// libclang's own version string, for the language-server log.
    [[nodiscard]] QString version() const;

signals:
    /// Diagnostic chatter from the completer (load failures, parse retries).
    void logMessage(const QString& message);

private:
    /// Layout-compatible stand-in for clang's CXString (a pointer plus an
    /// opaque flag word). Returned by value, so the exact shape matters.
    struct ClangString {
        const void* data = nullptr;
        unsigned privateFlags = 0;
    };

    /// Deliberately leaked: libclang.dll must never be unloaded once loaded
    /// (its teardown runs global cleanup that crashes on this platform), so
    /// the QLibrary object is created detached and kept for the process
    /// lifetime. load() is effectively a once-per-process operation.
    QLibrary* lib_ = nullptr;
    QString libPath_;
    bool usable_ = false;
    QStringList compilerArgs_;
    void* index_ = nullptr;  // CXIndex, created once on load()

    // --- resolved C API entry points (signatures in clang-c/Index.h) -------
    void* (*createIndex_)(int excludeDeclarationsFromPCH, int displayDiagnostics) = nullptr;
    void (*disposeIndex_)(void*) = nullptr;
    int (*parseTU2_)(void* index, const char* sourceFilename,
                     const char* const* argv, int argc,
                     void* unsavedFiles, unsigned numUnsaved,
                     unsigned options, void** outTU) = nullptr;
    void (*disposeTU_)(void*) = nullptr;
    void* (*codeCompleteAt_)(void* tu, const char* filename, unsigned line,
                             unsigned column, void* unsavedFiles,
                             unsigned numUnsaved, unsigned options) = nullptr;
    void (*disposeCodeCompleteResults_)(void*) = nullptr;
    unsigned (*getNumCompletionChunks_)(void* completionString) = nullptr;
    int (*getCompletionChunkKind_)(void* completionString, unsigned chunk) = nullptr;
    ClangString (*getCompletionChunkText_)(void* completionString,
                                           unsigned chunk) = nullptr;
    int (*getCompletionAvailability_)(void* completionString) = nullptr;
    ClangString (*getCompletionBriefComment_)(void* completionString) = nullptr;
    ClangString (*getClangVersion_)() = nullptr;
    const char* (*getCString_)(ClangString) = nullptr;
    void (*disposeString_)(ClangString*) = nullptr;
};

}  // namespace LightCpp::App
