#pragma once

#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

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

    /// The git executable, or empty when there is none.
    [[nodiscard]] static QString toolPath();
    [[nodiscard]] static bool isAvailable();

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

private:
    struct Job {
        QStringList arguments;
        QString title;
        bool feedsStatus = false;  // stdout is porcelain status output
    };

    /// Queue a command. `feedsStatus` marks the one job whose stdout is
    /// porcelain status output rather than text for the log.
    void run(const QStringList& arguments, const QString& title, bool feedsStatus);
    void startNext();

    QProcess* process_ = nullptr;
    QList<Job> queue_;
    Job current_;
    QString workingDirectory_;
};

}  // namespace LightCpp::App
