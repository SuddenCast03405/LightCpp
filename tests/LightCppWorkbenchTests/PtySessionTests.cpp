// PtySessionTests — the pure half of the terminal transport.
//
// Every assertion here is a decision a real terminal makes and a pipe does
// not: which shell to start, what flags make it interactive, which byte order
// the kernel expects, and which keystroke becomes a signal. Those are the
// things that are wrong in a terminal that "mostly works", so they are the
// things worth pinning down without an operating system in the way.

#include <QFileInfo>

#include <gtest/gtest.h>

#include "LightCpp/Workbench/PtySession.hpp"

using LightCpp::Workbench::Terminal::PtyCommand;
using LightCpp::Workbench::Terminal::TerminalSignal;
using LightCpp::Workbench::Terminal::candidateShells;
using LightCpp::Workbench::Terminal::controlCodeFor;
using LightCpp::Workbench::Terminal::isSignalKey;
using LightCpp::Workbench::Terminal::shellCommand;
using LightCpp::Workbench::Terminal::terminalEnvironment;
using LightCpp::Workbench::Terminal::winsizePayload;

// ---------------------------------------------------------------------------
// PtyCommand
// ---------------------------------------------------------------------------

TEST(PtyCommand, EmptyProgramIsNotValid)
{
    EXPECT_FALSE(PtyCommand{}.isValid());
    PtyCommand blank;
    blank.program = QStringLiteral("   ");
    EXPECT_FALSE(blank.isValid()) << "whitespace is not a program";
}

TEST(PtyCommand, AProgramIsEnoughToBeValid)
{
    PtyCommand command;
    command.program = QStringLiteral("/bin/bash");
    EXPECT_TRUE(command.isValid());
}

TEST(PtyCommand, DefaultTermIsTheOneTheRendererKnows)
{
    // xterm-256color is the terminfo every Unix ships and the one whose
    // capabilities match what TerminalWidget implements. A "better" TERM
    // would promise sequences the renderer cannot answer.
    EXPECT_EQ(PtyCommand{}.term, QStringLiteral("xterm-256color"));
}

// ---------------------------------------------------------------------------
// candidateShells
// ---------------------------------------------------------------------------

TEST(PtySessionShells, TheUsersShellComesFirstWhenItExists)
{
    const QString bash = QStringLiteral("/bin/sh");
    ASSERT_TRUE(QFileInfo(bash).isExecutable());
    const QStringList shells = candidateShells(false, bash.toLocal8Bit());
    ASSERT_FALSE(shells.isEmpty());
    EXPECT_EQ(shells.first(), bash)
        << "a user who set $SHELL asked for that shell, not for our favourite";
}

TEST(PtySessionShells, AStaleShellInTheEnvironmentIsIgnored)
{
    // The failure mode this prevents is nasty and common: $SHELL names a
    // shell the user uninstalled, the terminal opens and closes instantly,
    // and nothing anywhere says why.
    const QStringList shells =
        candidateShells(false, QByteArrayLiteral("/nonexistent/fish-9.9"));
    ASSERT_FALSE(shells.isEmpty());
    EXPECT_NE(shells.first(), QStringLiteral("/nonexistent/fish-9.9"));
    EXPECT_EQ(shells.first(), QStringLiteral("/bin/bash"));
}

TEST(PtySessionShells, AnEmptyEnvironmentStillYieldsAFallbackChain)
{
    const QStringList shells = candidateShells(false, QByteArray());
    EXPECT_EQ(shells, (QStringList{QStringLiteral("/bin/bash"),
                                   QStringLiteral("/bin/zsh"),
                                   QStringLiteral("/bin/sh")}));
}

TEST(PtySessionShells, WindowsPrefersPowerShellOverCmd)
{
    // cmd has no job control at all, which is what Ctrl+Z and "suspend the
    // program" need; pwsh is where the fixes are. The order is the feature.
    const QStringList shells = candidateShells(true, QByteArray());
    ASSERT_EQ(shells.size(), 3);
    EXPECT_EQ(shells.at(0), QStringLiteral("pwsh"));
    EXPECT_EQ(shells.at(1), QStringLiteral("powershell"));
    EXPECT_EQ(shells.at(2), QStringLiteral("cmd"));
}

