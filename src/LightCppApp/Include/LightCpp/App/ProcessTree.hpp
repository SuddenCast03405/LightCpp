#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace LightCpp::App {

/// One process as returned by the host operating system's process snapshot.
///
/// The name is carried along because two questions are asked of the same
/// snapshot and neither can be answered by a pid alone: "which of these did
/// the shell start?" is about the image, and "is the program I watched still
/// that program?" is about the image too — Windows reuses pids, so a listed
/// pid may by now be somebody else's process.
struct ProcessEntry {
    unsigned long pid = 0;
    unsigned long parentPid = 0;
    /// Lower-cased file name, e.g. "a.exe" — the form the lookups compare
    /// against, so callers do not each re-fold the case.
    QString image;
};

/// Every process below `rootPid`, transitively — children, grandchildren and
/// so on — ordered so that a process always comes after its parent. The root
/// itself is never included, and an unknown root yields nothing.
///
/// Windows keeps no process tree; it keeps one parent id per process, and
/// that id is stale the moment the parent exits. Two things follow, and both
/// are why this is a loop instead of a filter on `parentPid`:
///
/// - Killing a shell does not kill what the shell started. `cmd /k
///   a.exe` leaves `a.exe` alive when `cmd.exe` is terminated, holding the
///   .exe the next build must write — which then fails to link with
///   `permission denied` and nothing on screen to explain it (R2-19).
/// - The relation is not necessarily a tree once ids are reused: a live
///   process can name a parent id that now belongs to one of its own
///   descendants. The walk therefore remembers what it has seen and stops
///   instead of spinning.
[[nodiscard]] std::vector<unsigned long> processDescendants(
    const std::vector<ProcessEntry>& processes, unsigned long rootPid);

/// The image name the snapshot carries for `pid`, or an empty string when it
/// does not list it — which is the answer both for "already gone" and for the
/// id never having existed.
[[nodiscard]] QString processImage(const std::vector<ProcessEntry>& processes,
                                   unsigned long pid);

/// The pid of the first descendant of `rootPid` whose image is `imageLower`
/// (already folded), or 0 when there is none.
///
/// Descendants rather than direct children: `start a.exe` and a batch file
/// put the program one level deeper, and the watch that only looked at direct
/// children reported such a program as already finished.
[[nodiscard]] unsigned long descendantWithImage(
    const std::vector<ProcessEntry>& processes, unsigned long rootPid,
    const QString& imageLower);

/// The pid a terminal run is still running under, or 0 when it is not.
///
/// Three inputs, because the answer changes shape as the run goes on:
///
/// - `pinnedPid` is what the watch latched onto earlier. While the snapshot
///   still shows that pid carrying `imageLower`, that is the answer — even
///   with no shell in sight. The shell dying is exactly what happens when
///   Shift+F5 stops a terminal, and reading that as "the program is
///   finished" is the half of R2-19 that re-enabled Run while the program
///   was still holding the executable.
/// - With nothing pinned, the program is looked for among the shell's
///   descendants (`start a.exe` and batch files put it a level deeper).
/// - A pinned pid that now carries a different image belongs to somebody
///   else: Windows reuses ids, and following one would keep Run disabled
///   until an unrelated process exited.
///
/// 0 therefore means either "already gone" or "not seen yet"; the caller
/// tells those apart by whether it had pinned something.
[[nodiscard]] unsigned long watchedProgramPid(
    const std::vector<ProcessEntry>& processes, unsigned long pinnedPid,
    unsigned long shellPid, const QString& imageLower);

// --- the operating system's half -------------------------------------------
//
// These live next to the policy rather than in the window that uses them, and
// the reason is the same one that made ProcessShutdown take callables: the
// window cannot be driven in a test here (no screenshot, no way to type
// Shift+F5), while a process tree can be started and inspected directly. The
// policy above is only worth what its input is worth, so the input is
// checkable too.

/// The process table as it is right now. Empty when no snapshot can be taken,
/// which every caller has to treat as "I cannot see" rather than "nothing is
/// running".
[[nodiscard]] std::vector<ProcessEntry> snapshotProcesses();

/// End one process. False when it is there and would not go: a child started
/// elevated answers access denied, and saying nothing about that is how
/// "stopped" became a lie in the first place (R2-19). A pid that is already
/// gone counts as ended — which is what the caller was asking for.
[[nodiscard]] bool terminateProcess(unsigned long pid);

/// Terminate everything below `rootPid` and return the images that refused.
///
/// Order is children first, then their parents, so a parent cannot put a
/// replacement on screen while it is being dealt with. The root itself is
/// left alone: the caller stops it through the QProcess that owns its
/// handle, and doing it here would make two reports for one death.
[[nodiscard]] QStringList stopProcessTree(
    const std::vector<ProcessEntry>& processes, unsigned long rootPid);

}  // namespace LightCpp::App
