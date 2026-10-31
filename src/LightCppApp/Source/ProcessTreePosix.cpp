#include "LightCpp/App/ProcessTree.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <csignal>
#include <unistd.h>

#if defined(Q_OS_MACOS)
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/types.h>
#endif

namespace LightCpp::App {

#if defined(Q_OS_LINUX)
std::vector<ProcessEntry> snapshotProcesses()
{
    std::vector<ProcessEntry> processes;
    const QDir proc(QStringLiteral("/proc"));
    const QStringList entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : entries) {
        bool ok = false;
        const unsigned long pid = name.toULong(&ok);
        if (!ok || pid == 0) continue;
        QFile stat(proc.filePath(name + QStringLiteral("/stat")));
        if (!stat.open(QIODevice::ReadOnly)) continue;
        const QByteArray line = stat.readAll();
        // comm is parenthesized and can contain spaces or closing parens.
        const qsizetype close = line.lastIndexOf(QByteArray(")"));
        if (close < 0) continue;
        const QList<QByteArray> fields = line.mid(close + 1).simplified().split(QByteArray(" "));
        if (fields.size() < 2) continue;
        bool parentOk = false;
        const unsigned long parent = fields.at(1).toULong(&parentOk);
        if (!parentOk) continue;
        QString image = QFileInfo(proc.filePath(name + QStringLiteral("/exe"))).symLinkTarget();
        if (image.isEmpty()) {
            QFile comm(proc.filePath(name + QStringLiteral("/comm")));
            if (comm.open(QIODevice::ReadOnly)) image = QString::fromLocal8Bit(comm.readAll()).trimmed();
        } else {
            image = QFileInfo(image).fileName();
        }
        processes.push_back({pid, parent, image.toLower()});
    }
    return processes;
}
#elif defined(Q_OS_MACOS)
std::vector<ProcessEntry> snapshotProcesses()
{
    std::vector<ProcessEntry> processes;
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) return processes;
    std::vector<pid_t> pids(static_cast<std::size_t>(bytes) / sizeof(pid_t) + 64);
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (bytes <= 0) return processes;
    const std::size_t count = static_cast<std::size_t>(bytes) / sizeof(pid_t);
    for (std::size_t i = 0; i < count; ++i) {
        const pid_t pid = pids[i];
        if (pid <= 0) continue;
        struct proc_bsdinfo info {};
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info))
            != static_cast<int>(sizeof(info))) {
            continue;
        }
        // pbi_name is short and may truncate a long executable name. Resolve
        // the full image path when permitted, then fall back to the kernel name.
        char executablePath[PROC_PIDPATHINFO_MAXSIZE] = {};
        QString image;
        if (proc_pidpath(pid, executablePath, sizeof(executablePath)) > 0) {
            image = QFileInfo(QString::fromLocal8Bit(executablePath)).fileName();
        }
        if (image.isEmpty()) {
            image = QString::fromLocal8Bit(info.pbi_name);
        }
        processes.push_back({static_cast<unsigned long>(info.pbi_pid),
                             static_cast<unsigned long>(info.pbi_ppid),
                             image.toLower()});
    }
    return processes;
}
#endif

bool terminateProcess(unsigned long pid)
{
    if (pid == 0) return true;
    if (::kill(static_cast<pid_t>(pid), SIGKILL) == 0) return true;
    return errno == ESRCH;
}

}  // namespace LightCpp::App
