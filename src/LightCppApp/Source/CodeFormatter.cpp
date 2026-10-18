#include "LightCpp/App/CodeFormatter.hpp"

#include "LightCpp/App/ToolchainPaths.hpp"

#include <QTimer>

namespace LightCpp::App {
namespace {

/// Formatting a normal file takes milliseconds. This exists only so a
/// pathological input cannot leave a request pending for the rest of the
/// session.
constexpr int kFormatTimeoutMs = 15000;

}  // namespace

QString CodeFormatter::toolPath()
{
    // The copy next to the compilers first, because that is the clang-format
    // matching the clangd the editor already talks to. Then PATH, for anyone
    // who installed it some other way. Both halves live in ToolchainPaths,
    // which only answers with a path that exists.
    return ToolchainPaths::clangFormatPath();
}

bool CodeFormatter::isAvailable()
{
    return !toolPath().isEmpty();
}

QString CodeFormatter::resolvedToolPath() const
{
    if (!toolPathOverride_.isEmpty()) {
        return toolPathOverride_;
    }
    return toolPath();
}

QString CodeFormatter::resolvedFallbackStyle() const
{
    // "file" means the same thing here as everywhere else: discovery of a
    // .clang-format is already what -style=file does, so the fallback only
    // needs a real preset name — keep the built-in one.
    if (!fallbackStyle_.isEmpty()
        && fallbackStyle_.compare(QLatin1String("file"), Qt::CaseInsensitive) != 0) {
        return fallbackStyle_;
    }
    return QString::fromLatin1(kFallbackStyle);
}

QStringList CodeFormatter::argumentsFor(const QString& assumeFileName)
{
    // `-style=file` makes clang-format look for the nearest .clang-format and
    // `--fallback-style` covers the common case where the project has none.
    // Measured: with a .clang-format present its style wins; without one the
    // fallback is used. Naming the style directly instead (`-style=Microsoft`)
    // would ignore a project's .clang-format altogether.
    QStringList args{
        QStringLiteral("-style=file"),
        QStringLiteral("--fallback-style=") + QString::fromLatin1(kFallbackStyle),
    };
    if (!assumeFileName.isEmpty()) {
        // Lets the style search start at the real file without the buffer
        // having to be saved first.
        args << (QStringLiteral("--assume-filename=") + assumeFileName);
    }
    return args;
}

CodeFormatter::CodeFormatter(QObject* parent)
    : QObject(parent)
{
}

CodeFormatter::~CodeFormatter()
{
    discardProcess();
}

void CodeFormatter::discardProcess()
{
    inFlightId_ = 0;
    if (process_ == nullptr) {
        return;
    }
    process_->disconnect(this);
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(1000);
    }
    // Deleted rather than kept: a fresh QProcess is what lets the next
    // request reconnect, and it cannot report anything about the run we just
    // abandoned. Safe here because nothing in this path runs from inside the
    // process's own signal emission.
    delete process_;
    process_ = nullptr;
}

int CodeFormatter::format(const QString& text, const QString& assumeFileName)
{
    const int id = nextId_++;

    // Supersede whatever is running: its answer describes text that no longer
    // exists, and delivering it would undo the newer request.
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        discardProcess();
    }

    const QString tool = resolvedToolPath();
    if (tool.isEmpty()) {
        // Queued, so the caller is guaranteed to get the id back before the
        // failure arrives — otherwise there would be nothing to match it to.
        QMetaObject::invokeMethod(
            this, [this, id] { emit failed(id, tr("clang-format was not found.")); },
            Qt::QueuedConnection);
        return id;
    }

    // Reconnects after every supersede and timeout: discardProcess() deletes
    // the object rather than leaving a disconnected one behind, so this is a
    // normal path, not a one-off construction.
    if (process_ == nullptr) {
        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::SeparateChannels);
        connect(process_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &CodeFormatter::onFinished);
        // A signature with no parameters: QProcess hands over the error code,
        // and there is nothing this class can do with it that errorString()
        // does not say better.
        connect(process_, &QProcess::errorOccurred, this, &CodeFormatter::onError);
    }
    if (timeout_ == nullptr) {
        timeout_ = new QTimer(this);
        timeout_->setSingleShot(true);
        connect(timeout_, &QTimer::timeout, this, [this] {
            if (inFlightId_ == 0 || process_ == nullptr) {
                return;
            }
            const int id = inFlightId_;
            // Drop the process, not just its connections, or the next request
            // would run against a silent one and fail the same way again.
            discardProcess();
            emit failed(id, tr("clang-format did not finish within %1 seconds.")
                                .arg(kFormatTimeoutMs / 1000));
        });
    }

    inFlightId_ = id;
    process_->setProgram(tool);
    // Instance arguments: same shape as the static helper, but with the
    // configured fallback style.
    QStringList args{
        QStringLiteral("-style=file"),
        QStringLiteral("--fallback-style=") + resolvedFallbackStyle(),
    };
    if (!assumeFileName.isEmpty()) {
        args << (QStringLiteral("--assume-filename=") + assumeFileName);
    }
    process_->setArguments(args);
    process_->start();
    // Waited for on purpose: writing to stdin only works once the channel
    // exists, and a start that fails here has a clearer message than the one
    // errorOccurred would produce later.
    if (!process_->waitForStarted(5000)) {
        inFlightId_ = 0;
        emit failed(id, tr("clang-format could not be started (%1).").arg(tool));
        return id;
    }
    process_->write(text.toUtf8());
    process_->closeWriteChannel();
    timeout_->start(kFormatTimeoutMs);
    return id;
}

void CodeFormatter::onFinished(int exitCode, QProcess::ExitStatus status)
{
    const int id = inFlightId_;
    inFlightId_ = 0;
    if (timeout_ != nullptr) {
        timeout_->stop();
    }
    if (id == 0 || process_ == nullptr) {
        return;  // a superseded run
    }

    const QByteArray stdoutBytes = process_->readAllStandardOutput();
    const QByteArray stderrBytes = process_->readAllStandardError();
    if (status != QProcess::NormalExit || exitCode != 0) {
        const QString detail = QString::fromUtf8(stderrBytes).trimmed();
        emit failed(id, detail.isEmpty()
                            ? tr("clang-format exited with code %1.").arg(exitCode)
                            : detail);
        return;
    }
    emit formatted(id, QString::fromUtf8(stdoutBytes));
}

void CodeFormatter::onError()
{
    if (inFlightId_ == 0) {
        return;  // a superseded run continues to report against itself
    }
    const int id = inFlightId_;
    inFlightId_ = 0;
    if (timeout_ != nullptr) {
        timeout_->stop();
    }
    emit failed(id, tr("clang-format failed: %1")
                        .arg(process_ != nullptr ? process_->errorString()
                                                : QStringLiteral("unknown error")));
}

}  // namespace LightCpp::App
