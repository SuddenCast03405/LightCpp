// PtyProcessTests — the parts of a real terminal that an operating system can
// answer for us.
//
// This file deliberately contains the only tests in the repository that fork
// a process. That is justified: the alternative is shipping a terminal whose
// Ctrl+C never reaches the program, and nothing about the code reads wrong
// when that happens. Each test runs a real shell in a real pty and asks it
// questions a user would ask — "do you think you are a tty", "how big are
// you", "did Ctrl+C stop that loop".
//
// The tests skip themselves on a platform with no pty, so the suite stays
// green on a host that cannot run them while still covering the platform the
// IDE actually ships on.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>
#include <QRegularExpression>

#include <functional>
#include <memory>

#include <gtest/gtest.h>

#include <unistd.h>

#include "LightCpp/Workbench/PtyProcess.hpp"
#include "LightCpp/Workbench/PtySession.hpp"

using LightCpp::Workbench::Terminal::PtyCommand;
using LightCpp::Workbench::Terminal::PtyProcess;

namespace {

/// Collect output until `want` matches, the process finishes, or time runs
/// out. The loop is quit from the *data handler*, not by polling: a polling
/// loop with a short timer and no event source spins a CPU and, worse, makes a
/// hanging child look like a slow test rather than a broken one.
///
/// The handlers capture locals of this frame, so the data handler — which
/// appends to a local QString — must not survive this call: a byte arriving a
/// moment later would write through a dangling pointer. It is detached before
/// returning, which is also why callers that need later output install their
/// own handler *after* this returns rather than before.
QString readUntil(PtyProcess& process, const QRegularExpression& want,
                  int timeoutMs = 5000, int* exitCode = nullptr,
                  const std::function<void(int)>& onFinish = {})
{
    // Every object the handlers touch is long-lived: they are invoked from a
    // queued relay that may run after this frame is gone, and a frame-local
    // QEventLoop would make that a dangling pointer.
    struct Wait {
        QString text;
        bool matched = false;
        QEventLoop loop;
    };
    const auto wait = std::make_shared<Wait>();
    process.setDataHandler([wait, want](const QByteArray& chunk) {
        wait->text += QString::fromUtf8(chunk);
        if (!wait->matched && want.match(wait->text).hasMatch()) {
            wait->matched = true;
            wait->loop.quit();
        }
    });
    process.setFinishHandler([wait, exitCode, onFinish](int code) {
        if (exitCode != nullptr) {
            *exitCode = code;
        }
        if (onFinish) {
            onFinish(code);
        }
        wait->loop.quit();
    });
    QTimer guard;
    guard.setSingleShot(true);
    guard.setInterval(timeoutMs);
    QObject::connect(&guard, &QTimer::timeout, &wait->loop, &QEventLoop::quit);
    guard.start();
    if (!wait->matched) {
        wait->loop.exec();
    }
    const QString text = wait->text;
    // Only the data handler is detached. The finish handler stays on purpose:
    // it captures `exitCode`, which the caller passes precisely because the
    // process may end after the last useful byte (Ctrl+C kills `sleep`, then
    // the shell prints nothing at all). A test that keeps a local `PtyProcess`
    // is safe because the destructor runs before that local goes out of
    // scope; a caller that outlives its locals must clear the handler first.
    process.setDataHandler({});
    return text;
}

QString shellPath()
{
    const QString fromEnvironment = QString::fromLocal8Bit(qgetenv("SHELL"));
    if (!fromEnvironment.isEmpty() && QFileInfo(fromEnvironment).isExecutable()) {
        return fromEnvironment;
    }
    for (const char* candidate : {"/bin/sh", "/bin/bash"}) {
        if (QFileInfo(QString::fromLatin1(candidate)).isExecutable()) {
            return QString::fromLatin1(candidate);
        }
    }
    return {};
}

/// A QCoreApplication for the event loop the tests drive. Created lazily
/// because gtest_main owns main() and most tests in this binary are pure and
/// do not need one. The dispatcher posts reader-thread work through
/// QMetaObject::invokeMethod, which needs an application object to have a
/// thread with a running event loop.
QCoreApplication* ensureApplication()
{
    static QCoreApplication* application = nullptr;
    if (application == nullptr) {
        static int argc = 1;
        static char name[] = "LightCppWorkbenchTests";
        static char* argv[] = {name, nullptr};
        application = new QCoreApplication(argc, argv);
    }
    return application;
}

}  // namespace

