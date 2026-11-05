// ProcessShutdownTests — the policy behind stopping a window's children.
//
// The policy is three lines of loop, and its bugs are exactly the kind that
// never show up in a screenshot: the processes all go away, so the shutdown
// looks right; what differs is *when* the window stops answering while they
// do. Killing and waiting one process at a time makes the freeze the sum of
// the waits, killing them all first makes it the longest one.
//
// Which is why the interesting assertions here are about the *order* of the
// calls, not about their outcome. The fakes below record every call, so the
// log itself is the thing under test.
//
// None of this needs a child process, and that is on purpose: the machine
// this was written on cannot create QProcess' pipes at all, so anything that
// needed a real child could not be run here (see CodeFormatterProcess, which
// skips for the same reason).

#include "LightCpp/App/ProcessShutdown.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

using LightCpp::App::killAfterGrace;
using LightCpp::App::ProcessShutdownReport;
using LightCpp::App::StoppableProcess;
using LightCpp::App::stopProcesses;

namespace {

/// The calls the policy made, in order: "kill:a", "wait:a:2000".
using CallLog = std::vector<std::string>;

/// A stand-in process.
///
/// `stubborn` ones survive kill() — which is what a wedged build tool would
/// look like and the only reason the report has a `stuck` counter.
struct FakeProcess {
    std::string name;
    bool running = true;
    bool stubborn = false;
    CallLog* log = nullptr;

    /// The handle the policy is given for this process.
    StoppableProcess handle()
    {
        return StoppableProcess{
            [this] { return running; },
            [this] {
                log->push_back("kill:" + name);
                if (!stubborn) {
                    running = false;
                }
            },
            [this](int timeoutMs) {
                log->push_back("wait:" + name + ":" + std::to_string(timeoutMs));
            }};
    }
};

/// Index of the first entry with the given prefix, or -1.
int firstIndex(const CallLog& log, const std::string& prefix)
{
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i].rfind(prefix, 0) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// Index of the last entry with the given prefix, or -1.
int lastIndex(const CallLog& log, const std::string& prefix)
{
    for (int i = static_cast<int>(log.size()) - 1; i >= 0; --i) {
        if (log[static_cast<std::size_t>(i)].rfind(prefix, 0) == 0) {
            return i;
        }
    }
    return -1;
}

/// gtest prints a std::vector<std::string> poorly; a joined string reads.
std::string joined(const CallLog& log)
{
    std::string out;
    for (const std::string& entry : log) {
        if (!out.empty()) {
            out += ", ";
        }
        out += entry;
    }
    return out;
}

}  // namespace

TEST(ProcessShutdown, NothingRunningDoesNothing)
{
    CallLog log;
    FakeProcess a{"a", /*running=*/false, false, &log};
    FakeProcess b{"b", /*running=*/false, false, &log};

    const ProcessShutdownReport report = stopProcesses(
        {a.handle(), b.handle()}, 2000);

    EXPECT_EQ(report.running, 0);
    EXPECT_EQ(report.killed, 0);
    EXPECT_EQ(report.stuck, 0);
    // Not even a wait: there was nothing to wait for.
    EXPECT_TRUE(log.empty()) << joined(log);
}

TEST(ProcessShutdown, DeadProcessesAreNeverTouched)
{
    CallLog log;
    FakeProcess alive{"alive", /*running=*/true, false, &log};
    FakeProcess gone{"gone", /*running=*/false, false, &log};

    const ProcessShutdownReport report =
        stopProcesses({alive.handle(), gone.handle()}, 2000);

    EXPECT_EQ(report.running, 1);
    EXPECT_EQ(report.killed, 1);
    // Nothing in the log may mention the process that was already gone.
    EXPECT_EQ(lastIndex(log, "kill:gone"), -1) << joined(log);
    EXPECT_EQ(lastIndex(log, "wait:gone"), -1) << joined(log);
}

