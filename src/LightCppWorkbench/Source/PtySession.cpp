// PtySession — the pure half. See the header for why it is split.

#include "LightCpp/Workbench/PtySession.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace LightCpp::Workbench::Terminal {

QStringList candidateShells(bool windows, const QByteArray& environmentShell)
{
    QStringList candidates;
    const QString configured = QString::fromLocal8Bit(environmentShell).trimmed();
    if (!configured.isEmpty()) {
#if defined(Q_OS_WIN)
        // On Windows the shell name is produced by the OS rather than a full
        // path; QFileInfo::isExecutable is always true for an existing file
        // there, so existence is the test that means anything.
        const QFileInfo info(configured);
        const bool usable = info.isAbsolute()
                                ? info.isFile()
                                : !QStandardPaths::findExecutable(configured).isEmpty();
#else
        const bool usable = QFileInfo(configured).isExecutable()
                            && QFileInfo(configured).isFile();
#endif
        if (usable) {
            candidates.append(configured);
        }
    }
    if (windows) {
        // pwsh before powershell: the newer one is where the fixes are, and
        // where a user's profile is most likely to be. cmd last, because it
        // is the only one with no job control at all.
        candidates << QStringLiteral("pwsh")
                   << QStringLiteral("powershell")
                   << QStringLiteral("cmd");
    } else {
        candidates << QStringLiteral("/bin/bash")
                   << QStringLiteral("/bin/zsh")
                   << QStringLiteral("/bin/sh");
    }
    // De-duplicate while keeping the order: the first entry is the default.
    QStringList unique;
    for (const QString& candidate : candidates) {
        if (!candidate.isEmpty() && !unique.contains(candidate)) {
            unique.append(candidate);
        }
    }
    return unique;
}

PtyCommand shellCommand(const QString& shellPath, bool login,
                        const QString& workingDirectory)
{
    PtyCommand command;
    command.program = shellPath;
    command.workingDirectory = workingDirectory;
    const QString name = QFileInfo(shellPath).fileName().toLower();

    if (name.startsWith(QLatin1String("cmd"))) {
        // cmd.exe has no -i and rejects unknown switches by printing usage and
        // exiting, which in a terminal reads as "the terminal crashed".
        return command;
    }
    if (name.startsWith(QLatin1String("pwsh")) || name.startsWith(QLatin1String("powershell"))) {
        command.arguments << QStringLiteral("-NoLogo");
        if (login) {
            command.arguments << QStringLiteral("-NoProfile");
        }
        return command;
    }
    // A POSIX shell. `-i` first: an explicit interactive flag is what makes
    // fish and a few others enable job control when their stdin has not been
    // recognised as a tty yet.
    command.arguments << QStringLiteral("-i");
    if (login) {
        // A login shell re-reads the profile scripts. That is what "open a
        // login shell" means, and it is why it is opt-in: a user who picked
        // /bin/bash did not ask for `.profile` to run a second time.
        command.arguments << QStringLiteral("-l");
    }
    return command;
}

QByteArray winsizePayload(int columns, int rows)
{
    const quint16 safeRows = quint16(rows < 0 ? 0 : (rows > 65535 ? 65535 : rows));
    const quint16 safeColumns = quint16(columns < 0 ? 0 : (columns > 65535 ? 65535 : columns));
    QByteArray payload(8, '\0');
    // struct winsize { unsigned short ws_row; ws_col; ws_xpixel; ws_ypixel; }
    // Little-endian on every platform this ships to; big-endian hosts would
    // need the swap, and the ioctl there would fail loudly rather than
    // silently transpose the two fields.
    payload[0] = char(safeRows & 0xff);
    payload[1] = char((safeRows >> 8) & 0xff);
    payload[2] = char(safeColumns & 0xff);
    payload[3] = char((safeColumns >> 8) & 0xff);
    return payload;
}

int controlCodeFor(QChar letter)
{
    const uint code = letter.toUpper().unicode();
    if (code >= 'A' && code <= 'Z') {
        return int(code) - int('@');  // Ctrl+A = 1
    }
    switch (code) {
    case '@': return 0;
    case '[': return 0x1b;
    case '\\': return 0x1c;
    case ']': return 0x1d;
    case '^': return 0x1e;
    case '_': return 0x1f;
    case '?': return 0x7f;
    case ' ': return 0;
    default: break;
    }
    return -1;
}

bool isSignalKey(int controlCode, TerminalSignal* which)
{
    switch (controlCode) {
    case 0x03:
        if (which != nullptr) {
            *which = TerminalSignal::Interrupt;
        }
        return true;
    case 0x1c:
        if (which != nullptr) {
            *which = TerminalSignal::Quit;
        }
        return true;
    case 0x1a:
        if (which != nullptr) {
            *which = TerminalSignal::Suspend;
        }
        return true;
    case 0x04:
        if (which != nullptr) {
            *which = TerminalSignal::Eof;
        }
        return true;
    default:
        return false;
    }
}

QStringList terminalEnvironment(const QString& term)
{
    QStringList environment;
    environment << QStringLiteral("TERM=%1").arg(term.isEmpty() ? QStringLiteral("xterm-256color")
                                                               : term);
    // The renderer handles truecolour SGR, so programs that ask before
    // emitting 24-bit colour should be told the truth.
    environment << QStringLiteral("COLORTERM=truecolor");
    // A terminal that is not 8-bit-clean corrupts a program's output; saying
    // so explicitly stops tools from substituting '?' for unknown glyphs.
    environment << QStringLiteral("NCURSES_NO_UTF8_ACS=1");
    return environment;
}

}  // namespace LightCpp::Workbench::Terminal