TEST(PtySessionShells, ADuplicateIsNotOfferedTwice)
{
    // $SHELL=/bin/bash and the fallback list also contains /bin/bash.
    const QStringList shells = candidateShells(false, QByteArrayLiteral("/bin/bash"));
    EXPECT_EQ(shells.count(QStringLiteral("/bin/bash")), 1)
        << "the drop-down must not list one shell as two choices";
}

// ---------------------------------------------------------------------------
// shellCommand
// ---------------------------------------------------------------------------

TEST(PtySessionCommand, APosixShellIsInteractive)
{
    const PtyCommand command = shellCommand(QStringLiteral("/bin/bash"), false,
                                            QStringLiteral("/tmp"));
    EXPECT_EQ(command.program, QStringLiteral("/bin/bash"));
    EXPECT_EQ(command.arguments, QStringList{QStringLiteral("-i")});
    EXPECT_EQ(command.workingDirectory, QStringLiteral("/tmp"));
}

TEST(PtySessionCommand, OnlyAnExplicitLoginShellReadsTheProfileAgain)
{
    const PtyCommand interactive =
        shellCommand(QStringLiteral("/bin/zsh"), false, {});
    EXPECT_FALSE(interactive.arguments.contains(QStringLiteral("-l")));

    const PtyCommand login = shellCommand(QStringLiteral("/bin/zsh"), true, {});
    EXPECT_TRUE(login.arguments.contains(QStringLiteral("-i")));
    EXPECT_TRUE(login.arguments.contains(QStringLiteral("-l")));
}

TEST(PtySessionCommand, CmdGetsNoArgumentsAtAll)
{
    // cmd.exe prints its usage and exits on an unknown switch, which in a new
    // terminal tab looks exactly like a crash.
    for (const QString& name : {QStringLiteral("cmd"), QStringLiteral("cmd.exe"),
                                QStringLiteral("CMD.EXE")}) {
        const PtyCommand command = shellCommand(name, true, {});
        EXPECT_TRUE(command.arguments.isEmpty()) << qPrintable(name);
    }
}