TEST(PtyProcess, ASupportedHostCanCreateOne)
{
    // If this fails the rest of the file would skip, and the terminal would
    // fall back to a pipe without anyone noticing.
    EXPECT_TRUE(PtyProcess::isSupported());
}

TEST(PtyProcess, TheChildBelievesItIsATerminal)
{
    if (!PtyProcess::isSupported() || shellPath().isEmpty()) {
        GTEST_SKIP() << "no pty or no shell on this host";
    }
    ensureApplication();
    PtyProcess process;
    PtyCommand command;
    command.program = shellPath();
    // `test -t 0` is the question every colour-capable program asks. A pipe
    // answers no, a pty answers yes, and that single bit is the difference
    // between a coloured `ls` and a plain one.
    command.arguments = {QStringLiteral("-c"),
                         QStringLiteral("test -t 0 && echo TTY-YES || echo TTY-NO")};
    ASSERT_TRUE(process.start(command, 80, 24)) << qPrintable(process.lastError());
    const QString output =
        readUntil(process, QRegularExpression(QStringLiteral("TTY-(YES|NO)")));
    EXPECT_TRUE(output.contains(QStringLiteral("TTY-YES"))) << output.toStdString();
}

TEST(PtyProcess, TheChildSeesTheSizeWeStartedItWith)
{
    if (!PtyProcess::isSupported() || shellPath().isEmpty()) {
        GTEST_SKIP() << "no pty or no shell on this host";
    }
    ensureApplication();
    PtyProcess process;
    PtyCommand command;
    command.program = shellPath();
    // `stty size` prints "rows columns". Getting the two the wrong way round
    // is the classic winsize bug: at 80x24 it still looks plausible.
    command.arguments = {QStringLiteral("-c"), QStringLiteral("stty size 2>/dev/null || echo none")};
    ASSERT_TRUE(process.start(command, 132, 43)) << qPrintable(process.lastError());
    const QString output =
        readUntil(process, QRegularExpression(QStringLiteral("(\\d+ \\d+|none)")));
    EXPECT_TRUE(output.contains(QStringLiteral("43 132"))) << output.toStdString();
}

TEST(PtyProcess, ResizingReachesTheChildWithoutBeingAsked)
{
    if (!PtyProcess::isSupported() || shellPath().isEmpty()) {
        GTEST_SKIP() << "no pty or no shell on this host";
    }
    ensureApplication();
    PtyProcess process;
    PtyCommand command;
    command.program = shellPath();
    // A shell that waits for the resize and then reports: this is the SIGWINCH
    // path a full-screen program relies on to re-lay itself out.
    command.arguments = {QStringLiteral("-c"),
                         QStringLiteral("sleep 0.3; stty size 2>/dev/null || echo none")};
    ASSERT_TRUE(process.start(command, 80, 24)) << qPrintable(process.lastError());
    process.resize(120, 40);
    const QString output =
        readUntil(process, QRegularExpression(QStringLiteral("(\\d+ \\d+|none)")));
    EXPECT_TRUE(output.contains(QStringLiteral("40 120"))) << output.toStdString();
}

