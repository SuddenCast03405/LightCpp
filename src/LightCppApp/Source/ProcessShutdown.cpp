#include "LightCpp/App/ProcessShutdown.hpp"

namespace LightCpp::App {

ProcessShutdownReport stopProcesses(
    const std::vector<StoppableProcess>& processes, int timeoutMs)
{
    ProcessShutdownReport report;

    // Decide first, act second: the set must be fixed before the first kill
    // is issued, or a process that dies early would be waited for twice.
    std::vector<const StoppableProcess*> running;
    running.reserve(processes.size());
    for (const StoppableProcess& process : processes) {
        if (process.isRunning && process.isRunning()) {
            running.push_back(&process);
        }
    }
    report.running = static_cast<int>(running.size());

    // Every kill before any wait, so the children go down in parallel.
    for (const StoppableProcess* process : running) {
        if (process->kill) {
            process->kill();
        }
    }

    for (const StoppableProcess* process : running) {
        if (process->waitForFinished) {
            process->waitForFinished(timeoutMs);
        }
        // Ask the process itself rather than the wait's return value: with
        // QProcess underneath, waitForFinished() is false both for "timed
        // out" and for "was not running any more", so its answer cannot tell
        // a clean death from a child that refused to go.
        if (process->isRunning()) {
            ++report.stuck;
        } else {
            ++report.killed;
        }
    }

    return report;
}

}  // namespace LightCpp::App
