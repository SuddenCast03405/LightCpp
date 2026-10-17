#include "LightCpp/App/ProcessTree.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>

namespace LightCpp::App {

namespace {

/// Indices into `processes` of everything below `rootPid`, parents before
/// children. One walk, so the two public queries cannot drift apart.
std::vector<std::size_t> descendantIndices(
    const std::vector<ProcessEntry>& processes, unsigned long rootPid)
{
    std::vector<std::size_t> found;
    if (rootPid == 0) {
        return found;
    }
    // The frontier is the set of parents whose children are still to be
    // collected; `visited` makes a reused-pid loop terminate instead of
    // growing forever.
    std::vector<unsigned long> frontier{rootPid};
    std::set<unsigned long> visited{rootPid};
    while (!frontier.empty()) {
        std::vector<unsigned long> next;
        for (std::size_t i = 0; i < processes.size(); ++i) {
            const ProcessEntry& process = processes[i];
            if (process.pid == 0 || visited.count(process.pid) != 0) {
                continue;
            }
            if (std::find(frontier.begin(), frontier.end(), process.parentPid)
                == frontier.end()) {
                continue;
            }
            visited.insert(process.pid);
            found.push_back(i);
            next.push_back(process.pid);
        }
        frontier = std::move(next);
    }
    return found;
}

}  // namespace

std::vector<unsigned long> processDescendants(
    const std::vector<ProcessEntry>& processes, unsigned long rootPid)
{
    const std::vector<std::size_t> indices = descendantIndices(processes, rootPid);
    std::vector<unsigned long> pids;
    pids.reserve(indices.size());
    for (const std::size_t index : indices) {
        pids.push_back(processes[index].pid);
    }
    return pids;
}

QString processImage(const std::vector<ProcessEntry>& processes, unsigned long pid)
{
    // Pid 0 is the idle process, which is never anybody's child and never
    // anything to kill; treating it as "listed" would make a watch with no
    // pid look alive.
    if (pid == 0) {
        return {};
    }
    for (const ProcessEntry& process : processes) {
        if (process.pid == pid) {
            return process.image;
        }
    }
    return {};
}

unsigned long descendantWithImage(const std::vector<ProcessEntry>& processes,
                                  unsigned long rootPid, const QString& imageLower)
{
    if (imageLower.isEmpty()) {
        return 0;
    }
    for (const std::size_t index : descendantIndices(processes, rootPid)) {
        if (processes[index].image == imageLower) {
            return processes[index].pid;
        }
    }
    return 0;
}

unsigned long watchedProgramPid(const std::vector<ProcessEntry>& processes,
                                unsigned long pinnedPid, unsigned long shellPid,
                                const QString& imageLower)
{
    if (imageLower.isEmpty()) {
        return 0;
    }
    if (pinnedPid != 0) {
        // The pinned pid is asked about first and on its own: it keeps
        // answering after the shell is gone, which is the whole point.
        return processImage(processes, pinnedPid) == imageLower ? pinnedPid : 0;
    }
    if (shellPid == 0) {
        return 0;
    }
    return descendantWithImage(processes, shellPid, imageLower);
}

std::vector<ProcessEntry> snapshotProcesses()
{
    std::vector<ProcessEntry> processes;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return processes;
    }
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            ProcessEntry process;
            process.pid = entry.th32ProcessID;
            process.parentPid = entry.th32ParentProcessID;
            process.image = QString::fromWCharArray(entry.szExeFile).toLower();
            processes.push_back(process);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return processes;
}

bool terminateProcess(unsigned long pid)
{
    // Nothing to stop is not a failure: the deadline for a process that has
    // already exited is that it has already exited.
    if (pid == 0) {
        return true;
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process == nullptr) {
        // Gone between the snapshot and now — which is what was asked for.
        // Anything else (access denied on a child started elevated) is a
        // process that is still there and will not be stopped from here.
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    const bool ended = TerminateProcess(process, 1) != 0;
    CloseHandle(process);
    return ended;
}

QStringList stopProcessTree(const std::vector<ProcessEntry>& processes,
                            unsigned long rootPid)
{
    QStringList stayed;
    const std::vector<unsigned long> victims =
        processDescendants(processes, rootPid);
    // Back to front: the walk names parents before children, and a parent
    // that is still alive while its child dies can start a replacement in the
    // same breath. Deepest first leaves nothing that could.
    for (auto it = victims.rbegin(); it != victims.rend(); ++it) {
        if (terminateProcess(*it)) {
            continue;
        }
        const QString image = processImage(processes, *it);
        stayed << (image.isEmpty() ? QString::number(*it) : image);
    }
    return stayed;
}

}  // namespace LightCpp::App
