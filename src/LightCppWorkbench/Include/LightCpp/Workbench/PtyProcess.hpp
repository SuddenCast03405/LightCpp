#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <thread>

#include "LightCpp/Workbench/PtySession.hpp"

namespace LightCpp::Workbench::Terminal {

/// A child process attached to a pseudo-terminal, with the master side
/// owned by this object.
///
/// Why this exists at all, next to `QProcess`: a QProcess gives the child a
/// *pipe*, and a program that checks `isatty()` then behaves differently —
/// `ls` stops colouring, `git` stops paging, `python` stops prompting. It
/// also has no window size and no foreground process group, so there is
/// nowhere to deliver Ctrl+C except to the shell itself, which is not the
/// process a user is trying to interrupt.
///
/// The implementation is pty(7): `posix_openpt` + `grantpt` + `unlockpt` +
/// `ptsname` for the master, then `fork`, `setsid`, `ioctl(TIOCSCTTY)` and
/// `dup2` in the child. `setsid` before `TIOCSCTTY` is the part that is
/// easy to get wrong: without it the child shares the parent's session and
/// the ioctl fails with EPERM.
///
/// Threading: a dedicated reader thread blocks on `read(master)` and emits
/// `dataReceived`, because a QSocketNotifier on a pty master is not portable
/// (macOS reports readable-but-read-returns-0 on a closed peer in a way the
/// notifier does not surface). The thread is the only place that reads; the
/// Qt thread is the only place that writes.
class PtyProcess {
public:
    /// Delivered on the Qt thread, in order. Callbacks rather than signals
    /// because this library deliberately has no moc: everything else in
    /// `Workbench` is plain C++ that a test can link without a QApplication,
    /// and one QObject here would end that.
    using DataHandler = std::function<void(const QByteArray&)>;
    using FinishHandler = std::function<void(int exitCode)>;
    using FailureHandler = std::function<void(const QString& reason)>;

    PtyProcess();
    ~PtyProcess();

    PtyProcess(const PtyProcess&) = delete;
    PtyProcess& operator=(const PtyProcess&) = delete;

    /// Start `command` in a fresh pty of `columns` x `rows`. Returns false
    /// when the pty could not be created or the child could not be started;
    /// `lastError()` then says which.
    bool start(const PtyCommand& command, int columns, int rows);

    [[nodiscard]] bool isRunning() const { return running_; }
    [[nodiscard]] qint64 processId() const { return pid_; }
    /// The child's exit status. Only meaningful after `finished`.
    [[nodiscard]] int exitCode() const { return exitCode_; }
    [[nodiscard]] QString lastError() const { return lastError_; }
    /// True when this build can create a pty at all. False on a platform
    /// without one, so the caller can fall back to a pipe and say so instead
    /// of offering a terminal that silently is not one.
    [[nodiscard]] static bool isSupported();

    /// Tell the child the window changed. This is what makes `vim` and
    /// `top` lay themselves out again; a pipe has no equivalent.
    void resize(int columns, int rows);

    /// Write to the child's stdin — what the user typed.
    void write(const QByteArray& bytes);

    /// Deliver a signal to the child's *foreground process group*, which is
    /// the group the terminal would signal. Sending it to the shell instead
    /// interrupts the wrong process: the shell is waiting on the program.
    void sendSignal(TerminalSignal signal);

    /// Hang up: close the master, which sends SIGHUP to the session leader
    /// and lets the child clean up its own children first.
    void stop();

    void setDataHandler(DataHandler handler) { onData_ = std::move(handler); }
    void setFinishHandler(FinishHandler handler) { onFinish_ = std::move(handler); }
    /// The child could not be started or the pty died. Separate from the
    /// finish handler because there is no exit code to report.
    void setFailureHandler(FailureHandler handler) { onFailure_ = std::move(handler); }

private:
    void stopReader();
    void reap();
    /// Run `work` on the Qt thread. The reader thread never calls a handler
    /// directly: the handlers touch widgets.
    void dispatch(std::function<void()> work);

    qint64 master_ = -1;
    qint64 pid_ = 0;
    /// The reader blocks in read(2); stopping joins it. Closing the master
    /// from this thread is what makes the read return, which is why the fd
    /// is not closed until the thread has been joined.
    std::thread reader_;
    std::atomic<bool> stopping_{false};
    DataHandler onData_;
    FinishHandler onFinish_;
    FailureHandler onFailure_;
    /// Set from the reader thread, read from the Qt thread. A QPointer would
    /// need moc; an atomic flag is enough because destruction joins the
    /// reader before freeing anything.
    std::atomic<bool> alive_{true};
    bool running_ = false;
    int exitCode_ = 0;
    QString lastError_;
};

}  // namespace LightCpp::Workbench::Terminal
