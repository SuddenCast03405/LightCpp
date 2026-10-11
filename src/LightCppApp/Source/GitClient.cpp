#include "LightCpp/App/GitClient.hpp"

#include "LightCpp/App/ToolchainPaths.hpp"

#include <QDir>
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
    watchdog_ = new QTimer(this);
    watchdog_->setSingleShot(true);
    watchdog_->setInterval(60 * 1000);
    connect(watchdog_, &QTimer::timeout, this, [this] {
        if (process_ == nullptr || process_->state() == QProcess::NotRunning) {
            return;
        }
        const QString title = current_.title;
        current_ = Job{};
        process_->kill();
        emit commandFinished(
            title,
            QStringLiteral("git did not exit within 60 s and was killed"),
            -1);
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
    queue_.append(Job{arguments, title, feedsStatus});
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

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // git translates the phrases in its output, and `git status -b` is the one
    // place this IDE parses them ("ahead 1, behind 2"). Forcing the C locale
    // makes those words the ones the parser expects instead of leaving the
    // ahead/behind counts to be quietly dropped on a localised install.
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process_->setProcessEnvironment(env);
    process_->setWorkingDirectory(workingDirectory_.isEmpty() ? QDir::currentPath()
                                                              : workingDirectory_);
    process_->start(toolPath(), current_.arguments);
    watchdog_->start();
}

}  // namespace LightCpp::App
