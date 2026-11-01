// TerminalLaunchTests — the line the embedded terminal is fed to start a run.
//
// The run is started by typing the program's path into a shell, so the path
// is shell input. Left unquoted (R2-20) a path with a space is two words:
// cmd reports "'C:\My' is not recognized as an internal or external command"
// while the IDE shows the run as started. These tests assert the property
// that matters rather than one spelling — the line is a *single* shell word —
// so a helper that handed the raw path back would fail them even if it
// happened to match a hard-coded expected string for a path without spaces.

#include "LightCpp/App/TerminalLaunch.hpp"

#include <gtest/gtest.h>

#include <QChar>
#include <QString>

using LightCpp::App::TerminalLaunch::programCommandLine;

namespace {

/// Count the whitespace-separated words a shell would see, ignoring spaces
/// that sit inside single or double quotes. One word means the whole path is
/// the command; more than one means the shell reads a fragment of it.
int shellWordCount(const QString& line)
{
    int words = 0;
    bool inWord = false;
    QChar quote;
    for (const QChar c : line) {
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            }
            continue;
        }
        if (c == u'\'' || c == u'"') {
            if (!inWord) {
                ++words;
                inWord = true;
            }
            quote = c;
            continue;
        }
        if (c.isSpace()) {
            inWord = false;
            continue;
        }
        if (!inWord) {
            ++words;
            inWord = true;
        }
    }
    return words;
}

}  // namespace

#ifdef Q_OS_WIN
TEST(TerminalLaunch, ACmdPathWithSpacesStaysOneWord)
{
    const QString line = programCommandLine(QStringLiteral("C:/My Project/bin/hello.exe"),
                                            /*powerShellStyle=*/false);
    EXPECT_EQ(line, QStringLiteral("\"C:/My Project/bin/hello.exe\""));
    EXPECT_EQ(shellWordCount(line), 1);
}
#else
TEST(TerminalLaunch, PosixPathWithSpacesStaysOneWord)
{
    const QString line = programCommandLine(QStringLiteral("/home/user/My Project/bin/hello"),
                                            /*powerShellStyle=*/false);
    EXPECT_EQ(line, QStringLiteral("'/home/user/My Project/bin/hello'"));
    EXPECT_EQ(shellWordCount(line), 1);
}

TEST(TerminalLaunch, PosixSingleQuotesAreEscaped)
{
    const QString line = programCommandLine(QStringLiteral("/home/user/O'Brien/hello"),
                                            /*powerShellStyle=*/false);
    EXPECT_EQ(line, QStringLiteral("'/home/user/O'\\''Brien/hello'"));
    EXPECT_EQ(shellWordCount(line), 1);
}
#endif

TEST(TerminalLaunch, TheUnquotedPathWouldBeReadAsTwoWords)
{
    // The counter-example the fix exists for: without quoting, cmd sees
    // 'C:/My' and then 'Project/bin/hello.exe'.
    const QString raw = QStringLiteral("C:/My Project/bin/hello.exe");
    EXPECT_EQ(shellWordCount(raw), 2);
}

TEST(TerminalLaunch, APathWithoutSpacesIsStillQuoted)
{
    const QString line = programCommandLine(QStringLiteral("C:/msys64/ucrt64/bin/hello.exe"),
                                            /*powerShellStyle=*/false);
#ifdef Q_OS_WIN
    EXPECT_EQ(line, QStringLiteral("\"C:/msys64/ucrt64/bin/hello.exe\""));
#else
    EXPECT_EQ(line, QStringLiteral("'C:/msys64/ucrt64/bin/hello.exe'"));
#endif
    EXPECT_EQ(shellWordCount(line), 1);
}

TEST(TerminalLaunch, PowerShellGetsTheCallOperator)
{
    // A PowerShell template is launched without the value its -Command/-c
    // expects, so a bare quoted path would only evaluate to a string; the
    // call operator is what runs it.
    const QString line = programCommandLine(QStringLiteral("C:/My Project/bin/hello.exe"),
                                            /*powerShellStyle=*/true);
    EXPECT_EQ(line, QStringLiteral("& 'C:/My Project/bin/hello.exe'"));
    EXPECT_EQ(shellWordCount(line), 2);  // "&" and the quoted argument
}

TEST(TerminalLaunch, PowerShellSingleQuotesInThePathAreDoubled)
{
    const QString line = programCommandLine(QStringLiteral("C:/O'Brien/hello.exe"),
                                            /*powerShellStyle=*/true);
    EXPECT_EQ(line, QStringLiteral("& 'C:/O''Brien/hello.exe'"));
}

// ---------------------------------------------------------------------------
// Program arguments (Run Project passes the backend's runArguments through)
// ---------------------------------------------------------------------------

TEST(TerminalLaunch, ArgumentsBecomeTheirOwnShellWords)
{
    // A run argument with a space must not be split: the project's
    // runArguments are one word each, quoted like the program path.
    const QString script = QStringLiteral("C:/My Docs/script.py");
    const QString line = programCommandLine(QStringLiteral("python.exe"),
                                            /*powerShellStyle=*/false,
                                            {script, QStringLiteral("--verbose")});
#ifdef Q_OS_WIN
    EXPECT_EQ(line,
              QStringLiteral("\"python.exe\" \"C:/My Docs/script.py\" "
                             "\"--verbose\""));
#else
    EXPECT_EQ(line,
              QStringLiteral("'python.exe' 'C:/My Docs/script.py' '--verbose'"));
#endif
    EXPECT_EQ(shellWordCount(line), 3);
}

TEST(TerminalLaunch, PowerShellQuotesEveryArgument)
{
    const QString line = programCommandLine(QStringLiteral("C:/app/hello.exe"),
                                            /*powerShellStyle=*/true,
                                            {QStringLiteral("--name"),
                                             QStringLiteral("O'Brien")});
    EXPECT_EQ(line,
              QStringLiteral("& 'C:/app/hello.exe' '--name' 'O''Brien'"));
    // The call operator is its own word; the program and its two arguments
    // follow it, each quoted as one word.
    EXPECT_EQ(shellWordCount(line), 4);
}

#ifndef Q_OS_WIN
TEST(TerminalLaunch, PosixArgumentsEscapeEmbeddedQuotes)
{
    const QString line = programCommandLine(QStringLiteral("/app/hello"),
                                            /*powerShellStyle=*/false,
                                            {QStringLiteral("it's fine")});
    EXPECT_EQ(line, QStringLiteral("'/app/hello' 'it'\\''s fine'"));
    EXPECT_EQ(shellWordCount(line), 2);
}
#endif
