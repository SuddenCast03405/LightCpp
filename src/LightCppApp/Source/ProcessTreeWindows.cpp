#include "LightCpp/App/ProcessTree.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace LightCpp::App {
std::vector<ProcessEntry> snapshotProcesses()
{
    std::vector<ProcessEntry> processes;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return processes;
    PROCESSENTRY32W entry{};
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
    if (pid == 0) return true;
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process == nullptr) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool ended = TerminateProcess(process, 1) != 0;
    CloseHandle(process);
    return ended;
}

}  // namespace LightCpp::App
