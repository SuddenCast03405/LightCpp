#include "LightCpp/App/BatchFormatter.hpp"

#include "LightCpp/App/CodeFormatter.hpp"
#include "LightCpp/App/ToolchainPaths.hpp"

#include <QProcess>
#include <QTimer>

namespace LightCpp::App {
namespace {

/// A single file formats in milliseconds; this only guards against a
/// pathological input hanging the whole batch.
constexpr int kPerFileTimeoutMs = 10000;

}  // namespace

BatchFormatter::BatchFormatter(QObject* parent)
    : QObject(parent)
{
}

BatchFormatter::~BatchFormatter()
{
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        // Invalidate the run before killing: the `finished` that kill()
        // triggers must not advance the queue of a batch that no longer
        // matters.
        requestId_ = 0;
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
    }
}

int BatchFormatter::formatFiles(const QStringList& filePaths,
                                const QString& workingDir)
{
    const int id = nextId_++;

    // Supersede whatever is running: its remaining answers describe a batch
    // the user has cancelled, and delivering them would race with this run.
    // No disconnect here — the same QProcess is reused for this batch, and
    // the requestId_ = 0 guard already makes the stale handlers drop out.
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        requestId_ = 0;
        process_->kill();
        process_->waitForFinished(1000);
    }

    files_ = filePaths;
    workingDir_ = workingDir;
    total_ = filePaths.size();
    done_ = 0;
    formatted_ = 0;
    failed_ = 0;
    failures_.clear();
    currentFile_.clear();
    requestId_ = id;

    if (total_ == 0) {
        // Nothing to do — report success immediately so the caller's finished
        // handler still runs and can clear its "formatting..." status.
        emitFinished();
        return id;
    }

    if (!CodeFormatter::isAvailable()) {
        // clang-format is missing for the whole batch; every file counts as
        // failed so the summary names the cause once rather than per file.
        failed_ = total_;
        done_ = total_;
        failures_.append(tr("clang-format was not found."));
        emitFinished();
        return id;
    }

    if (process_ == nullptr) {
        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::SeparateChannels);
        connect(process_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &BatchFormatter::onFinished);
        connect(process_, &QProcess::errorOccurred, this, &BatchFormatter::onError);
    }
    if (timeout_ == nullptr) {
        timeout_ = new QTimer(this);
        timeout_->setSingleShot(true);
        connect(timeout_, &QTimer::timeout, this, [this] {
            if (requestId_ == 0 || process_ == nullptr
                || process_->state() == QProcess::NotRunning) {
                return;
            }
            // Flag the kill so onFinished() attributes it as a timeout; the
            // finished signal fires on the next event-loop turn and advances
            // the queue. clang-format -i writes a temp file then renames on
            // success, so a kill mid-run leaves the original intact.
            timedOutFile_ = true;
            process_->kill();
        });
    }

    startNext();
    return id;
}

bool BatchFormatter::isRunning() const
{
    return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

void BatchFormatter::startNext()
{
    if (files_.isEmpty()) {
        emitFinished();
        return;
    }
    // Fresh file in flight — drop any mark left over from the previous one.
    // A FailedToStart error sets it but never produces a finished signal
    // (that path is handled synchronously below), so the next file must not
    // inherit the flag.
    errorSeen_ = false;
    currentFile_ = files_.first();
    const QString tool = CodeFormatter::toolPath();
    // -style=file discovers the nearest .clang-format; the fallback covers
    // trees without one. -i writes the formatted text back in place.
    QStringList args{
        QStringLiteral("-i"),
        QStringLiteral("-style=file"),
        QStringLiteral("--fallback-style=") + QString::fromLatin1(CodeFormatter::kFallbackStyle),
        currentFile_,
    };
    process_->setWorkingDirectory(workingDir_);
    process_->setProgram(tool);
    process_->setArguments(args);
    process_->start();
    if (!process_->waitForStarted(5000)) {
        // Could not even launch — count this file and continue, so one bad
        // path does not strand the rest of the batch.
        ++failed_;
        failures_.append(tr("%1: could not start clang-format (%2)")
                             .arg(currentFile_, process_->errorString()));
        ++done_;
        emit progress(done_, total_, currentFile_, false);
        currentFile_.clear();
        files_.removeFirst();
        startNext();
        return;
    }
    timeout_->start(kPerFileTimeoutMs);
}

void BatchFormatter::onFinished(int exitCode, QProcess::ExitStatus status)
{
    if (timeout_ != nullptr) {
        timeout_->stop();
    }
    if (requestId_ == 0) {
        return;  // a superseded run reporting late — drop it
    }
    const QString file = currentFile_;
    if (file.isEmpty()) {
        return;  // nothing in flight
    }
    currentFile_.clear();
    const QByteArray stderrBytes = process_->readAllStandardError();
    bool ok = false;
    if (timedOutFile_) {
        timedOutFile_ = false;
        // A killed process can also surface as errorOccurred(Crashed) before
        // this signal; the timeout already owns the attribution.
        errorSeen_ = false;
        ++failed_;
        failures_.append(tr("%1: timed out").arg(file));
    } else if (errorSeen_) {
        // errorOccurred fired earlier for this file and only marked it (a
        // crash emits both signals) — count the failure here, where the
        // queue advances, so it lands exactly once.
        errorSeen_ = false;
        ++failed_;
        failures_.append(tr("%1: %2")
                             .arg(file, process_ != nullptr ? process_->errorString()
                                                             : QStringLiteral("unknown error")));
    } else if (status == QProcess::NormalExit && exitCode == 0) {
        ok = true;
        ++formatted_;
    } else {
        ++failed_;
        const QString detail = QString::fromUtf8(stderrBytes).trimmed();
        failures_.append(detail.isEmpty()
                            ? tr("%1: clang-format exited with code %2").arg(file).arg(exitCode)
                            : tr("%1: %2").arg(file, detail));
    }
    ++done_;
    emit progress(done_, total_, file, ok);
    files_.removeFirst();
    startNext();
}

void BatchFormatter::onError()
{
    if (requestId_ == 0) {
        return;  // a superseded run reporting late — drop it
    }
    if (currentFile_.isEmpty()) {
        return;  // nothing in flight
    }
    // Mark only — do not count and do not advance. For a crash the matching
    // finished() follows immediately; onFinished() owns the counting and the
    // queue so the file is handled exactly once. FailedToStart never gets a
    // finished(), but that path is handled synchronously in startNext().
    errorSeen_ = true;
    if (timeout_ != nullptr) {
        timeout_->stop();
    }
}

void BatchFormatter::emitFinished()
{
    const int id = requestId_;
    requestId_ = 0;
    emit finished(id, formatted_, failed_, failures_);
}

}  // namespace LightCpp::App
