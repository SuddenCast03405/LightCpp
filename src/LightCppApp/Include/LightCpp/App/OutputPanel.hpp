#pragma once

#include <QList>
#include <QPlainTextEdit>
#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
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

/// Output area.  Five switchable channels behind one drop-down:
///
///   Clangd Log      — the language server's own log (stderr + window/logMessage)
///   Build Log       — compiler output and IDE file operations
///   Program Output  — the run console: what the built program prints, plus the
///                     run banner and its exit status
///   Git             — status, diffs and commit output
///   Problems        — errors/warnings pushed by clangd, as a clickable list
///                     that jumps to the offending source location
///
/// Deliberately a plain QWidget (not a QDockWidget): the panel lives inside
/// the main window's splitter layout and can never be undocked into a
/// floating window of its own.
class OutputPanel : public QWidget {
    Q_OBJECT
public:
    /// Channel indices, matching the drop-down order.
    enum Channel {
        ChannelServer = 0,   // clangd log
        ChannelBuild = 1,    // compiler / IDE log
        ChannelProgram = 2,  // the program's own stdout/stderr
        ChannelGit = 3,      // git status / diff / commit
        ChannelProblems = 4, // diagnostics list
    };

    explicit OutputPanel(QWidget* parent = nullptr);

    /// Append a line to the clangd server log.
    void appendServerLog(const QString& text);
    /// Append a line to the compiler / IDE log.
    void appendBuildOutput(const QString& text);
    /// Append a line of output produced by the running program.
    void appendProgramOutput(const QString& text);
    /// Append a line to the git channel.
    void appendGitOutput(const QString& text);
    /// Replace the Problems list. An empty list shows "no problems".
    void setProblems(const QList<ProblemEntry>& problems);

    /// Bring `channel` to the front.
    void showChannel(Channel channel);

    /// Apply the settings' output font to the log channels and the Problems
    /// list. Invalid (unset) fonts are ignored.
    void setOutputFont(const QFont& font);

    /// Clear the channel currently on screen.
    void clearOutput();

signals:
    /// The user activated an entry in the Problems list. Line/column are
    /// 0-based, matching the editor's own coordinate system.
    void problemActivated(const QString& filePath, int line, int column);

private:
    /// Refresh the drop-down labels (the Problems entry carries counts).
    void updateChannelLabels();
    /// Rebuild the Problems tree from a flat list.
    void rebuildProblemsTree(const QList<ProblemEntry>& problems);

    QLabel* header_;
    QComboBox* channelCombo_;
    QToolButton* clearButton_;
    QStackedWidget* stack_;
    QPlainTextEdit* serverLog_;
    QPlainTextEdit* buildLog_;
    QPlainTextEdit* programOutput_;
    QPlainTextEdit* gitLog_;
    QTreeWidget* problems_;
};
