#pragma once

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

#include <memory>

#include "LightCpp/Workbench/ShellIntegration.hpp"
#include "LightCpp/Workbench/TerminalEmulator.hpp"

class QLabel;
class QPlainTextEdit;
class QScrollBar;
class QTextCharFormat;
class QTimer;

/// The terminal panel: a `Workbench::Terminal::Emulator` on one side and a
/// read-only `QPlainTextEdit` on the other.
///
/// The split is the whole point. Everything that decides *what* the terminal
/// shows lives in the emulator, which is pure Qt-Core and therefore unit
/// tested without a display; this widget only decides how a run of cells
/// becomes characters and formats, plus how keystrokes travel back out.
///
/// Two details are load-bearing:
///
///   * Colour comes out of the emulator's `paletteColor` table rather than a
///     per-cell brush, so a 24-bit gradient and a 256-colour cube cost the
///     same to resolve.
///   * The document is rebuilt from the emulator's screen model rather than
///     appended to, because a full-screen program erases and rewrites rows;
///     appending would leave the previous frame behind. Repaints are
///     coalesced on a zero timer, so a build that prints a thousand lines
///     still repaints once per event-loop turn.
class TerminalWidget : public QWidget {
    Q_OBJECT
public:
    explicit TerminalWidget(const QString& title, QWidget* parent = nullptr);
    ~TerminalWidget() override;

    /// Bytes from the shell. Decoded and rendered; the emulator keeps any
    /// partial escape sequence or UTF-8 character for the next call.
    void feed(const QByteArray& bytes);
    /// Report the child process's exit status, so the panel can say so
    /// instead of leaving a dead input area looking live.
    void setExited(int exitCode);
    void setExitedWithError(const QString& reason);

    [[nodiscard]] QString title() const { return title_; }
    [[nodiscard]] bool isExited() const { return exited_; }
    /// The text typed on the current command line, as far as the widget
    /// saw it (Enter and Escape reset it). Used by history.
    [[nodiscard]] QString commandLine() const { return typed_; }

    /// The emulator, for actions that read the screen (copy whole screen,
    /// export scrollback). Never null.
    [[nodiscard]] LightCpp::Workbench::Terminal::Emulator* emulator() const
    {
        return emulator_.get();
    }
    [[nodiscard]] LightCpp::Workbench::Shell::Tracker* tracker() const
    {
        return tracker_.get();
    }

    /// Apply the configured monospace font.
    void setTerminalFont(const QFont& font);
    /// Columns and rows the current viewport can show.
    [[nodiscard]] QSize gridSize() const;

    /// Send text as if typed (bytes only; the shell echoes it back).
    void sendText(const QString& text);
    /// Send a raw sequence (arrows, function keys, paste markers).
    void sendKey(const QByteArray& sequence);

    /// Record a shell-integration mark. The owner feeds these as it parses
    /// OSC 133 out of the same stream; the widget keeps them so a gutter or
    /// a "previous prompt" action has something to ask.
    void addMark(const LightCpp::Workbench::Shell::Mark& mark);

signals:
    /// One keystroke, already encoded for the terminal (arrows become CSI
    /// sequences, Enter becomes CR). The owner writes this to the shell.
    void bytesTyped(const QByteArray& bytes);
    /// The user pressed Enter on a command line. The bytes still travel
    /// through `bytesTyped`; this is for history and for "run that again".
    void commandEntered(const QString& text);
    /// The window title changed (OSC 0/2), so a tab can follow `vim`.
    void titleChanged(const QString& title);
    /// The user cleared the screen or the scrollback.
    void cleared();
    /// The visible grid changed size (the window was resized, or the font
    /// was). The owner forwards it to the pty so the child gets SIGWINCH and
    /// a full-screen program re-lays itself out instead of drawing past the
    /// edge of a screen it thinks is still 80 columns wide.
    void gridResized(int columns, int rows);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    void rebuildFontMetrics();
    void resizeEmulatorToViewport();
    void scheduleRepaint();
    void repaintNow();
    void scrollToLive();
    void handleKeyPress(QKeyEvent* event);
    void copySelection();
    void pasteFromClipboard();
    void updateScrollBar();
    void setStatus(const QString& text);
    void clearScreen();
    void clearScrollback();
    /// Turn one cell's attributes into a character format. Colours are
    /// resolved here, which is why the palette lives in the emulator.
    [[nodiscard]] QTextCharFormat formatFor(
        const LightCpp::Workbench::Terminal::Attributes& attributes) const;

    std::unique_ptr<LightCpp::Workbench::Terminal::Emulator> emulator_;
    std::unique_ptr<LightCpp::Workbench::Shell::Tracker> tracker_;

    QPlainTextEdit* view_ = nullptr;
    QScrollBar* scrollBar_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* repaintTimer_ = nullptr;

    QString title_;
    QFont font_;
    int cellWidth_ = 8;
    int cellHeight_ = 16;
    /// What the user has typed on the current line. Reset at Enter and at
    /// Escape; a caret key leaves it alone because the widget cannot know
    /// where readline moved the cursor.
    QString typed_;
    bool exited_ = false;
    bool fillingScrollBar_ = false;
    /// True while the view is scrolled back, so a live byte arriving does
    /// not scroll the user away from what they were reading.
    bool userScrolledBack_ = false;
};
