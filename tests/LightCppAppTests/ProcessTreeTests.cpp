// ProcessTreeTests — who has to die when a terminal is stopped.
//
// The defect behind these tests (R2-19) is invisible in a screenshot:
// Shift+F5 in an embedded terminal prints "--- Stopping terminal ---", the
// shell goes away, the Run action lights up again, and the program is still
// running. What it holds is the .exe the next build has to write, so the
// follow-up F7 fails in the linker with `permission denied` and no
// explanation anywhere.
//
// So the assertions here are about the *set of pids* a snapshot yields when
// the shell is the root — the part that has to be right for the kill to be
// worth anything — and about the two ways a snapshot can lie: a stale parent
// id (the process it names is gone) and a reused one (the id now belongs to
// somebody else, possibly inside the very subtree being walked).
//
// The live-process regression uses the native process-creation API on
// Windows. QProcess' pipe setup is unavailable in some test environments
// ("CreateFile failed. (all pipe instances are in use.)"), even though the
// process table and native process APIs remain usable.

#include "LightCpp/App/ProcessTree.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QProcess>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using LightCpp::App::descendantWithImage;
using LightCpp::App::ProcessEntry;
using LightCpp::App::processDescendants;
using LightCpp::App::processImage;
using LightCpp::App::snapshotProcesses;
using LightCpp::App::stopProcessTree;
using LightCpp::App::terminateProcess;
using LightCpp::App::watchedProgramPid;

