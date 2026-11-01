// DebuggerControllerTests — what a failed debugger start leaves behind.
//
// The failure that matters here is the one with no visible symptom until the
// *next* session: a debugger that cannot be launched at all. QProcess
// reports FailedToStart from inside start(), and the controller's own error
// handler used to call stop() on the spot — which frees and clears process_
// while start() is still running on top of it. The line right after the
// signal (`if (process_->state() ...)`) then dereferenced a null pointer,
// and the process object that should have been dropped stayed assigned and
// connected, so a later session inherited its handlers.
//
// No real debugger is involved: a file named gdb.exe that is not a runnable
// image fails to launch on any machine, which is exactly the guarantee the
// suite needs — this machine cannot create QProcess' pipes either, so a
// "real" start could not be tested here at all.
//
// --- breakpoint removal ----------------------------------------------------
//
// The second half pins down what the dialogs below can and cannot do. Every
// fact in it was measured against the debuggers on this machine — cdb
// 10.0.19041.5609, lldb and gdb 17.2 from MSYS2 — driving the same
// two-function program built twice (cl.exe/PDB and g++/DWARF):
//
//   cdb   `bp `file:line`` prints *nothing* when it resolves, "breakpoint N
//         redefined" for a duplicate, and "Bp expression `...' could not be
//         resolved, adding deferred bp" when it does not. `bl` prints
//         " 0 e 00007ff7`1c866ea4 ... msvcprog!main+0x4" and `.bpcmds`
//         "bp0 0x00007ff7`1c866ea4 ;" — the id sits next to a symbol or an
//         address, never next to the location, so "which id is my file:line"
//         has no answer to read. `bc 1` removed exactly one breakpoint;
//         `bc 0x1` is a syntax error ("Syntax error in 'bc 0x1'").
//   lldb  `breakpoint delete main.c.7` answered "0 breakpoints deleted;
//         0 breakpoint locations disabled." and left the breakpoint armed —
//         which is the command this controller used to send. `breakpoint
//         delete -f` answered "All breakpoints removed. (2 breakpoints)".
//   gdb   `clear file:line` deletes by location, so it keeps that form.
//
// Since cdb and lldb cannot delete one breakpoint from a location, a removal
// there is "clear everything, re-arm the rest" — and the rest has to be
// exactly right, or removing one breakpoint silently disarms the others.

#include "LightCpp/App/DebuggerController.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QObject>
#include <QStringList>
#include <QTemporaryDir>

#include <string>
#include <utility>

using LightCpp::App::DebugBreakpoint;
using LightCpp::App::DebuggerController;

namespace {

/// Writes `bytes` to `path`; false when it could not be written.
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

/// A debugger that exists (so the controller's checks pass) but cannot run
/// (so CreateProcess fails), plus a target that exists.
struct FailingLaunch {
    QTemporaryDir dir;
    QString debuggerPath;
    QString targetPath;
    bool ready = false;

    FailingLaunch()
    {
        debuggerPath = dir.filePath(QStringLiteral("gdb.exe"));
        targetPath = dir.filePath(QStringLiteral("program.exe"));
        ready = writeFile(debuggerPath, "not an executable image")
                && writeFile(targetPath, "not an executable image");
#ifndef Q_OS_WIN
        if (ready) {
            const QFile::Permissions permissions =
                QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
            ready = QFile::setPermissions(debuggerPath, permissions)
                    && QFile::setPermissions(targetPath, permissions);
        }
#endif
    }
};

/// Records the controller's signals for the duration of a test.
struct SignalRecorder {
    QStringList logs;
    int started = 0;
    int stopped = 0;
};

void connectRecorder(DebuggerController& controller, SignalRecorder& recorder)
{
    QObject::connect(&controller, &DebuggerController::logLine,
                     [&recorder](const QString& line) { recorder.logs << line; });
    QObject::connect(&controller, &DebuggerController::started,
                     [&recorder] { ++recorder.started; });
    QObject::connect(&controller, &DebuggerController::stopped,
                     [&recorder] { ++recorder.stopped; });
}

}  // namespace

