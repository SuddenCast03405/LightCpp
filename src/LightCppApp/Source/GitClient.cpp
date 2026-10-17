#include "LightCpp/App/GitClient.hpp"

#include "LightCpp/App/ToolchainPaths.hpp"

#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

namespace LightCpp::App {

QString GitClient::defaultToolPath()
{
    if (QFileInfo::exists(kGitPath)) {
        return kGitPath;
    }
    return QStandardPaths::findExecutable(QStringLiteral("git"));
}

QString GitClient::toolPath() const
{
    if (!toolPathOverride_.isEmpty()) {
        return toolPathOverride_;
    }
    return defaultToolPath();
}

bool GitClient::isAvailable()
{
    return !defaultToolPath().isEmpty();
}

GitClient::GitClient(QObject* parent)
    : QObject(parent)
{
    // One watchdog for the whole queue, restarted per command: a git that
    // never exits (stale index.lock held by a crashed process, a credential
    // prompt that escaped GIT_TERMINAL_PROMPT=0) would otherwise wedge the
    // queue — and every future status refresh — until the IDE restarts.
    // The budget is not fixed: it comes from what the command does.
    watchdog_ = new QTimer(this);
    watchdog_->setSingleShot(true);
    connect(watchdog_, &QTimer::timeout, this, [this] {
        if (process_ == nullptr || process_->state() == QProcess::NotRunning) {
            return;
        }
        // Read the job out before clearing it: the finished handler recognises
        // "this kill is mine" by the empty title.
        const Job job = current_;
        current_ = Job{};
        process_->kill();
        const QString text = GitCommandPolicy::timeoutMessage(
            job.title, job.kind, GitCommandPolicy::timeoutMsFor(job.kind));
        if (job.feedsStatus) {
            // The panel is waiting for this answer, and silence is what made
            // the symptom invisible: give it the empty summary (the same shape
            // as "not a repository") and put the reason in the log through the
            // log-only signal. Not commandFinished: the window answers that one
            // with another refresh, so a wedged git would be killed once a
            // minute for the rest of the session.
            emit statusReady(LightCpp::Project::GitStatusSummary{});
            emit statusTimedOut(text);
        } else {
            emit commandFinished(job.title, text, -1);
        }
        startNext();
    });
}

GitClient::~GitClient()
{
    if (watchdog_ != nullptr) {
        watchdog_->stop();
    }
    if (process_ == nullptr) {
        return;
    }
    // Drop anything still queued first: the finished handler starts the next
    // job, so a waiting command would otherwise be launched *during* shutdown
    // and reported as "QProcess: Destroyed while process is still running".
    queue_.clear();
    current_ = Job{};
    if (process_->state() != QProcess::NotRunning) {
        // Same reason as stop(): no receiver can still be interested, and
        // letting the handlers run on the way out only gives them a chance to
        // start something new.
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
    }
}

void GitClient::setWorkingDirectory(const QString& dir)
{
    if (workingDirectory_ == dir) {
        return;
    }
    workingDirectory_ = dir;
    // A queued command belongs to the directory it was queued for, so drop it
    // rather than run it somewhere it was not meant for.
    queue_.clear();
}

bool GitClient::isBusy() const noexcept
{
    return !queue_.isEmpty()
           || (process_ != nullptr && process_->state() != QProcess::NotRunning);
}

void GitClient::refreshStatus()
{
    run({QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
         QStringLiteral("-b"), QStringLiteral("-uall")},
        QStringLiteral("git status"), true);
}

void GitClient::run(const QStringList& arguments, const QString& title)
{
    run(arguments, title, false);
}

void GitClient::run(const QStringList& arguments, const QString& title,
                    bool feedsStatus)
{
    if (arguments.isEmpty()) {
        return;
    }
    queue_.append(Job{arguments, title, feedsStatus,
                      GitCommandPolicy::kindOf(arguments)});
    startNext();
}

void GitClient::startNext()
{
    if (queue_.isEmpty()) {
        return;
    }
    // A job is already in flight: its own finished handler pumps the queue when
    // it gets there. Starting anyway would overwrite `current_` and have
    // QProcess refuse the second start() — the command would vanish, and the
    // running job's output would be reported under the new job's name.
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        return;
    }
    if (process_ == nullptr) {
        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::SeparateChannels);
        connect(process_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            if (process_ == nullptr) return;
            watchdog_->stop();
            const QString title = current_.title;
            const bool feedsStatus = current_.feedsStatus;
            current_ = Job{};
            if (title.isEmpty()) {
                // The watchdog already reported this job as timed out; the
                // kill's own finished signal has nothing left to report.
                startNext();
                return;
            }

            const QString stdoutText = QString::fromUtf8(process_->readAllStandardOutput());
            const QString stderrText = QString::fromUtf8(process_->readAllStandardError());
            const int exitCode = (status == QProcess::NormalExit) ? code : -1;

            if (feedsStatus) {
                // A failing `git status` is how "this is not a repository" is
                // reported — an empty answer, not an error to print.
                emit statusReady(exitCode == 0
                                     ? LightCpp::Project::parseGitStatus(
                                           stdoutText.toUtf8())
                                     : LightCpp::Project::GitStatusSummary{});
            } else {
                QString text = stdoutText;
                if (!stderrText.isEmpty()) {
                    if (!text.isEmpty() && !text.endsWith(u'\n')) {
                        text += u'\n';
                    }
                    text += stderrText;
                }
                emit commandFinished(title, text.trimmed(), exitCode);
            }
            startNext();
        });
        connect(process_, &QProcess::errorOccurred, this, [this] {
            if (process_ == nullptr) return;
            if (process_->error() != QProcess::FailedToStart) {
                return;
            }
            watchdog_->stop();
            const QString title = current_.title;
            current_ = Job{};
            emit commandFinished(title, process_->errorString(), -1);
            startNext();
        });
    }

    current_ = queue_.takeFirst();
    if (current_.arguments.isEmpty()) {
        current_ = Job{};
        startNext();
        return;
    }

    if (workingDirectory_.isEmpty()) {
        // No working set: refuse, and do not let QProcess fall back to this
        // process's own start-up directory. Running git there is how `git
        // init` once created a repository inside the IDE's install folder
        // (or the user's home) while the window reported success and showed
        // the branch of the directory it happened to be launched from. The
        // queue still has to move on, so the next job is started as usual.
        const QString title = current_.title;
        const bool feedsStatus = current_.feedsStatus;
        current_ = Job{};
        if (feedsStatus) {
            // A status refresh with nothing to describe is the empty answer,
            // exactly as outside a repository.
            emit statusReady(LightCpp::Project::GitStatusSummary{});
        } else {
            emit commandFinished(
                title,
                QStringLiteral("no working directory for git — open a folder or a project first"),
                -1);
        }
        startNext();
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // git translates the phrases in its output, and `git status -b` is the one
    // place this IDE parses them ("ahead 1, behind 2"). Forcing the C locale
    // makes those words the ones the parser expects instead of leaving the
    // ahead/behind counts to be quietly dropped on a localised install.
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process_->setProcessEnvironment(env);
    process_->setWorkingDirectory(workingDirectory_);
    // Non-ASCII file names arrive quoted in octal escapes ("\346\265\213…")
    // unless quotepath is off; the parser and the log pane want the real
    // names (the output is decoded as UTF-8 further up).
    process_->start(toolPath(),
                    QStringList{QStringLiteral("-c"),
                                QStringLiteral("core.quotepath=false")}
                        + current_.arguments);
    watchdog_->setInterval(GitCommandPolicy::timeoutMsFor(current_.kind));
    watchdog_->start();
}

}  // namespace LightCpp::App