namespace {

ProcessEntry entry(unsigned long pid, unsigned long parentPid, const char* image)
{
    return ProcessEntry{pid, parentPid, QString::fromLatin1(image)};
}

/// Sorted pids, so an assertion talks about the set and not the walk's order.
std::vector<unsigned long> sorted(std::vector<unsigned long> pids)
{
    std::sort(pids.begin(), pids.end());
    return pids;
}

/// The shape R2-19 is about: cmd.exe /k started a.exe, and a.exe started a
/// worker of its own.
std::vector<ProcessEntry> shellWithProgram()
{
    return {
        entry(4, 0, "system"),
        entry(640, 4, "explorer.exe"),
        entry(1000, 640, "lightcpp.exe"),
        entry(1200, 1000, "cmd.exe"),      // the shell
        entry(1300, 1200, "a.exe"),        // the program typed into it
        entry(1310, 1300, "worker.exe"),   // something a.exe started
        entry(1400, 640, "notepad.exe"),   // unrelated, same depth as cmd
    };
}

/// A real shell with a real child under it, started detached.
///
/// Detached because that is the only way this machine can start a child at
/// all: QProcess' capture channels cannot be created here ("CreateFile
/// failed. (all pipe instances are in use.)"), which is the reason the rest of
/// this file works from snapshots rather than from live processes. The child
/// is `ping` rather than a sleep so it needs no stdin, and its output goes to
/// nul so an invalid stdout handle cannot end it early.
///
/// `%SystemRoot%` rather than a bare `ping`: this suite's PATH is not the
/// user's (the test runner replaces it), so a program looked up by name is
/// not necessarily found — cmd then prints "not recognized" and exits, and
/// the test would report the tree as empty for a reason that has nothing to
/// do with the code under test.
///
/// The returned value is the shell's pid only: `ping` is not on the process
/// table yet when startDetached returns, so the child is looked for by
/// waitForImage(). 0 means this machine would not start one, and the tests
/// skip instead of failing.
QString testChildImage()
{
#ifdef Q_OS_WIN
    return QStringLiteral("ping.exe");
#else
    return QStringLiteral("sleep");
#endif
}

unsigned long startShellWithProgram()
{
#ifdef Q_OS_WIN
    // Avoid QProcess here: its pipe setup can fail on otherwise usable
    // Windows test runners. Create the persistent shell directly, then have
    // cmd's START builtin launch ping as an independent process.
    wchar_t systemDirectory[MAX_PATH]{};
    const UINT systemDirectoryLength =
        GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (systemDirectoryLength == 0 ||
        systemDirectoryLength >= MAX_PATH) {
        return 0;
    }

    std::wstring commandProcessor(systemDirectory,
                                  systemDirectoryLength);
    commandProcessor += L"\\cmd.exe";
    std::wstring commandLine =
        L"\"" + commandProcessor +
        L"\" /d /k start \"\" /b \"%SystemRoot%\\System32\\ping.exe\" "
        L"-n 30 127.0.0.1 >nul";
    const std::wstring workingDirectory =
        QDir::toNativeSeparators(QDir::tempPath()).toStdWString();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(commandProcessor.c_str(), commandLine.data(),
                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, workingDirectory.c_str(),
                        &startup, &process)) {
        return 0;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<unsigned long>(process.dwProcessId);
#else
    qint64 pid = 0;
    const bool started = QProcess::startDetached(
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"), QStringLiteral("sleep 30 & wait")},
        QDir::tempPath(), &pid);
    return started && pid > 0 ? static_cast<unsigned long>(pid) : 0;
#endif
}
/// The pid of `imageLower` among `rootPid`'s descendants, waiting up to
/// `timeoutMs` for it to appear; 0 when it never does.
unsigned long waitForImage(unsigned long rootPid, const QString& imageLower,
                           int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        const unsigned long found =
            descendantWithImage(snapshotProcesses(), rootPid, imageLower);
        if (found != 0) {
            return found;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return 0;
}

/// Whether `pid` has left the process table within `timeoutMs`.
bool waitUntilGone(unsigned long pid, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (processImage(snapshotProcesses(), pid).isEmpty()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

TEST(ProcessDescendants, AShellsProgramAndItsChildrenAreAllBelowTheShell)
{
    // The whole point: the shell's descendants are not just its direct
    // children. Killing only the shell leaves a.exe and worker.exe running.
    const std::vector<unsigned long> pids = sorted(processDescendants(shellWithProgram(), 1200));
    EXPECT_EQ(pids, (std::vector<unsigned long>{1300, 1310}));
}

TEST(ProcessDescendants, NothingUnrelatedIsIncluded)
{
    // notepad.exe is two levels above the shell, lightcpp.exe is the shell's
    // parent, and neither has anything to do with stopping the terminal.
    const std::vector<unsigned long> pids = processDescendants(shellWithProgram(), 1200);
    EXPECT_EQ(pids.size(), 2u);
    EXPECT_EQ(std::count(pids.begin(), pids.end(), 1400ul), 0);
    EXPECT_EQ(std::count(pids.begin(), pids.end(), 1000ul), 0);
}

TEST(ProcessDescendants, TheRootItselfIsNeverIncluded)
{
    // The caller kills the root separately (it is the process it holds);
    // counting it twice would report one process killed twice.
    const std::vector<unsigned long> pids = processDescendants(shellWithProgram(), 1200);
    EXPECT_EQ(std::count(pids.begin(), pids.end(), 1200ul), 0);
}

TEST(ProcessDescendants, AnUnknownRootYieldsNothing)
{
    EXPECT_TRUE(processDescendants(shellWithProgram(), 99).empty());
    // ... and so does pid 0, the idle process that is never anybody's parent.
    EXPECT_TRUE(processDescendants(shellWithProgram(), 0).empty());
}

TEST(ProcessDescendants, AStaleParentIdDoesNotDragInTheProcessItNowNames)
{
    // worker.exe's parent (a.exe) has exited: its id is still written down,
    // but nothing in the snapshot carries it. A filter on "parent id is in
    // the answer" would happily continue from here; the walk must not, or an
    // unrelated process that inherited the id would be killed with it.
    const std::vector<ProcessEntry> processes{
        entry(1200, 1000, "cmd.exe"),
        entry(1350, 1300, "worker.exe"),  // 1300 is gone
        entry(1360, 1350, "later.exe"),
    };
    EXPECT_TRUE(processDescendants(processes, 1200).empty());
}

TEST(ProcessDescendants, AReusedParentIdCannotMakeTheWalkLoop)
{
    // Windows reuses pids, so the parent relation can point back into the
    // subtree: 700 says its parent is 701, and 701 says its parent is 700.
    // Reached from the shell, both are descendants — and the walk has to
    // stop after collecting them instead of following the loop forever.
    const std::vector<ProcessEntry> processes{
        entry(1200, 1000, "cmd.exe"),
        entry(700, 1200, "a.exe"),
        entry(701, 700, "b.exe"),
        entry(702, 701, "c.exe"),
    };
    // a cycle back at 700
    std::vector<ProcessEntry> looped = processes;
    looped.push_back(entry(700, 702, "a.exe-again"));  // same id twice
    const std::vector<unsigned long> pids = sorted(processDescendants(looped, 1200));
    EXPECT_EQ(pids, (std::vector<unsigned long>{700, 701, 702}));
}

TEST(ProcessDescendants, ParentsComeBeforeTheirChildren)
{
    // Nothing in the kill path depends on the order (TerminateProcess is
    // immediate), but the order is claimed in the header, and a claim that
    // nobody checks is a claim that rots: walking children first would also
    // be a defensible policy, so the choice has to be written down once.
    const std::vector<ProcessEntry> processes{
        entry(1200, 1000, "cmd.exe"),
        entry(1300, 1200, "a.exe"),
        entry(1310, 1300, "worker.exe"),
    };
    const std::vector<unsigned long> pids = processDescendants(processes, 1200);
    ASSERT_EQ(pids.size(), 2u);
    EXPECT_EQ(pids[0], 1300ul);
    EXPECT_EQ(pids[1], 1310ul);
}

TEST(ProcessImage, AnswersTheImageTheSnapshotCarries)
{
    EXPECT_EQ(processImage(shellWithProgram(), 1200), QStringLiteral("cmd.exe"));
}

TEST(ProcessImage, AnEmptyStringMeansGone)
{
    // This is the question the terminal watch asks every 300 ms: "is the pid
    // I pinned still the program it was?" — 0 being not listed, and 0 being
    // also the answer for pid 0 itself.
    EXPECT_TRUE(processImage(shellWithProgram(), 9999).isEmpty());
    EXPECT_TRUE(processImage(shellWithProgram(), 0).isEmpty());
}

TEST(ProcessImage, APidThatNowBelongsToSomebodyElseAnswersWithThatName)
{
    // The caller compares against the name it is watching, so a reused id
    // reads as "the program is gone" instead of keeping Run disabled until
    // an unrelated process happens to exit.
    const std::vector<ProcessEntry> processes{entry(1300, 1200, "other.exe")};
    EXPECT_EQ(processImage(processes, 1300), QStringLiteral("other.exe"));
    EXPECT_NE(processImage(processes, 1300), QStringLiteral("a.exe"));
}

TEST(DescendantWithImage, FindsTheProgramTheShellStarted)
{
    EXPECT_EQ(descendantWithImage(shellWithProgram(), 1200,
                                  QStringLiteral("a.exe")),
              1300ul);
}

TEST(DescendantWithImage, FindsAGrandchildSuchAsStartOrABatchFile)
{
    // `start a.exe` and a .bat wrapper both put the program one level deeper.
    // The old watch looked only at direct children, so it reported such a
    // program as already finished and re-enabled Run while it ran.
    EXPECT_EQ(descendantWithImage(shellWithProgram(), 1200,
                                  QStringLiteral("worker.exe")),
              1310ul);
}

TEST(DescendantWithImage, NotInTheSubtreeIsNotFound)
{
    // notepad.exe exists in the snapshot but is not below the shell; running
    // something that resembles another window's process must not be mistaken
    // for the program this terminal started.
    EXPECT_EQ(descendantWithImage(shellWithProgram(), 1200,
                                  QStringLiteral("notepad.exe")),
              0ul);
}

TEST(DescendantWithImage, AnEmptyImageMatchesNothing)
{
    // The guard that keeps a watch with no target from latching onto the
    // first child (in cmd /k, that would be whatever the user typed).
    EXPECT_EQ(descendantWithImage(shellWithProgram(), 1200, QString()), 0ul);
    EXPECT_EQ(descendantWithImage(shellWithProgram(), 0, QStringLiteral("a.exe")),
              0ul);
}

// ---------------------------------------------------------------------------
// The same three questions, asked of the real machine.
//
// The snapshots above are the policy's input; these tests are about the
// input being real. Both halves are needed: a correct walk over a table
// nobody can fill in is worth nothing, and Toolhelp32 is exactly the kind of
// call that fails quietly (an empty snapshot reads as "nothing is running").
// ---------------------------------------------------------------------------

TEST(ProcessTreeOnRealProcesses, TerminatingTheShellLeavesItsProgramRunning)
{
    // The defect itself, reproduced rather than described. Stop a shell the
    // way QProcess::kill() does, then look at what is left: the program it
    // started is still on the table, still holding whatever it opened — in
    // the IDE that is the .exe the next build has to write, which is why
    // Shift+F5 followed by F7 produced a linker error with no explanation.
    const unsigned long shell = startShellWithProgram();
    if (shell == 0) {
        GTEST_SKIP() << "no detached process can be started here (the table "
                        "has " << snapshotProcesses().size() << " entries)";
    }
    const unsigned long program =
        waitForImage(shell, testChildImage(), 5000);
    if (program == 0) {
        (void)terminateProcess(shell);  // nothing observed, still ours to stop
        GTEST_SKIP() << "the shell (" << shell << ") started no program to "
                        "observe; the table has "
                     << snapshotProcesses().size() << " entries";
    }

    EXPECT_TRUE(terminateProcess(shell));
    EXPECT_TRUE(waitUntilGone(shell, 5000));
    EXPECT_FALSE(processImage(snapshotProcesses(), program).isEmpty())
        << "a program that outlived its shell — the state Shift+F5 used to "
           "leave behind";

    // Hygiene: do not leak it into the next test.
    (void)terminateProcess(program);
    EXPECT_TRUE(waitUntilGone(program, 5000));
}

TEST(ProcessTreeOnRealProcesses, StoppingTheTreeEndsTheProgramWithTheShell)
{
    // The fix, on the real thing: walk the tree first, then the shell dies,
    // and nothing of it is left. A plain terminateProcess() here would leave
    // the program running (the test above proves it does).
    const unsigned long shell = startShellWithProgram();
    if (shell == 0) {
        GTEST_SKIP() << "no detached process can be started here (the table "
                        "has " << snapshotProcesses().size() << " entries)";
    }
    const unsigned long program =
        waitForImage(shell, testChildImage(), 5000);
    if (program == 0) {
        (void)terminateProcess(shell);  // nothing observed, still ours to stop
        GTEST_SKIP() << "the shell (" << shell << ") started no program to "
                        "observe; the table has "
                     << snapshotProcesses().size() << " entries";
    }

    const QStringList stayed = stopProcessTree(snapshotProcesses(), shell);
    EXPECT_TRUE(stayed.isEmpty()) << "refused to stop: "
                                  << stayed.join(QStringLiteral(", ")).toStdString();
    EXPECT_TRUE(terminateProcess(shell));
    EXPECT_TRUE(waitUntilGone(shell, 5000));
    EXPECT_TRUE(waitUntilGone(program, 5000))
        << "the program outlived the tree kill; it would have kept the "
           "executable locked";
}

// ---------------------------------------------------------------------------
// The watch: which pid is still running this program.
//
// This is the half of R2-19 that had nothing to do with killing anything —
// the poll answered "the program is finished" from the shell's exit, so the
// Run action came back on while the program was still there. The decision is
// a function of one snapshot (ProcessTree::watchedProgramPid) so it can be
// asked these questions without a shell in the way.
// ---------------------------------------------------------------------------

TEST(WatchedProgramPid, APinnedPidIsStillTheProgramAfterTheShellIsGone)
{
    // The table after Shift+F5: cmd.exe is gone, a.exe is not. This is the
    // assertion the old code failed — it only ever looked below the shell,
    // and with no shell there was nothing to look below, so the program was
    // reported finished.
    const std::vector<ProcessEntry> processes{
        entry(640, 4, "explorer.exe"),
        entry(1300, 1200, "a.exe"),  // 1200 (the shell) has exited
    };
    EXPECT_EQ(watchedProgramPid(processes, 1300, 0, QStringLiteral("a.exe")),
              1300ul);
}

TEST(WatchedProgramPid, APinnedPidThatNowCarriesAnotherImageCountsAsGone)
{
    // Pids are reused: 1300 is on the table, but it is not the program any
    // more. Following it would keep Run disabled until that stranger exited.
    const std::vector<ProcessEntry> processes{entry(1300, 1200, "other.exe")};
    EXPECT_EQ(watchedProgramPid(processes, 1300, 0, QStringLiteral("a.exe")),
              0ul);
}

TEST(WatchedProgramPid, WithoutAPinTheProgramIsLookedForUnderTheShell)
{
    // The first tick after the run: nothing pinned yet, so the answer comes
    // from the tree.
    EXPECT_EQ(watchedProgramPid(shellWithProgram(), 0, 1200, QStringLiteral("a.exe")),
              1300ul);
}

TEST(WatchedProgramPid, AGrandchildIsFoundToo)
{
    // `start a.exe` and .bat wrappers put the program a level deeper; the
    // watch used to look only at direct children and call those runs over.
    EXPECT_EQ(watchedProgramPid(shellWithProgram(), 0, 1200,
                                QStringLiteral("worker.exe")),
              1310ul);
}

TEST(WatchedProgramPid, NoShellAndNoPinWatchesNothing)
{
    EXPECT_EQ(watchedProgramPid(shellWithProgram(), 0, 0, QStringLiteral("a.exe")),
              0ul);
}

TEST(WatchedProgramPid, AnEmptyImageWatchesNothing)
{
    // A watch with no target must not latch onto the first process it sees.
    EXPECT_EQ(watchedProgramPid(shellWithProgram(), 0, 1200, QString()), 0ul);
    EXPECT_EQ(watchedProgramPid(shellWithProgram(), 1300, 1200, QString()), 0ul);
}

TEST(WatchedProgramPid, NotSeenYetIsIndistinguishableFromGoneInTheTable)
{
    // Both answer 0; the caller tells them apart by whether it had pinned
    // something, which is why the grace window is in the caller and not
    // here. Written down because "0 means gone" would be the wrong reading
    // of this function and would drop the grace window.
    EXPECT_EQ(watchedProgramPid(shellWithProgram(), 0, 1200, QStringLiteral("b.exe")),
              0ul);
    const std::vector<ProcessEntry> withoutShell{entry(640, 4, "explorer.exe")};
    EXPECT_EQ(watchedProgramPid(withoutShell, 1300, 0, QStringLiteral("a.exe")),
              0ul);
}
