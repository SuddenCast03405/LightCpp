// PtySession — a real pseudo-terminal, so the terminal in this IDE is a
// terminal (lightcpp.1.md §九 "tool.terminal", "tool.shell").
//
// The screen model and the renderer already existed. What they were fed was
// the raw byte stream of a QProcess: no tty, therefore
//
//   * `isatty(1)` is false, so `ls` stops colouring, `git` stops paging and
//     `python` decides it is not interactive;
//   * there is no window size, so a full-screen program draws at 80x24 and
//     the cursor ends up in the wrong place after the first resize;
//   * there are no signals, so Ctrl+C reaches the shell as a byte only when
//     the shell is reading — a program that is *computing* ignores it;
//   * input is line-buffered by the terminal driver convention the program
//     expects, which a pipe does not reproduce.
//
// Split in two, deliberately:
//
//   * `PtyCommand` (this file's pure half) decides *what* to run and *how* to
//     translate a window size to an ioctl payload. Both are where the bugs
//     are: a login shell that skips the user's profile, a `TERM` the system
//     terminfo does not know, rows and columns swapped in the winsize struct.
//   * `PtyProcess` (PtyProcess.cpp / PtyProcessWindows.cpp) owns the file
//     descriptors, the child and the reader thread. It is the part that
//     cannot be tested without an operating system.
//
// The split is the same one the rest of this repository uses, and it is why
// the interesting decisions can be asserted: `ls` colouring depends on TERM,
// `Ctrl+C` depends on the pty being the foreground process group, and `vim`
// on the size arriving before it paints.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::Terminal {

/// Everything needed to start one shell session.
struct PtyCommand {
    QString program;
    QStringList arguments;
    /// The working directory for the child. Empty means "inherit".
    QString workingDirectory;
    /// What the child sees as TERM.
    QString term = QStringLiteral("xterm-256color");
    /// Extra environment on top of the inherited one, "KEY=VALUE" each.
    /// Applied after TERM so a user override wins.
    QStringList environment;

    [[nodiscard]] bool isValid() const { return !program.trimmed().isEmpty(); }
};

/// The shells to try, in order, when the user asks for "the terminal" without
/// naming one: the login shell from the environment first (that is what the
/// user configured, and what their profile customises), then the platform's
/// usual suspects.
///
/// `$SHELL` is honoured only when the file is executable: a stale `$SHELL`
/// pointing at an uninstalled shell is a common way to get a terminal that
/// immediately exits, and the failure is indistinguishable from "the terminal
/// does not work".
[[nodiscard]] QStringList candidateShells(bool windows, const QByteArray& environmentShell);

/// The command for `candidate`, set up the way an interactive terminal runs a
/// shell:
///
///   * `-i` so job control and prompts are on even though stdin is not what
///     the shell considers a tty at startup (it is, but only after the
///     dup2 — being explicit costs nothing and fixes fish);
///   * `-l` for a login shell **only when it was named explicitly by the
///     user**, because a login shell re-reads `.profile` and a user who
///     asked for "bash" did not ask for a second round of profile scripts;
///   * `-NoLogo` for PowerShell, whose banner would otherwise be the first
///     thing on screen in every new terminal;
///   * no arguments at all for cmd.exe, which has no interactive flag and
///     rejects one.
[[nodiscard]] PtyCommand shellCommand(const QString& shellPath, bool login,
                                      const QString& workingDirectory);

/// The `winsize` struct's payload as the kernel expects it: four 16-bit
/// unsigned values — rows, columns, xpixels, ypixels — in that order.
///
/// Order is the whole point. Getting rows and columns the wrong way round
/// produces a terminal that looks plausible at 80x24 (both are 24-ish) and
/// garbled at any other size, which is exactly the kind of bug that survives
/// a casual look.
[[nodiscard]] QByteArray winsizePayload(int columns, int rows);

/// The byte a control key sends to the terminal. `Ctrl+@` is NUL, `Ctrl+A`
/// is 0x01 … `Ctrl+Z` is 0x1a, and `Ctrl+\\`, `Ctrl+]`, `Ctrl+^`, `Ctrl+_`
/// sit at 0x1c..0x1f. Returns -1 for a key that has no control code, so the
/// caller can fall back to sending the text.
[[nodiscard]] int controlCodeFor(QChar letter);

/// The signals a terminal offers through keystrokes, as text-named constants
/// so the caller does not include <csignal> just to write one of them.
enum class TerminalSignal {
    Interrupt,  ///< Ctrl+C → SIGINT to the foreground group
    Quit,       ///< Ctrl+\ → SIGQUIT
    Suspend,    ///< Ctrl+Z → SIGTSTP
    Eof,        ///< Ctrl+D → EOF on the master side
};

/// True when the byte is one of the four above. The caller sends the signal
/// *and* the byte: a shell reading line-edited input expects the byte, a
/// program that is computing expects the signal, and sending both is what a
/// real terminal does.
[[nodiscard]] bool isSignalKey(int controlCode, TerminalSignal* which);

/// The environment TERM and friends a child should start with. `COLORTERM`
/// is set to `truecolor` because the renderer really does handle 24-bit SGR,
/// and a program that asks first should get the truth.
[[nodiscard]] QStringList terminalEnvironment(const QString& term);

}  // namespace LightCpp::Workbench::Terminal