TEST(ProcessShutdown, EveryKillIsIssuedBeforeTheFirstWait)
{
    CallLog log;
    FakeProcess a{"a", true, false, &log};
    FakeProcess b{"b", true, false, &log};
    FakeProcess c{"c", true, false, &log};

    const ProcessShutdownReport report =
        stopProcesses({a.handle(), b.handle(), c.handle()}, 2000);

    // This is the assertion the whole policy exists for: with the waits in
    // between, the window would be unresponsive for 3 x 2000 ms instead of
    // ~2000 ms.
    EXPECT_EQ(lastIndex(log, "kill:"), 2) << joined(log);
    EXPECT_EQ(firstIndex(log, "wait:"), 3) << joined(log);
    EXPECT_EQ(report.running, 3);
    EXPECT_EQ(report.killed, 3);
    for (const char* name : {"a", "b", "c"}) {
        EXPECT_EQ(lastIndex(log, std::string("kill:") + name),
                  firstIndex(log, std::string("kill:") + name))
            << joined(log);
        EXPECT_EQ(lastIndex(log, std::string("wait:") + name),
                  firstIndex(log, std::string("wait:") + name))
            << joined(log);
    }
}

TEST(ProcessShutdown, NoProcessIsWaitedForTwice)
{
    CallLog log;
    FakeProcess a{"a", true, false, &log};
    FakeProcess b{"b", true, false, &log};

    const ProcessShutdownReport report =
        stopProcesses({a.handle(), b.handle()}, 2000);

    EXPECT_EQ(report.killed, 2);
    EXPECT_EQ(log, (CallLog{"kill:a", "kill:b", "wait:a:2000", "wait:b:2000"}));
}

TEST(ProcessShutdown, TheWaitGetsTheCallersBudget)
{
    CallLog log;
    FakeProcess alive{"alive", true, false, &log};

    const ProcessShutdownReport report =
        stopProcesses({alive.handle()}, 1234);

    EXPECT_EQ(report.killed, 1);
    // The budget is passed through unchanged: the *caller* owns the decision
    // of how long a close may block, and the report is what tells it whether
    // the budget was enough.
    EXPECT_EQ(log, (CallLog{"kill:alive", "wait:alive:1234"}));
}

TEST(ProcessShutdown, AStubbornProcessIsReportedNotHidden)
{
    CallLog log;
    FakeProcess a{"a", true, /*stubborn=*/false, &log};
    FakeProcess wedged{"wedged", true, /*stubborn=*/true, &log};
    FakeProcess c{"c", true, false, &log};

    const ProcessShutdownReport report =
        stopProcesses({a.handle(), wedged.handle(), c.handle()}, 2000);

    EXPECT_EQ(report.running, 3);
    EXPECT_EQ(report.killed, 2);
    EXPECT_EQ(report.stuck, 1);
    // The sweep still covered every process: one wedged child must not stop
    // the others from being stopped.
    EXPECT_EQ(lastIndex(log, "kill:"), 2) << joined(log);
    EXPECT_EQ(lastIndex(log, "wait:"), 5) << joined(log);
}

TEST(ProcessShutdown, TheReportAddsUp)
{
    CallLog log;
    FakeProcess a{"a", true, false, &log};
    FakeProcess b{"b", false, false, &log};
    FakeProcess wedged{"wedged", true, true, &log};

    const ProcessShutdownReport report =
        stopProcesses({a.handle(), b.handle(), wedged.handle()}, 500);

    EXPECT_EQ(report.killed + report.stuck, report.running);
    EXPECT_EQ(report.running, 2);
    EXPECT_LE(report.killed + report.stuck, 3);
}