TEST(PtySessionCommand, PowerShellIsQuietAndOnlyLoginSuppressesTheProfile)
{
    const PtyCommand command =
        shellCommand(QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe"), false, {});
    EXPECT_EQ(command.arguments, QStringList{QStringLiteral("-NoLogo")})
        << "-NoLogo hides the banner; the profile must still run";

    const PtyCommand login =
        shellCommand(QStringLiteral("powershell.exe"), true, {});
    EXPECT_TRUE(login.arguments.contains(QStringLiteral("-NoLogo")));
    EXPECT_TRUE(login.arguments.contains(QStringLiteral("-NoProfile")));
}

// ---------------------------------------------------------------------------
// winsizePayload
// ---------------------------------------------------------------------------

TEST(PtySessionWinsize, RowsComeBeforeColumns)
{
    // struct winsize { ws_row; ws_col; ws_xpixel; ws_ypixel; }. Swapping the
    // first two looks plausible at 80x24 (both are in the twenties) and is
    // obviously broken at any other size — the classic terminal bug.
    const QByteArray payload = winsizePayload(132, 43);
    ASSERT_EQ(payload.size(), 8);
    const auto rows = quint16(quint8(payload.at(0)) | (quint8(payload.at(1)) << 8));
    const auto columns = quint16(quint8(payload.at(2)) | (quint8(payload.at(3)) << 8));
    EXPECT_EQ(rows, 43);
    EXPECT_EQ(columns, 132);
}

TEST(PtySessionWinsize, PixelFieldsAreZeroWhenNobodyKnowsThem)
{
    // Reporting a made-up pixel size makes a program compute a font size from
    // it; zero means "no opinion", which is the truth for a grid of cells.
    const QByteArray payload = winsizePayload(80, 24);
    EXPECT_EQ(payload.mid(4), QByteArray(4, '\0'));
}

TEST(PtySessionWinsize, ASizeTooLargeForTheStructIsClampedNotWrapped)
{
    // 70000 & 0xffff would be 4464 — a terminal 30 times smaller than asked,
    // with no error anywhere.
    const QByteArray payload = winsizePayload(70000, 65536);
    const auto rows = quint16(quint8(payload.at(0)) | (quint8(payload.at(1)) << 8));
    const auto columns = quint16(quint8(payload.at(2)) | (quint8(payload.at(3)) << 8));
    EXPECT_EQ(rows, 65535);
    EXPECT_EQ(columns, 65535);
}

TEST(PtySessionWinsize, ANegativeSizeBecomesZeroRatherThanAHugePositive)
{
    const QByteArray payload = winsizePayload(-5, -1);
    EXPECT_EQ(payload, QByteArray(8, '\0'));
}

// ---------------------------------------------------------------------------
// controlCodeFor
// ---------------------------------------------------------------------------

TEST(PtySessionControl, LettersMapToTheirControlCodeCaseInsensitively)
{
    for (char letter = 'a'; letter <= 'z'; ++letter) {
        const int expected = letter - 'a' + 1;
        EXPECT_EQ(controlCodeFor(QChar(letter)), expected) << letter;
        EXPECT_EQ(controlCodeFor(QChar('A' + (letter - 'a'))), expected) << letter;
    }
}

TEST(PtySessionControl, TheFivePunctuationKeysSitWhereTheTerminalExpects)
{
    // These are the ones a lookup table written from memory usually gets
    // wrong; a terminal emulator sending the wrong byte is silent corruption.
    EXPECT_EQ(controlCodeFor(QChar('\\')), 0x1c);
    EXPECT_EQ(controlCodeFor(QChar(']')), 0x1d);
    EXPECT_EQ(controlCodeFor(QChar('^')), 0x1e);
    EXPECT_EQ(controlCodeFor(QChar('_')), 0x1f);
    EXPECT_EQ(controlCodeFor(QChar('?')), 0x7f);
}

TEST(PtySessionControl, AtAndSpaceAreNul)
{
    EXPECT_EQ(controlCodeFor(QChar('@')), 0);
    EXPECT_EQ(controlCodeFor(QChar(' ')), 0);
}

TEST(PtySessionControl, APrintableKeyHasNoControlCode)
{
    // Digits and the remaining punctuation: Ctrl+1 is not a thing on any
    // terminal, and sending 0 for it would inject a NUL into the shell.
    for (const char* key : {"1", "9", "=", ",", "."}) {
        EXPECT_EQ(controlCodeFor(QChar::fromLatin1(key[0])), -1) << key;
    }
}

// ---------------------------------------------------------------------------
// isSignalKey
// ---------------------------------------------------------------------------

TEST(PtySessionSignals, TheFourKeysThatAreSignals)
{
    struct Case {
        int code;
        TerminalSignal signal;
    };
    const Case cases[] = {{0x03, TerminalSignal::Interrupt},
                          {0x1c, TerminalSignal::Quit},
                          {0x1a, TerminalSignal::Suspend},
                          {0x04, TerminalSignal::Eof}};
    for (const Case& entry : cases) {
        TerminalSignal which = TerminalSignal::Suspend;
        EXPECT_TRUE(isSignalKey(entry.code, &which)) << entry.code;
        EXPECT_EQ(which, entry.signal) << entry.code;
    }
}

TEST(PtySessionSignals, EverythingElseIsJustAByte)
{
    for (const int code : {0x01, 0x05, 0x0d, 0x1b, 0x1d, 0x7f}) {
        TerminalSignal which = TerminalSignal::Interrupt;
        EXPECT_FALSE(isSignalKey(code, &which)) << code;
    }
}

TEST(PtySessionSignals, ANullOutPointerIsAllowed)
{
    // The caller often only wants the yes/no answer, e.g. when deciding
    // whether to warn before closing a session.
    EXPECT_TRUE(isSignalKey(0x03, nullptr));
    EXPECT_FALSE(isSignalKey(0x41, nullptr));
}

// ---------------------------------------------------------------------------
// terminalEnvironment
// ---------------------------------------------------------------------------

TEST(PtySessionEnvironment, TermAndTruecolourTravelWithTheChild)
{
    const QStringList environment = terminalEnvironment(QStringLiteral("xterm-256color"));
    EXPECT_TRUE(environment.contains(QStringLiteral("TERM=xterm-256color")));
    EXPECT_TRUE(environment.contains(QStringLiteral("COLORTERM=truecolor")))
        << "the renderer handles 24-bit SGR, so a program that asks gets the truth";
}

TEST(PtySessionEnvironment, AnEmptyTermFallsBackInsteadOfSettingTermToNothing)
{
    // TERM= (empty) makes ncurses refuse to start; no variable at all makes it
    // guess. Neither is what the user wants.
    const QStringList environment = terminalEnvironment(QString());
    EXPECT_TRUE(environment.contains(QStringLiteral("TERM=xterm-256color")));
}