TEST(PtyProcess, CtrlCStopsAProgramThatIsNotReadingInput)
{
    if (!PtyProcess::isSupported() || shellPath().isEmpty()) {
        GTEST_SKIP() << "no pty or no shell on this host";
    }
    ensureApplication();

    // What Ctrl+C must do is stop a program that is not reading its input.
    // `sleep 30` is that program: a pipe would keep the byte 0x03 in its
    // buffer and `sleep` would run to completion, while a terminal turns the
    // byte into SIGINT for the foreground process group.
    PtyProcess process;
    PtyCommand command;
    command.program = shellPath();
    command.arguments = {
        QStringLiteral("-c"),
        QStringLiteral("echo READY; sleep 30; echo DONE-$?")};
    ASSERT_TRUE(process.start(command, 80, 24)) << qPrintable(process.lastError());

    // Wait for the shell to reach `sleep` rather than sleeping for a guessed
    // interval: signalling before the fork would hit the shell itself, and
    // that is the one outcome this test would misread as success.
    const QString ready = readUntil(process, QRegularExpression(QStringLiteral("READY")));
    ASSERT_TRUE(ready.contains(QStringLiteral("READY"))) << ready.toStdString();

    int exitCode = 0;
    bool finished = false;
    QElapsedTimer elapsed;
    elapsed.start();
    process.write(QByteArray(1, '\x03'));
    // The shell very likely prints nothing at all in response, so the wait is
    // on the finish callback, not on a line of output.
    const QString after = readUntil(
        process, QRegularExpression(QStringLiteral("DONE-")), 5000, &exitCode,
        [&finished](int) { finished = true; });
    const qint64 ms = elapsed.elapsed();

    // How the shell reports a signalled child differs between dash, bash and
    // zsh, and a non-interactive shell is allowed to die on the same signal.
    // Neither is the IDE's contract. What is: the program stopped long before
    // its 30 seconds, and the session ended instead of hanging on a child that
    // ignores its input.
    EXPECT_LT(ms, 2000) << "Ctrl+C must stop sleep immediately, not time it out";
    EXPECT_FALSE(process.isRunning()) << after.toStdString();
    EXPECT_TRUE(finished) << "the IDE is owed a finish callback when Ctrl+C ends a session";
    EXPECT_NE(exitCode, 0) << "a session killed by Ctrl+C must not look like success: "
                           << after.toStdString();
}

TEST(PtyProcess, WithIsigOffCtrlCIsJustAByte)
{
    if (!PtyProcess::isSupported() || shellPath().isEmpty()) {
        GTEST_SKIP() << "no pty or no shell on this host";
    }
    ensureApplication();
    // The control for the test above. `stty -isig` turns off the terminal's
    // translation, so `cat` receives 0x03 as an ordinary character and keeps
    // copying it forever. If this test ever starts failing because 0x03 kills
    // the child, the Ctrl+C test is passing for the wrong reason — the pty is
    // delivering a signal that the IDE never asked for.
    PtyProcess process;
    PtyCommand command;
    command.program = shellPath();
    command.arguments = {QStringLiteral("-c"),
                         QStringLiteral("stty -isig; echo READY; cat; echo EXIT-$?")};
    ASSERT_TRUE(process.start(command, 80, 24)) << qPrintable(process.lastError());
    // Wait for `cat`, not for the echoed input: with echo on, the terminal
    // echoes the typed word before `stty -isig` has even run, and signalling
    // in that window exercises the wrong mode.
    const QString ready = readUntil(process, QRegularExpression(QStringLiteral("READY")));
    ASSERT_TRUE(ready.contains(QStringLiteral("READY"))) << ready.toStdString();
    process.write(QByteArrayLiteral("plain\n"));
    const QString echoed = readUntil(process, QRegularExpression(QStringLiteral("plain")));
    ASSERT_TRUE(echoed.contains(QStringLiteral("plain"))) << echoed.toStdString();
    process.write(QByteArray(1, '\x03'));
    // Nothing should happen, so the only thing that can happen is the guard
    // firing. A short guard is enough: the byte is either a signal or it is
    // not, and that is decided as it passes through the line discipline.
    const QString after =
        readUntil(process, QRegularExpression(QStringLiteral("EXIT-")), 600);
    EXPECT_TRUE(process.isRunning())
        << "with ISIG off, 0x03 is data and must not end the session: "
        << after.toStdString();
    EXPECT_FALSE(after.contains(QStringLiteral("EXIT-"))) << after.toStdString();
}