TEST(ProcessShutdown, AnEntryWithNothingToCallIsSkipped)
{
    CallLog log;
    FakeProcess alive{"alive", true, false, &log};

    // A default-constructed entry has no callables at all. Treating it as a
    // process to stop would crash on the first call (main() builds these from
    // QProcess pointers, and a null one is exactly this shape).
    const ProcessShutdownReport report =
        stopProcesses({StoppableProcess{}, alive.handle()}, 2000);

    EXPECT_EQ(report.running, 1);
    EXPECT_EQ(report.killed, 1);
    EXPECT_EQ(log, (CallLog{"kill:alive", "wait:alive:2000"}));
}

TEST(ProcessShutdown, AnEmptySetIsANoOp)
{
    const ProcessShutdownReport report = stopProcesses({}, 2000);

    EXPECT_EQ(report.running, 0);
    EXPECT_EQ(report.killed, 0);
    EXPECT_EQ(report.stuck, 0);
}

// ---------------------------------------------------------------------------
// killAfterGrace — the half of stopping a child that must not wait (R2-25).
//
// A language server asked to leave during an index takes a second or more; the
// GUI thread cannot spend that. So the kill is handed to the event loop, and
// the assertion that matters is *when* it is issued: not during the call.
// The scheduler below stands in for that event loop and is driven by hand —
// no sleeping, no timers, no child process.
// ---------------------------------------------------------------------------

namespace {

/// Records what was scheduled, so a test can fire it when it likes.
struct FakeScheduler {
    int delay = -1;
    std::function<void()> callback;

    LightCpp::App::DelayedScheduler handle()
    {
        return [this](int delayMs, std::function<void()> fire) {
            delay = delayMs;
            callback = std::move(fire);
        };
    }
};

}  // namespace

TEST(KillAfterGrace, NothingIsKilledDuringTheCall)
{
    CallLog log;
    FakeProcess server{"clangd", /*running=*/true, false, &log};
    FakeScheduler scheduler;

    killAfterGrace(server.handle(), 500, scheduler.handle());

    // The process was asked to leave by its caller; this side only arranges
    // what happens if it does not. Killing now would be the block this replaces.
    EXPECT_TRUE(log.empty()) << joined(log);
    EXPECT_EQ(scheduler.delay, 500);
    EXPECT_NE(scheduler.callback, nullptr);
}

TEST(KillAfterGrace, TheKillHappensWhenTheGraceIsUp)
{
    CallLog log;
    FakeProcess server{"clangd", true, false, &log};
    FakeScheduler scheduler;

    killAfterGrace(server.handle(), 500, scheduler.handle());
    ASSERT_TRUE(scheduler.callback != nullptr);
    // Nothing yet: the grace is the point, and killing early is what the old
    // code did by waiting for the exit instead.
    EXPECT_TRUE(log.empty()) << joined(log);
    scheduler.callback();

    EXPECT_EQ(log, (CallLog{"kill:clangd"}));
}

TEST(KillAfterGrace, AServerThatLeftOnItsOwnIsNotKilled)
{
    CallLog log;
    FakeProcess server{"clangd", true, false, &log};
    FakeScheduler scheduler;

    killAfterGrace(server.handle(), 500, scheduler.handle());
    server.running = false;  // exited while the grace was running
    scheduler.callback();

    EXPECT_TRUE(log.empty()) << joined(log);
}

TEST(KillAfterGrace, FiringTwiceKillsOnce)
{
    CallLog log;
    FakeProcess server{"clangd", /*running=*/true, /*stubborn=*/true, &log};
    FakeScheduler scheduler;

    killAfterGrace(server.handle(), 500, scheduler.handle());
    scheduler.callback();
    scheduler.callback();

    // A repeated timeout must not send a second kill at whatever the first one
    // left behind.
    EXPECT_EQ(log, (CallLog{"kill:clangd"}));
}

TEST(KillAfterGrace, WithoutAnEventLoopTheChildIsNotLeftBehind)
{
    CallLog log;
    FakeProcess server{"clangd", true, false, &log};

    // No scheduler = nothing will ever come back to do it.
    killAfterGrace(server.handle(), 500, {});

    EXPECT_EQ(log, (CallLog{"kill:clangd"}));
}
