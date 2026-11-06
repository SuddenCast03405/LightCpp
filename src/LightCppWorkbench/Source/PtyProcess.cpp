// PtyProcess — the POSIX half of a real terminal.
//
// Everything here is the operating system's business and is deliberately not
// unit tested: a test of a fork/exec would be testing Linux. What *is* tested
// is the decision layer next door (PtyCommand, winsizePayload, controlCodeFor),
// and the two are separated precisely so the interesting half can be asserted.

#include "LightCpp/Workbench/PtyProcess.hpp"

#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QMetaObject>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(Q_OS_LINUX)
#include <pty.h>
#elif defined(Q_OS_MACOS)
#include <util.h>
#endif

namespace LightCpp::Workbench::Terminal {

namespace {

/// Build the environment array the child execve takes. Qt's own
/// QProcessEnvironment cannot be used because the child is started by hand.
std::vector<std::string> buildEnvironment(const QStringList& overrides)
{
    std::vector<std::string> environment;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string text(*entry);
        bool replaced = false;
        for (const QString& override_ : overrides) {
            const std::string key = override_.section(QLatin1Char('='), 0, 0).toStdString();
            if (text.compare(0, key.size() + 1, key + "=") == 0) {
                replaced = true;
                environment.push_back(override_.toStdString());
                break;
            }
        }
        if (!replaced) {
            environment.push_back(text);
        }
    }
    // An override whose key was not inherited at all still has to be set.
    for (const QString& override_ : overrides) {
        const std::string text = override_.toStdString();
        const std::string key = override_.section(QLatin1Char('='), 0, 0).toStdString();
        bool present = false;
        for (const std::string& existing : environment) {
            if (existing.compare(0, key.size() + 1, key + "=") == 0) {
                present = true;
                break;
            }
        }
        if (!present) {
            environment.push_back(text);
        }
    }
    return environment;
}

/// Turn a waitpid status into the number the caller sees. A signalled child
/// has no exit status, so it is reported as the negative signal number: a
/// caller that prints the code then says "-15" rather than the misleading 0.
int exitStatusOf(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return -WTERMSIG(status);
    }
    return -1;
}

/// Hand `work` to the Qt thread. The reader thread must never touch the
/// handlers directly: they append to widgets. QMetaObject needs a QObject, so
/// a bare function is posted through a small relay object created on the fly.
class Relay : public QObject {
public:
    explicit Relay(std::function<void()> work) : work_(std::move(work)) {}
    void run()
    {
        std::function<void()> work = std::move(work_);
        work();
        deleteLater();
    }

private:
    std::function<void()> work_;
};

}  // namespace

PtyProcess::PtyProcess() = default;

PtyProcess::~PtyProcess()
{
    alive_ = false;
    stop();
}

bool PtyProcess::isSupported()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}

