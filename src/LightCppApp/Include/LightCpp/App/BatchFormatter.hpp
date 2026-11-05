#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class QTimer;

namespace LightCpp::App {

/// Formats a batch of files in place with clang-format.
///
/// Files are processed one at a time so progress can be reported and a file
/// with a syntax error does not abort the run. The style flags match
/// CodeFormatter — `-style=file` with the Microsoft fallback — so a
/// project's `.clang-format` wins and trees without one still get a
/// sensible shape.
///
/// One run is in flight at a time; a second call supersedes the first, just
/// like the single-file formatter. The shared QProcess is driven by its own
/// `finished` signal: each completed file pops the queue and starts the next.
class BatchFormatter : public QObject {
    Q_OBJECT
public:
    /// The file count at/above which a confirmation is asked for before a
    /// batch runs. Above this many files the reformatting is a large diff,
    /// and the user should opt in rather than have it happen silently.
    static constexpr int kWarnThreshold = 16;

    explicit BatchFormatter(QObject* parent = nullptr);
    ~BatchFormatter() override;

    /// clang-format in place, one file at a time. The answer arrives as
    /// progress()/finished(), carrying the returned id. An empty list
    /// finishes immediately with no work done. `workingDir` is where
    /// clang-format runs (so it finds a `.clang-format` at the project root).
    /// A second call supersedes the first.
    int formatFiles(const QStringList& filePaths, const QString& workingDir);

    [[nodiscard]] bool isRunning() const;

    /// Point the batch at a specific clang-format (the settings'
    /// clangFormatPath). Empty restores the compiled-in default. Must be
    /// set before formatFiles() starts a run.
    void setToolPath(const QString& path) { toolPathOverride_ = path; }
    /// Override the --fallback-style passed per file ("file"/empty keep the
    /// built-in default). Same rules as CodeFormatter.
    void setFallbackStyle(const QString& style) { fallbackStyle_ = style; }

signals:
    /// `done` files have been processed out of `total`; `currentFile` is the
    /// one just finished, and `success` says whether it was rewritten on
    /// disk (a failed file is left untouched). Empty `currentFile` while the
    /// run is between files.
    void progress(int done, int total, const QString& currentFile,
                  bool success);
    /// The whole batch ended. `formatted` succeeded, `failed` did not, and
    /// `failures` lists a message for each file that broke.
    void finished(int requestId, int formatted, int failed,
                  const QStringList& failures);

private:
    void startNext();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError();
    void emitFinished();

    QProcess* process_ = nullptr;
    QTimer* timeout_ = nullptr;
    /// Settings-provided overrides; empty = the compiled-in defaults.
    QString toolPathOverride_;
    QString fallbackStyle_;
    QStringList files_;        // the queue; front is in flight
    QString currentFile_;      // file clang-format is running on right now
    QString workingDir_;
    int total_ = 0;
    int done_ = 0;
    int formatted_ = 0;
    int failed_ = 0;
    QStringList failures_;
    /// Id of the active batch, or 0 when idle. A superseded run's late
    /// `finished` sees 0 and bails out.
    int requestId_ = 0;
    int nextId_ = 1;
    /// Set by the per-file timeout so the impending `finished` (the process
    /// was killed) is attributed as a timeout rather than a crash.
    bool timedOutFile_ = false;
    /// Set by errorOccurred() so the failure is counted exactly once — in
    /// onFinished(), where the queue advances. A crash emits both signals;
    /// handling the file in either one of them would double-count it.
    bool errorSeen_ = false;
};

}  // namespace LightCpp::App