TEST(PtyProcess, AnExplicitSignalReachesAChildInItsOwnProcessGroup)
{
    if (!PtyProcess::isSupported() || shellPath().isEmpty()) {
        GTEST_SKIP() << "no pty or no shell on this host";
    }
    ensureApplication();
    PtyProcess process;
    PtyCommand command;
    command.program = shellPath();
    // The trap proves two things at once: the signal arrived as SIGINT rather
    // than as a byte, and it went to the group the child leads (kill(-pid)),
    // because a plain kill(pid) would have been re-reported by the shell.
    // The trap runs only once the shell has stopped waiting for `sleep`, so
    // the shell first prints a prompt character and the test waits for it —
    // signalling a non-interactive shell while it waits on a child is a
    // request the shell is allowed to honour by dying.
    command.arguments = {
        QStringLiteral("-c"),
        QStringLiteral("trap 'echo GOT-INT; exit 42' INT; "
                       "while :; do printf '.'; sleep 0.05; done")};
    ASSERT_TRUE(process.start(command, 80, 24)) << qPrintable(process.lastError());
    const QString ready = readUntil(process, QRegularExpression(QStringLiteral("\\.")));
    ASSERT_TRUE(ready.contains(QStringLiteral("."))) << ready.toStdString();
    process.sendSignal(LightCpp::Workbench::Terminal::TerminalSignal::Interrupt);
    int exitCode = 0;
    const QString output =
        readUntil(process, QRegularExpression(QStringLiteral("GOT-INT")), 5000, &exitCode);
    EXPECT_TRUE(output.contains(QStringLiteral("GOT-INT"))) << output.toStdString();
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 300) {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    EXPECT_FALSE(process.isRunning());
    EXPECT_EQ(exitCode, 42);
}

TEST(PtyProcess, ClosingASessionWhoseChildKeepsTalkingReturnsPromptly)
{
    if (!PtyProcess::isSupported() || shellPath().isEmpty()) {
        GTEST_SKIP() << "no pty or no shell on this host";
    }
    ensureApplication();
    // The reader thread blocks on the master. If stop() merely closed it, the
    // child — still alive and still writing — would keep the open file
    // description alive and the join would wait for output that never ends.
    // Closing a window must not hang on a busy program, so this test asks for
    // exactly that: a child that ignores SIGHUP (so SIGKILL is needed) and
    // floods the terminal the whole time.
    const QString started = QStringLiteral("trap '' HUP; echo READY; "
                                           "while :; do echo spinning; done");
    auto process = std::make_unique<PtyProcess>();
    PtyCommand command;
    command.program = shellPath();
    command.arguments = {QStringLiteral("-c"), started};
    ASSERT_TRUE(process->start(command, 80, 24)) << qPrintable(process->lastError());
    const QString ready = readUntil(*process, QRegularExpression(QStringLiteral("READY")));
    ASSERT_TRUE(ready.contains(QStringLiteral("READY"))) << ready.toStdString();
    // A short quiet moment is not needed: the point is that output is flowing.
    QElapsedTimer elapsed;
    elapsed.start();
    process.reset();  // destructor runs stop()
    const qint64 ms = elapsed.elapsed();
    EXPECT_LT(ms, 3000) << "closing a busy terminal took " << ms << " ms";
}

TEST(PtyProcess, TheSessionEndsWhenTheChildExits)
{
    if (!PtyProcess::isSupported() || shellPath().isEmpty()) {
        GTEST_SKIP() << "no pty or no shell on this host";
    }
    ensureApplication();
    ensureApplication();
    PtyProcess process;
    PtyCommand command;
    command.program = shellPath();
    command.arguments = {QStringLiteral("-c"), QStringLiteral("exit 7")};
    int exitCode = 12345;
    bool sawFinish = false;
    process.setFinishHandler([&](int code) {
        exitCode = code;
        sawFinish = true;
    });
    ASSERT_TRUE(process.start(command, 80, 24)) << qPrintable(process.lastError());
    // 5 s is generous: the reader thread has to see EOF, then the reap runs
    // on the next event-loop turn.
    QElapsedTimer timer;
    timer.start();
    while (!sawFinish && timer.elapsed() < 5000) {
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }
    EXPECT_TRUE(sawFinish);
    EXPECT_EQ(exitCode, 7);
    EXPECT_FALSE(process.isRunning());
}

TEST(PtyProcess, StartingWithNoProgramFailsAndSaysWhy)
{
    PtyProcess process;
    QString reason;
    process.setFailureHandler([&reason](const QString& text) { reason = text; });
    EXPECT_FALSE(process.start(PtyCommand{}, 80, 24));
    EXPECT_FALSE(reason.isEmpty());
    EXPECT_FALSE(process.isRunning());
}
