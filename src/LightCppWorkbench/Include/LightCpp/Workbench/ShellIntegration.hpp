// ShellIntegration — what a prompt-aware terminal needs to know.
//
// lightcpp.1.md §九 asks for shell integration. The useful part is not
// decoration: knowing where a prompt ends and a command begins is what makes
// "copy the last command", "run that again", "jump to the previous prompt"
// and "did this command fail" possible at all.
//
// The mechanism is OSC 133, the convention shells and terminals agreed on:
//
//   ESC ] 133 ; A ST   prompt start
//   ESC ] 133 ; B ST   prompt end / command line start
//   ESC ] 133 ; C ST   command start (the command was accepted)
//   ESC ] 133 ; D ; n  command finished, n = exit status
//
// Everything here is a small state machine over that stream plus helpers for
// the questions a terminal asks. It deliberately does not parse the command
// text itself: the shell is the only thing that can do that honestly, and a
// half-parse would produce a "current command" that is sometimes wrong.

#pragma once

#include <QList>
#include <QString>

namespace LightCpp::Workbench::Shell {

/// One mark emitted by the shell, with the row it appeared on.
struct Mark {
    enum class Kind {
        PromptStart,   ///< A
        PromptEnd,     ///< B
        CommandStart,  ///< C
        CommandEnd,    ///< D
    };

    Kind kind = Kind::PromptStart;
    /// Screen row (0-based, from the terminal emulator) the mark was seen on.
    int row = 0;
    /// Exit status, only meaningful for CommandEnd. -1 when the shell did not
    /// report one.
    int exitCode = -1;

    [[nodiscard]] bool operator==(const Mark& other) const
    {
        return kind == other.kind && row == other.row && exitCode == other.exitCode;
    }
};

/// The command the shell last reported, with what happened to it.
struct CommandRecord {
    int promptRow = 0;    ///< where the prompt started
    int commandRow = 0;   ///< where the command line was
    /// The text the user typed, when the shell sent it with an OSC 633 `E`
    /// mark or the terminal could read it. Empty otherwise.
    QString text;
    int exitCode = -1;    ///< -1: still running, or never reported
    bool finished = false;
};

/// Parse the OSC 133 payload (the text after "133;") into a mark.
/// Returns false when the payload is not a mark this class understands —
/// which is the normal case, since OSC 133 shares its channel with titles
/// and hyperlinks.
[[nodiscard]] bool parseOsc133(const QString& payload, int row, Mark* mark);

/// Accumulates marks into command records.
class Tracker {
public:
    /// Feed one mark. Marks that arrive out of order (a `D` without a `C`,
    /// which happens when the shell starts mid-session) are ignored rather
    /// than turned into a record with nonsense fields.
    void feed(const Mark& mark);

    /// The marks seen, in order, capped — a long session emits four per
    /// command and keeping all of them would grow without bound.
    [[nodiscard]] const QList<Mark>& marks() const { return marks_; }

    /// Finished commands, oldest first, capped at `historyLimit`.
    [[nodiscard]] const QList<CommandRecord>& commands() const { return commands_; }
    void setHistoryLimit(int limit);
    void clear();

    /// The row the current prompt starts on, or -1 when no prompt is open.
    [[nodiscard]] int currentPromptRow() const;
    /// True while a command is running (a `C` has been seen with no `D`).
    [[nodiscard]] bool isCommandRunning() const { return running_ >= 0; }

    /// The most recent finished command, or a default record when there is
    /// none (check `finished`).
    [[nodiscard]] CommandRecord lastCommand() const;
    /// The last command that failed, for "show me the error" and for
    /// highlighting the failure in the gutter.
    [[nodiscard]] CommandRecord lastFailure() const;
    /// Rows of the prompts in `[fromRow, toRow]`, for "jump to previous
    /// prompt". Returned in screen order.
    [[nodiscard]] QList<int> promptRowsBetween(int fromRow, int toRow) const;

private:
    void appendMark(const Mark& mark);

    QList<Mark> marks_;
    QList<CommandRecord> commands_;
    int historyLimit_ = 200;
    int markLimit_ = 2000;
    /// Index into marks_ of the open `PromptStart`, or -1.
    int promptOpen_ = -1;
    /// Index into marks_ of the open `CommandStart`, or -1.
    int running_ = -1;
    /// The command row of the currently open command, for the record.
    int commandRow_ = 0;
};

/// The text of the prompt line from a terminal's current line, given where
/// the prompt-end mark sits: everything before the marker is the prompt,
/// everything after is what the user has typed so far.
[[nodiscard]] QString promptText(const QString& line, int promptEndColumn);

/// The command part of a line, given where the prompt ends. Used for "copy
/// the command line" and for completion against what is typed.
[[nodiscard]] QString commandText(const QString& line, int promptEndColumn);

}  // namespace LightCpp::Workbench::Shell