#if defined(Q_OS_UNIX)
/// Minimal MI-speaking shell used to test GDB async running records without
/// depending on a debugger installation or a real executable.
struct FakeGdb {
    QTemporaryDir dir;
    QString debuggerPath;
    QString targetPath;
    bool ready = false;

    FakeGdb()
    {
        debuggerPath = dir.filePath(QStringLiteral("gdb"));
        targetPath = dir.filePath(QStringLiteral("program"));
        const QByteArray script =
            "#!/bin/sh\n"
            "while IFS= read -r command; do\n"
            "  case \"$command\" in\n"
            "    -exec-run) printf '%s\\n' '*stopped,reason=\"breakpoint-hit\",frame={file=\"main.cpp\",line=\"3\"}' ;;\n"
            "    -exec-continue) printf '%s\\n' 'continue-command' '*running,thread-id=\"all\"' '*running,thread-id=\"all\"' ;;\n"
            "    -exec-next) printf '%s\\n' '*stopped,reason=\"exited-signalled\",signal-name=\"SIGTERM\"' ;;\n"
            "    -gdb-exit) exit 0 ;;\n"
            "  esac\n"
            "done\n";
        ready = writeFile(debuggerPath, script)
                && writeFile(targetPath, QByteArray("target"));
        const QFile::Permissions executable =
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        ready = ready && QFile::setPermissions(debuggerPath, executable)
                && QFile::setPermissions(targetPath, executable);
    }
};
#endif


TEST(DebuggerController, AFailedStartReportsFailureAndLeavesNoSession)
{
    FailingLaunch fixture;
    ASSERT_TRUE(fixture.ready);

    DebuggerController controller;
    controller.setDebuggerPath(fixture.debuggerPath);
    SignalRecorder recorder;
    connectRecorder(controller, recorder);

    // Before the fix this did not return at all: the error handler had
    // already called stop(), so process_ was null on the next line.
    EXPECT_FALSE(controller.start(fixture.targetPath, fixture.dir.path(), {}));
    EXPECT_FALSE(controller.isRunning());
    EXPECT_EQ(recorder.started, 0);
    // The session was announced before the launch, so its end is announced
    // too — listeners that key on stopped() must not be left waiting.
    EXPECT_EQ(recorder.stopped, 1);
    EXPECT_TRUE(recorder.logs.contains(QStringLiteral("debugger did not start")));
}

TEST(DebuggerController, ASecondStartIsNotSabotagedByTheFirstOne)
{
    FailingLaunch fixture;
    ASSERT_TRUE(fixture.ready);

    DebuggerController controller;
    controller.setDebuggerPath(fixture.debuggerPath);
    SignalRecorder recorder;
    connectRecorder(controller, recorder);

    // The failed start used to leave a dead QProcess assigned and connected,
    // and the next start() overwrote the member: whatever the corpse still
    // emitted was then handled as if it belonged to the new session.
    for (int attempt = 0; attempt < 3; ++attempt) {
        EXPECT_FALSE(controller.start(fixture.targetPath, fixture.dir.path(), {}));
        EXPECT_FALSE(controller.isRunning());
    }
    EXPECT_EQ(recorder.started, 0);
    EXPECT_EQ(recorder.stopped, 3);

    controller.stop();  // must be a no-op, not a second teardown
    EXPECT_FALSE(controller.isRunning());
    EXPECT_EQ(recorder.stopped, 3);
}