bool PtyProcess::start(const PtyCommand& command, int columns, int rows)
{
    if (!command.isValid()) {
        lastError_ = QStringLiteral("no program given");
        if (onFailure_) {
            onFailure_(lastError_);
        }
        return false;
    }
    if (running_) {
        lastError_ = QStringLiteral("a session is already running");
        if (onFailure_) {
            onFailure_(lastError_);
        }
        return false;
    }

    // The struct is filled directly rather than from the payload: this is the
    // one place where the field order is the kernel's. winsizePayload() exists
    // for the platforms that pass the bytes across an API boundary instead.
    struct winsize size {};
    size.ws_row = static_cast<unsigned short>(rows < 1 ? 1 : (rows > 65535 ? 65535 : rows));
    size.ws_col = static_cast<unsigned short>(columns < 1 ? 1 : (columns > 65535 ? 65535 : columns));

    int master = -1;
    int slave = -1;
#if defined(Q_OS_MACOS)
    // macOS openpty() does the grantpt/unlockpt dance itself.
    if (openpty(&master, &slave, nullptr, nullptr, &size) != 0) {
        lastError_ = QStringLiteral("openpty failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        if (onFailure_) {
            onFailure_(lastError_);
        }
        return false;
    }
#else
    master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        lastError_ = QStringLiteral("posix_openpt failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        if (onFailure_) {
            onFailure_(lastError_);
        }
        return false;
    }
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        lastError_ = QStringLiteral("could not unlock the pty: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        ::close(master);
        if (onFailure_) {
            onFailure_(lastError_);
        }
        return false;
    }
    char name[256] = {};
    if (::ptsname_r(master, name, sizeof(name)) != 0) {
        lastError_ = QStringLiteral("ptsname failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        ::close(master);
        if (onFailure_) {
            onFailure_(lastError_);
        }
        return false;
    }
    slave = ::open(name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        lastError_ = QStringLiteral("could not open %1: %2").arg(QString::fromLocal8Bit(name), QString::fromLocal8Bit(std::strerror(errno)));
        ::close(master);
        if (onFailure_) {
            onFailure_(lastError_);
        }
        return false;
    }
    // The size is set on the slave before the child runs, so the shell's
    // very first `stty size` and the first paint of a full-screen program
    // are already right — a SIGWINCH afterwards would be a second answer to
    // a question the program has not asked yet.
    ::ioctl(slave, TIOCSWINSZ, &size);
#endif

    std::vector<std::string> environment =
        buildEnvironment(command.environment.isEmpty()
                             ? terminalEnvironment(command.term)
                             : QStringList(terminalEnvironment(command.term))
                                   + command.environment);
    std::vector<std::string> arguments;
    arguments.push_back(command.program.toStdString());
    for (const QString& argument : command.arguments) {
        arguments.push_back(argument.toStdString());
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    std::vector<char*> envp;
    envp.reserve(environment.size() + 1);
    for (std::string& entry : environment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        lastError_ = QStringLiteral("fork failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        ::close(master);
        ::close(slave);
        if (onFailure_) {
            onFailure_(lastError_);
        }
        return false;
    }
    if (pid == 0) {
        // --- child ---------------------------------------------------------
        // A new session detaches from the IDE's controlling terminal and
        // makes this child a session leader, which is the precondition for
        // TIOCSCTTY below. Without setsid the ioctl fails with EPERM and the
        // program never gets a controlling terminal — so job control, and
        // with it Ctrl+C, silently does not work.
        ::setsid();
#if defined(TIOCSCTTY)
        ::ioctl(slave, TIOCSCTTY, 0);
#endif
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        ::dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) {
            ::close(slave);
        }
        // The master must not leak into the child: holding it open means the
        // parent's read never sees EOF when the child exits.
        ::close(master);
        if (!command.workingDirectory.isEmpty()) {
            if (::chdir(command.workingDirectory.toLocal8Bit().constData()) != 0) {
                // Not fatal: a missing directory should leave the shell in
                // the inherited one rather than refuse to start.
            }
        }
        ::execve(argv[0], argv.data(), envp.data());
        // exec failed. _exit, not exit: the child has its own stdio and must
        // not flush the parent's buffers into the terminal.
        const char message[] = "lightcpp: could not start the shell\r\n";
        (void)!::write(STDERR_FILENO, message, sizeof(message) - 1);
        ::_exit(127);
    }

    // --- parent ------------------------------------------------------------
    ::close(slave);
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    master_ = master;
    pid_ = pid;
    running_ = true;
    stopping_ = false;

    reader_ = std::thread([this] {
        QByteArray buffer(4096, '\0');
        while (!stopping_.load()) {
            // poll() with a timeout, not a bare read(): the reader has to be
            // able to notice `stopping_` even when the child is silent, which
            // is exactly the state a terminal sits in while nobody types.
            struct pollfd watch {};
            watch.fd = int(master_);
            watch.events = POLLIN;
            const int ready = ::poll(&watch, 1, 200);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0) {
                continue;  // timeout: re-check stopping_
            }
            const ssize_t got = ::read(int(master_), buffer.data(), size_t(buffer.size()));
            if (got > 0) {
                const QByteArray chunk = QByteArray(buffer.constData(), int(got));
                dispatch([this, chunk] {
                    if (alive_ && onData_) {
                        onData_(chunk);
                    }
                });
                continue;
            }
            if (got == 0) {
                break;  // EOF: the child closed the slave side
            }
            if (errno == EINTR) {
                continue;
            }
            // EIO is the normal "the other side is gone" on Linux, not an
            // error worth reporting.
            break;
        }
        dispatch([this] { reap(); });
    });
    return true;
}

void PtyProcess::dispatch(std::function<void()> work)
{
    // The relay is built on the *calling* thread — the reader — so it has to be
    // moved to the app's thread before invokeMethod will queue anything there.
    // Without the move the posted event goes to a thread with no event loop
    // and is simply never delivered: the session starts, the child writes, and
    // nothing ever arrives.
    Relay* relay = new Relay(std::move(work));
    if (QCoreApplication::instance() != nullptr) {
        relay->moveToThread(QCoreApplication::instance()->thread());
    }
    QMetaObject::invokeMethod(relay, &Relay::run, Qt::QueuedConnection);
}

