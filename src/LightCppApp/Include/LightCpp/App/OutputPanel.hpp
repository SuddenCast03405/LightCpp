#pragma once

#include <QList>
#include <QPlainTextEdit>
#include <QString>
#include <QWidget>

#include <functional>

class TerminalWidget;
class QComboBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QToolButton;
class QTreeWidget;

/// One row in the Problems list: a language-server diagnostic plus the file
/// it belongs to. Line/column are 0-based (straight from LSP).
struct ProblemEntry {
    QString filePath;   // real path, or the virtual path of an untitled buffer
    int line = 0;
    int column = 0;
    /// 1 = Error, 2 = Warning, 3 = Information, 4 = Hint.
    int severity = 1;
    QString message;
    QString code;
};

    /// Output area.  Ten switchable channels behind one drop-down:
    ///
    ///   Clangd Output   — the clangd language server's own log
    ///                     (stderr + window/logMessage)
    ///   Build Log       — compiler output and IDE file operations
    ///   Program Output  — the run console: what the built program prints, plus the
    ///                     run banner and its exit status. A one-line input field
    ///                     below the log feeds the program's stdin while it runs.
    ///   Git             — status, diffs and commit output
    ///   Problems (LSP Server) — errors/warnings pushed by the language server, as a
    ///                     clickable list that jumps to the offending source location
    ///   Problems (Compiler) — errors/warnings parsed out of the compiler's
    ///                     log (single-file compile and project build alike),
    ///                     clickable the same way
    ///   Debugger        — the debug session: debugger console, breakpoints and
    ///                     the program's output while it runs under the debugger,
    ///                     plus an input row feeding the debugger's console
    ///   Plugins         — plugin load/unload messages and plugin command output
    ///   ccls Output     — the ccls language server's own log
    ///   libclang Output — libclang completer chatter (load failures, parse retries)
    ///
    /// clangd and ccls never run at the same time (one completion backend),
    /// but each keeps its own channel so the drop-down names the server
    /// whose output it holds.
///
/// Deliberately a plain QWidget (not a QDockWidget): the panel lives inside
/// the main window's splitter layout and can never be undocked into a
/// floating window of its own.
class OutputPanel : public QWidget {
    Q_OBJECT
public:
    /// Channel indices, matching the drop-down order.
    enum Channel {
        ChannelServer = 0,          // clangd log
        ChannelBuild = 1,           // compiler / IDE log
        ChannelProgram = 2,         // the program's own stdout/stderr
        ChannelGit = 3,             // git status / diff / commit
        ChannelProblems = 4,        // LSP diagnostics list
        ChannelProblemsCompiler = 5, // compiler diagnostics list
        ChannelDebug = 6,           // debug session console
        ChannelPlugins = 7,         // plugin load/unload + plugin command output
        ChannelCcls = 8,            // ccls server log
        ChannelLibClang = 9,        // libclang completer chatter
    };

    explicit OutputPanel(QWidget* parent = nullptr);

    /// Append a line to the clangd server log.
    void appendServerLog(const QString& text);
    /// Append a line to the ccls server log.
    void appendCclsOutput(const QString& text);
    /// Append a line to the libclang completer log.
    void appendLibClangOutput(const QString& text);
    /// Append a line to the compiler / IDE log.
    void appendBuildOutput(const QString& text);
    /// Append a line of output produced by the running program.
    void appendProgramOutput(const QString& text);
    /// Append a line to the git channel.
    void appendGitOutput(const QString& text);
    /// Append a line to the debugger channel.
    void appendDebugOutput(const QString& text);
    /// Append a line to the plugins channel (load messages and plugin
    /// command output routed through IdeServices::log).
    void appendPluginOutput(const QString& text);
    /// Open (or reuse) the embedded-terminal channel, titled after the
    /// selected terminal (e.g. "Windows Command Prompt") — the Windows
    /// Terminal style drop-down entry for one shell session. The log is
    /// cleared, the channel brought to front and the input row enabled.
    /// Returns the channel index.
    int openTerminalChannel(const QString& title, bool resetBuffer = true);
    /// Append raw terminal output (already byte-decoded) at the cursor,
    /// without appendPlainText's per-line paragraph logic: an unfinished
    /// line — a prompt such as "D:\>" that never ends with a newline —
    /// stays open and the following bytes continue on the same line, like
    /// a real terminal.
    void appendTerminalOutput(const QString& text);
    /// Enable/disable the terminal input row (live while the shell runs).
    void setTerminalInputEnabled(bool enabled);
    /// Render the terminal through a screen model instead of appending text.
    ///
    /// Appending is right for a command's *transcript* and wrong for a
    /// full-screen program: `clear` arrives as an escape sequence, not as a
    /// cleared buffer, so a transcript keeps every frame. Once a widget is
    /// installed the channel is a grid, and `appendTerminalOutput` becomes a
    /// plain-text fallback for callers that have no bytes. Ownership stays
    /// with the caller; passing nullptr restores the transcript.
    void setTerminalScreen(TerminalWidget* terminal);
    [[nodiscard]] TerminalWidget* terminalScreen() const { return terminal_; }
    /// Rename a channel after the session it holds (a shell setting its
    /// window title, a terminal that runs `vim`). Out-of-range indices are
    /// ignored: a late title from a closed session must not create an entry.
    void setChannelTitle(int channel, const QString& title);
    /// Replace the Problems list. An empty list shows "no problems".
    void setProblems(const QList<ProblemEntry>& problems);
    /// Replace the Problems (Compiler) list (parsed compiler output).
    void setCompilerProblems(const QList<ProblemEntry>& problems);

