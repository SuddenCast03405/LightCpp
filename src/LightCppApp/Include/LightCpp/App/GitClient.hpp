#pragma once

#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "LightCpp/App/GitCommandPolicy.hpp"
#include "LightCpp/Project/GitStatus.hpp"

namespace LightCpp::App {

/// Runs git on behalf of the window.
///
/// Commands go through one queue, one process at a time: git takes a lock on
/// the index, so two commands that touch it at once is exactly the situation
/// git refuses. The queue also keeps the log readable — a command's output
/// arrives as one block rather than interleaved with the next one's.
///
/// The parsing of `git status --porcelain=v1` lives in LightCppProject, where
/// it is unit-tested; this class only supplies the bytes.
class GitClient : public QObject {
    Q_OBJECT
public:
    explicit GitClient(QObject* parent = nullptr);
    ~GitClient() override;

    /// The git executable: the path set for this client when there is one
    /// (from the settings), else the compiled-in default, else empty.
    [[nodiscard]] QString toolPath() const;
    /// The compiled-in git location (MSYS2, then PATH). Used when no
    /// settings-provided path has been set.
    [[nodiscard]] static QString defaultToolPath();
    [[nodiscard]] static bool isAvailable();

    /// Point this client at a specific git executable (the settings'
    /// gitPath). Empty restores the compiled-in default.
    void setToolPath(const QString& path) { toolPathOverride_ = path; }

    /// Directory git runs in. Git walks up from there itself, so a
    /// subdirectory of a repository is fine.
    void setWorkingDirectory(const QString& dir);
    [[nodiscard]] const QString& workingDirectory() const noexcept
    {
        return workingDirectory_;
    }

    /// Ask for `git status --porcelain=v1 -b -uall`; the answer arrives as
    /// statusReady(). A second call while one is queued is harmless.
    void refreshStatus();
    /// Queue an arbitrary git command. `title` names it in the log, e.g.
    /// "git commit".
    void run(const QStringList& arguments, const QString& title);

    /// True while a command is running or queued.
    [[nodiscard]] bool isBusy() const noexcept;

signals:
    void statusReady(const LightCpp::Project::GitStatusSummary& summary);
    /// A command finished. `text` is stdout followed by stderr, and
    /// `exitCode` is 0 on success — for anything else git's own message in
    /// `text` is the explanation, so it is shown rather than summarised.
    void commandFinished(const QString& title, const QString& text, int exitCode);
    /// A status refresh was abandoned by the watchdog. Reported separately
    /// from commandFinished on purpose: that signal asks the window for
    /// another refresh, and a wedged git would then be killed once a minute
    /// for the rest of the session. The status answer itself has already been
    /// emitted (the empty summary) — this is the explanation for the log.
    void statusTimedOut(const QString& text);
    /// A status refresh could not be started (for example, git is missing).
    /// Kept separate from commandFinished so the window does not enqueue an
    /// endless status-refresh loop in response to its own failed refresh.
    void statusFailed(const QString& text);

private:
    struct Job {
        QStringList arguments;
        QString title;
        bool feedsStatus = false;  // stdout is porcelain status output
        GitCommandPolicy::Kind kind = GitCommandPolicy::Kind::ReadOnly;
        /// Working directory captured when the job was queued. A status
        /// result from an old project must never update the current project.
        QString workingDirectory;
    };

    /// Queue a command. `feedsStatus` marks the one job whose stdout is
    /// porcelain status output rather than text for the log.
    void run(const QStringList& arguments, const QString& title, bool feedsStatus);
    void startNext();

    QProcess* process_ = nullptr;
    QList<Job> queue_;
    Job current_;
    QString workingDirectory_;
    /// The git executable to use; empty = the compiled-in default.
    QString toolPathOverride_;
    /// Kills a command that produces no exit for too long (a stale index
    /// lock, a hung network fetch): without it one wedged git stalls the
    /// queue — and every later status refresh — forever. The budget is per
    /// command (GitCommandPolicy): a read may not take a minute, while a
    /// commit runs the user's hooks and a push waits on the network.
    QTimer* watchdog_ = nullptr;
};

}  // namespace LightCpp::App
