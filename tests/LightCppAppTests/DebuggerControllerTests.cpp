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

#include "LightCpp/App/DebuggerController.hpp"

#include <gtest/gtest.h>

#include <QFile>
#include <QObject>
#include <QStringList>
#include <QTemporaryDir>

#include <utility>

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