    /// Bring `channel` to the front.
    void showChannel(Channel channel);

    /// Apply the settings' output font to the log channels and the Problems
    /// list. Invalid (unset) fonts are ignored.
    void setOutputFont(const QFont& font);

    /// Clear the channel currently on screen.
    void clearOutput();

    /// Enable/disable the program-input field (enabled while a program the
    /// IDE owns is running; the Console Pauser mode runs in its own console
    /// and never touches this field).
    void setProgramInputEnabled(bool enabled);
    /// Show/hide the program input row entirely. The captured Program
    /// Output mode and embedded terminals drive the run through this field;
    /// Console Pauser and non-embedded terminals (own console window) have
    /// no IDE-side stdin and hide the row.
    void setProgramInputVisible(bool visible);
    /// Enable/disable the debugger-input field (enabled while a debug
    /// session is live).
    void setDebuggerInputEnabled(bool enabled);

signals:
    /// The user activated an entry in the Problems list. Line/column are
    /// 0-based. `compilerColumn` marks a column that came from a compiler,
    /// which counts *display cells* (each CJK character is two) rather than
    /// the UTF-16 code units QTextCursor uses — the receiver converts it.
    void problemActivated(const QString& filePath, int line, int column,
                          bool compilerColumn);
    /// The user submitted a line of input for the running program (Enter in
    /// the input field, or the Send button).
    void programInputSubmitted(const QString& text);
    /// The user submitted a console command for the live debug session.
    void debuggerInputSubmitted(const QString& text);
    /// The user submitted a line for the embedded terminal (a shell command
    /// or input to the program the shell is currently running).
    void terminalInputSubmitted(const QString& text);

private:
    /// Rebuild a Problems tree from a flat list. Shared by the LSP and the
    /// compiler channel — the trees behave identically, only the source of
    /// the entries differs.
    void rebuildProblemsTree(QTreeWidget* tree,
                             const QList<ProblemEntry>& problems);
    /// Fill `tree` from `problems` and stamp the error/warning counts into
    /// the channel drop-down label of the matching Problems channel.
    void updateProblemsChannel(QTreeWidget* tree, int comboIndex,
                               const QString& baseLabel,
                               const QList<ProblemEntry>& problems);
    /// Build a "log view + one-line input row" page: what the Program and
    /// Debugger channels share. The input starts disabled; `submit` routes
    /// the typed line (the field is cleared before `submit` runs). When
    /// `rowOut` is given it receives the whole input-row container, so the
    /// owner can hide the row (not just disable the field).
    QWidget* makeInputLogPage(QPlainTextEdit* log, QLineEdit** inputOut,
                              const QString& placeholder,
                              const std::function<void(const QString&)>& submit,
                              QWidget** rowOut = nullptr);

    QLabel* header_;
    QComboBox* channelCombo_;
    QToolButton* clearButton_;
    QStackedWidget* stack_;
    QPlainTextEdit* serverLog_;
    QPlainTextEdit* cclsLog_;
    QPlainTextEdit* libClangLog_;
    QPlainTextEdit* buildLog_;
    QPlainTextEdit* programOutput_;
    QLineEdit* programInput_ = nullptr;  // stdin for the running program
    /// The whole input row (field + Send) of the Program channel; hidden
    /// when the run mode does not capture the program in the IDE.
    QWidget* programInputRow_ = nullptr;
    QLineEdit* debuggerInput_ = nullptr;  // raw commands for the live session
    QPlainTextEdit* gitLog_;
    QPlainTextEdit* debugLog_;
    QPlainTextEdit* pluginLog_;
    QTreeWidget* problems_;
    /// The screen-model renderer, when one is installed (see
    /// setTerminalScreen).
    TerminalWidget* terminal_ = nullptr;
    /// The embedded terminal session (created on demand by
    /// openTerminalChannel): one log + input row appended after the fixed
    /// channels, titled after the selected terminal.
    QPlainTextEdit* terminalLog_ = nullptr;
    QLineEdit* terminalInput_ = nullptr;
    QWidget* terminalInputRow_ = nullptr;
    int terminalChannel_ = -1;  // stack/combo index once opened
    QTreeWidget* compilerProblems_;
};
