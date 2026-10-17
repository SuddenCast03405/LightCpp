#pragma once

#include <functional>
#include <vector>

namespace LightCpp::App {

/// What one shutdown pass had to do, so the caller can decide what to say
/// about it instead of the work happening silently.
struct ProcessShutdownReport {
    /// Processes that were still running when the pass started.
    int running = 0;
    /// Of those, the ones that were gone once their wait expired.
    int killed = 0;
    /// Of those, the ones that outlived their wait. Not an error path: a
    /// process that will not die is a fact to report, not a reason to keep
    /// the window waiting.
    int stuck = 0;
};

/// A process as the shutdown policy sees it.
///
/// Three callables instead of a `QProcess*`, because the policy has to be
/// exercisable without starting a child process. That is not a stylistic
/// preference: on the machine this was written for, QProcess cannot create
/// its pipes at all ("CreateFile failed. (all pipe instances are in use.)"),
/// so a policy test that needed a real child could never run there.
struct StoppableProcess {
    /// Null means "nothing to stop here" — the entry is skipped.
    std::function<bool()> isRunning;
    std::function<void()> kill;
    std::function<void(int)> waitForFinished;
};

/// Stop a set of processes: kill every one that is running, then wait —
/// bounded — for each.
///
/// Killing before waiting is the whole point. `waitForFinished` returns as
/// soon as the process is gone, so issuing every kill up front lets all of
/// the children die at the same time; killing and waiting one process at a
/// time puts the *sum* of the waits on the critical path instead of the
/// longest one. On a window close that is the difference between a brief
/// pause and a window that stops answering for seconds — with two processes
/// waiting two seconds each, four.
///
/// Nothing here waits longer than `timeoutMs` per process, and a process that
/// is still alive afterwards is counted in the report and otherwise left
/// alone: whether that is worth telling the user is the caller's call.
[[nodiscard]] ProcessShutdownReport stopProcesses(
    const std::vector<StoppableProcess>& processes, int timeoutMs);

/// "Run this callback after `delayMs`" — the caller's event loop, passed in so
/// the policy below does not need a QTimer, and so a test can drive the timer
/// by hand instead of sleeping.
using DelayedScheduler = std::function<void(int delayMs, std::function<void()>)>;

/// Kill `process` if it is still running once `delayMs` has passed — and do
/// nothing now.
///
/// This is the non-blocking half of stopping a child, for the callers that must
/// not wait: a language server asked to shut down during an index takes a
/// second or more to leave, and blocking the GUI thread on it froze the window
/// on every Kit switch, on ticking Syntax Check off, and on closing (R2-25).
///
/// The process is told to leave by the caller, before this is called; all this
/// adds is "and if it is still there later, end it". The kill is issued from
/// the callback, so the caller's own thread is free in between; a process that
/// exits on its own in that window is left alone.
///
/// The callback runs at most once, however many times the scheduler misbehaves.
void killAfterGrace(const StoppableProcess& process, int delayMs,
                    const DelayedScheduler& scheduler);

}  // namespace LightCpp::App