void PtyProcess::reap()
{
    if (!alive_ || !running_ || pid_ <= 0) {
        return;
    }
    int status = 0;
    const pid_t waited = ::waitpid(pid_t(pid_), &status, WNOHANG);
    if (waited == 0) {
        // Still running: the pty closed but the child has not been reaped
        // yet. Try again when the event loop next spins.
        dispatch([this] { reap(); });
        return;
    }
    running_ = false;
    exitCode_ = exitStatusOf(status);
    if (onFinish_) {
        onFinish_(exitCode_);
    }
}

void PtyProcess::stopReader()
{
    if (!reader_.joinable()) {
        return;
    }
    stopping_ = true;
    // Wake the reader before closing the fd. Closing a descriptor does *not*
    // make another thread's blocked read(2) return — the kernel keeps the
    // open file description alive until the read finishes — so a session
    // whose child is still producing output would block the join forever and
    // hang the IDE on window close. Writing does not have that problem: it
    // wakes the reader, which then sees `stopping_` and leaves before the
    // descriptor is closed under it. Terminal input is the one thing that
    // cannot make the child misbehave, so a bell is a safe nudge.
    if (master_ >= 0) {
        const char bell = '\a';
        (void)!::write(int(master_), &bell, 1);
    }
    reader_.join();
    // Now that nothing else can be reading it, the descriptor is ours to close.
    if (master_ >= 0) {
        ::close(int(master_));
        master_ = -1;
    }
}

void PtyProcess::resize(int columns, int rows)
{
    if (master_ < 0) {
        return;
    }
    struct winsize size {};
    size.ws_row = static_cast<unsigned short>(rows < 1 ? 1 : (rows > 65535 ? 65535 : rows));
    size.ws_col = static_cast<unsigned short>(columns < 1 ? 1 : (columns > 65535 ? 65535 : columns));
    // The ioctl sends SIGWINCH to the foreground group by itself; the caller
    // does not need to signal anything.
    ::ioctl(int(master_), TIOCSWINSZ, &size);
}

void PtyProcess::write(const QByteArray& bytes)
{
    if (master_ < 0 || bytes.isEmpty()) {
        return;
    }
    qsizetype written = 0;
    while (written < bytes.size()) {
        const ssize_t got = ::write(int(master_), bytes.constData() + written,
                                    size_t(bytes.size() - written));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            // EAGAIN would mean the master is non-blocking, which it is not;
            // EIO means the child died between the check and the write, and
            // there is nothing useful to do about it.
            break;
        }
        written += got;
    }
}

void PtyProcess::sendSignal(TerminalSignal signal)
{
    if (pid_ <= 0) {
        return;
    }
    int number = SIGINT;
    switch (signal) {
    case TerminalSignal::Interrupt: number = SIGINT; break;
    case TerminalSignal::Quit:      number = SIGQUIT; break;
    case TerminalSignal::Suspend:   number = SIGTSTP; break;
    case TerminalSignal::Eof:       return;  // no signal: the byte is the EOF
    }
    // The child is a session leader with its own pty, so its pid *is* the
    // foreground group id of that pty as long as no program called setpgid.
    // Signal the group, not the process: that is what the terminal driver
    // does, and it reaches a program the shell has spawned.
    ::kill(-pid_t(pid_), number);
}

void PtyProcess::stop()
{
    stopReader();
    if (pid_ > 0) {
        // SIGHUP is what a real terminal sends when the window is closed; the
        // child's own traps then get a chance to clean up. SIGKILL is only the
        // last resort, after a grace period the event loop does not wait for.
        ::kill(-pid_t(pid_), SIGHUP);
        int status = 0;
        bool reaped = false;
        for (int attempt = 0; attempt < 50; ++attempt) {
            const pid_t waited = ::waitpid(pid_t(pid_), &status, WNOHANG);
            if (waited != 0) {
                reaped = true;
                break;
            }
            ::usleep(2000);
        }
        if (!reaped) {
            // Still there: it ignored SIGHUP.
            ::kill(-pid_t(pid_), SIGKILL);
            reaped = ::waitpid(pid_t(pid_), &status, 0) != 0;
        }
        pid_ = 0;
        // The caller is told the session ended even when it ended because the
        // caller asked it to. Without this the reader thread's reap() can see
        // pid_ already cleared and return quietly, so a window closing its
        // terminal would never hear "finished" — and had its data handler
        // called after the widget that owns it had been destroyed.
        running_ = false;
        exitCode_ = exitStatusOf(status);
        if (reaped && onFinish_) {
            onFinish_(exitCode_);
        }
    }
    running_ = false;
}

}  // namespace LightCpp::Workbench::Terminal