TEST(DebuggerController, RefusesToStartWithoutADebuggerOrATarget)
{
    FailingLaunch fixture;
    ASSERT_TRUE(fixture.ready);

    DebuggerController controller;
    SignalRecorder recorder;
    connectRecorder(controller, recorder);

    // Nothing configured yet.
    EXPECT_FALSE(controller.start(fixture.targetPath, fixture.dir.path(), {}));
    // A path that exists but maps to no known backend.
    controller.setDebuggerPath(fixture.targetPath);
    EXPECT_EQ(controller.backend(), DebuggerController::Backend::Unknown);
    EXPECT_FALSE(controller.start(fixture.targetPath, fixture.dir.path(), {}));
    // A usable debugger, but no target to debug.
    controller.setDebuggerPath(fixture.debuggerPath);
    EXPECT_EQ(controller.backend(), DebuggerController::Backend::Gdb);
    EXPECT_FALSE(controller.start(
        fixture.dir.filePath(QStringLiteral("missing.exe")), fixture.dir.path(), {}));

    // Each refusal is logged once, and none of them ever created a process.
    ASSERT_EQ(recorder.logs.size(), 3);
    EXPECT_TRUE(recorder.logs.at(0).startsWith(QStringLiteral("debugger not usable")));
    EXPECT_TRUE(recorder.logs.at(2).startsWith(QStringLiteral("debug target not found")));
    EXPECT_EQ(recorder.stopped, 0);  // nothing was announced, nothing to end
}

namespace {

using Backend = DebuggerController::Backend;

/// The three breakpoints the removal tests work on; `kMiddle` is the one that
/// gets removed, so a wrong re-arm list shows up as a missing or extra plant.
const DebugBreakpoint kFirst{"/proj/src/main.cpp", 6};    // line 7 to a debugger
const DebugBreakpoint kMiddle{"/proj/src/main.cpp", 7};   // line 8
const DebugBreakpoint kLast{"/proj/src/util.cpp", 11};    // line 12

/// Literal expectations, written out rather than rebuilt from the production
/// helpers: a test that asks the code under test what to expect agrees with it
/// even when both are wrong.
QString cdbPlant(const QString& location)
{
    return QStringLiteral("bp `%1`").arg(location);
}

QString lldbPlant(const QString& fileName, int oneBasedLine)
{
    return QStringLiteral("breakpoint set --file \"%1\" --line %2")
        .arg(fileName)
        .arg(oneBasedLine);
}

/// gtest has no printer for QString, so a failed comparison would show raw
/// UTF-16 code units ("<62-00>, <63-00>") instead of the command that was
/// actually written. Comparing std::string keeps the failure readable.
std::string text(const QString& value)
{
    return value.toStdString();
}

std::string text(const QStringList& commands)
{
    return commands.join(QStringLiteral("\n")).toStdString();
}


#if defined(Q_OS_UNIX)
TEST(DebuggerController, GdbRunningRecordsEmitOneResumedNotification)
{
    FakeGdb fake;
    ASSERT_TRUE(fake.ready);

    DebuggerController controller;
    controller.setDebuggerPath(fake.debuggerPath);
    int pausedCount = 0;
    int resumedCount = 0;
    int continueCommandCount = 0;
    QObject::connect(&controller, &DebuggerController::paused,
                     [&] { ++pausedCount; });
    QObject::connect(&controller, &DebuggerController::resumed,
                     [&] { ++resumedCount; });
    QObject::connect(&controller, &DebuggerController::logLine,
                     [&](const QString& line) {
                         if (line == QStringLiteral("continue-command")) {
                             ++continueCommandCount;
                         }
                     });

    ASSERT_TRUE(controller.start(fake.targetPath, fake.dir.path(), {}));

    QElapsedTimer timer;
    timer.start();
    while (pausedCount == 0 && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_EQ(pausedCount, 1);

    controller.cont();
    controller.cont();  // a second click before *running must be ignored
    timer.restart();
    while (resumedCount == 0 && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    // The fake debugger emits two *running records. Only the paused ->
    // running transition should be announced.
    timer.restart();
    while (timer.elapsed() < 100) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    EXPECT_EQ(resumedCount, 1);
    EXPECT_EQ(continueCommandCount, 1);
    controller.stop();
}
TEST(DebuggerController, GdbSignalExitIsNotReportedAsASourcePause)
{
    FakeGdb fake;
    ASSERT_TRUE(fake.ready);

    DebuggerController controller;
    controller.setDebuggerPath(fake.debuggerPath);
    int pausedCount = 0;
    QStringList logs;
    QObject::connect(&controller, &DebuggerController::paused,
                     [&] { ++pausedCount; });
    QObject::connect(&controller, &DebuggerController::logLine,
                     [&](const QString& line) { logs.append(line); });

    ASSERT_TRUE(controller.start(fake.targetPath, fake.dir.path(), {}));
    QElapsedTimer timer;
    timer.start();
    while (pausedCount == 0 && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_EQ(pausedCount, 1);

    // Changing the selected debugger only affects the next session; this
    // live process is still speaking GDB/MI and must receive -exec-next.
    controller.setDebuggerPath(fake.dir.filePath(QStringLiteral("cdb.exe")));
    EXPECT_EQ(controller.backend(), DebuggerController::Backend::Cdb);
    controller.stepOver();
    timer.restart();
    while (!logs.contains(QStringLiteral("target exited"))
           && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    EXPECT_TRUE(logs.contains(QStringLiteral("target exited")));
    EXPECT_EQ(pausedCount, 1);
    controller.stop();
}
#endif


}  // namespace

TEST(DebuggerController, ACdbRemovalTakesTheOneBreakpointAndReArmsTheOthers)
{
    // The defect this replaces: the removal command was a bare "bc *", so
    // deleting one breakpoint cleared every breakpoint in the session while
    // the editor kept showing the others as set.
    const QStringList commands = LightCpp::App::removeBreakpointCommands(
        Backend::Cdb, kMiddle, {kFirst, kLast});

    const QStringList expected{QStringLiteral("bc *"),
                               cdbPlant(QStringLiteral("/proj/src/main.cpp:7")),
                               cdbPlant(QStringLiteral("/proj/src/util.cpp:12"))};
    EXPECT_EQ(text(commands), text(expected));

    // Stated as the requirement rather than as a string comparison: every
    // breakpoint that was not removed is planted again afterwards.
    for (const DebugBreakpoint& kept : {kFirst, kLast}) {
        EXPECT_TRUE(commands.contains(
            cdbPlant(LightCpp::App::breakpointLocation(kept))));
    }
    EXPECT_FALSE(commands.contains(
        cdbPlant(LightCpp::App::breakpointLocation(kMiddle))));
}

TEST(DebuggerController, ACdbRemovalOfTheOnlyBreakpointIsJustTheClear)
{
    const QStringList commands = LightCpp::App::removeBreakpointCommands(
        Backend::Cdb, kMiddle, {});
    EXPECT_EQ(text(commands), std::string("bc *"));
}

TEST(DebuggerController, AGdbRemovalTakesTheOneLocationAndNothingElse)
{
    // GDB has delete-by-location, so it must not go anywhere near a
    // clear-everything command.
    const QStringList commands = LightCpp::App::removeBreakpointCommands(
        Backend::Gdb, kMiddle, {kFirst, kLast});
    EXPECT_EQ(text(commands),
              std::string("-interpreter-exec console \"clear /proj/src/main.cpp:8\""));
    EXPECT_FALSE(commands.join(QLatin1Char('|')).contains(QLatin1String("bc")));
    EXPECT_FALSE(commands.join(QLatin1Char('|')).contains(QLatin1String("delete")));
}

TEST(DebuggerController, ALldbRemovalDoesNotUseTheFileDotLineIdThatDoesNothing)
{
    // `breakpoint delete main.cpp.8` answers "0 breakpoints deleted" and
    // leaves the breakpoint armed — a removal that reports success in the log
    // while the breakpoint keeps hitting.
    const QStringList commands = LightCpp::App::removeBreakpointCommands(
        Backend::Lldb, kMiddle, {kFirst, kLast});

    EXPECT_EQ(text(commands.first()), std::string("breakpoint delete -f"));
    // A size assertion first: `at()` on a shorter list is undefined behaviour,
    // and a test that crashes instead of failing hides which line disagreed.
    ASSERT_EQ(commands.size(), 3);  // the clear plus one plant per survivor
    const QString joined = commands.join(QLatin1Char('\n'));
    EXPECT_FALSE(joined.contains(QStringLiteral("main.cpp.8")));
    EXPECT_EQ(text(commands.at(1)), text(lldbPlant(QStringLiteral("main.cpp"), 7)));
    EXPECT_EQ(text(commands.at(2)), text(lldbPlant(QStringLiteral("util.cpp"), 12)));
}

TEST(DebuggerController, TheReArmKeepsThePlantingOrder)
{
    // Order matters for reading the debug console, and a re-arm that reversed
    // it would look like the IDE had changed the user's list.
    const DebugBreakpoint third{"/proj/src/util.cpp", 30};
    const QStringList commands = LightCpp::App::removeBreakpointCommands(
        Backend::Cdb, kMiddle, {kFirst, kLast, third});

    ASSERT_EQ(commands.size(), 4);
    EXPECT_EQ(text(commands.at(1)), text(cdbPlant(QStringLiteral("/proj/src/main.cpp:7"))));
    EXPECT_EQ(text(commands.at(2)), text(cdbPlant(QStringLiteral("/proj/src/util.cpp:12"))));
    EXPECT_EQ(text(commands.at(3)), text(cdbPlant(QStringLiteral("/proj/src/util.cpp:31"))));
}

TEST(DebuggerController, ThePlantCommandSpeaksEachBackendDialect)
{
    using LightCpp::App::addBreakpointCommand;
    EXPECT_EQ(text(addBreakpointCommand(Backend::Gdb, kMiddle)),
              std::string("-break-insert \"/proj/src/main.cpp:8\""));
    EXPECT_EQ(text(addBreakpointCommand(Backend::Cdb, kMiddle)),
              std::string("bp `/proj/src/main.cpp:8`"));
    // LLDB resolves by file name, not by path.
    EXPECT_EQ(text(addBreakpointCommand(Backend::Lldb, kMiddle)),
              std::string("breakpoint set --file \"main.cpp\" --line 8"));
    EXPECT_TRUE(addBreakpointCommand(Backend::Unknown, kMiddle).isEmpty());
}

TEST(DebuggerController, ALocationIsOneBasedAndUsesForwardSlashes)
{
    // The API is 0-based like every other editor coordinate here; every
    // debugger counts lines from 1, and Windows paths reach them with forward
    // slashes (all three accept that form).
    const DebugBreakpoint native{QStringLiteral("E:\\proj\\src\\main.cpp"), 0};
    EXPECT_EQ(text(LightCpp::App::breakpointLocation(native)),
              std::string("E:/proj/src/main.cpp:1"));
}

TEST(DebuggerController, ABackendThatIsNotSupportedGetsNoRemovalCommands)
{
    EXPECT_TRUE(LightCpp::App::removeBreakpointCommands(
                    Backend::Unknown, kMiddle, {kFirst})
                    .isEmpty());
}


TEST(DebuggerController, LLDBFrameParsesWindowsDriveLetterAndColumn)
{
    QString file;
    int line = -1;
    EXPECT_TRUE(LightCpp::App::parseLldbFrameLocation(
        QStringLiteral("frame #0: 0x1234 app\x60main at C:\\Work Space\\src\\main.cpp:12:4"),
        file, line));
    EXPECT_EQ(file, QStringLiteral("C:/Work Space/src/main.cpp"));
    EXPECT_EQ(line, 11);
}

TEST(DebuggerController, LLDBFrameParsesPosixColonPathWithoutColumn)
{
    QString file;
    int line = -1;
    EXPECT_TRUE(LightCpp::App::parseLldbFrameLocation(
        QStringLiteral("frame #0: 0x1234 app\x60main at /tmp/work:folder/main.cpp:7"),
        file, line));
    EXPECT_EQ(file, QStringLiteral("/tmp/work:folder/main.cpp"));
    EXPECT_EQ(line, 6);
}

TEST(DebuggerController, LLDBFrameRejectsMissingOrInvalidLine)
{
    QString file;
    int line = -1;
    EXPECT_FALSE(LightCpp::App::parseLldbFrameLocation(
        QStringLiteral("frame #0: 0x1234 app\x60main at main.cpp"), file, line));
    EXPECT_FALSE(LightCpp::App::parseLldbFrameLocation(
        QStringLiteral("frame #0: 0x1234 app\x60main at main.cpp:0"), file, line));
}
