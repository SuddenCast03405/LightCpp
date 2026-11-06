// TerminalEmulator — the part of a terminal that is not a PTY.
//
// lightcpp.1.md §九 asks for a real built-in terminal. The hard half is the
// pseudo-terminal, which is a platform API; the other half is a small
// screen model that turns a stream of bytes into what a terminal shows.
// This file is the second half, and it is pure logic on purpose: the escape
// sequences below are exactly the things that break silently when they are
// only ever tested by looking at a screenshot.
//
// What is modelled, and why each part is needed rather than nice to have:
//
//   * CSI (ESC [ ... final) with cursor movement, erase, insert/delete,
//     scroll regions and SGR. SGR is *tracked* but not interpreted into
//     pixels — the renderer upstream applies it to a colour buffer, and the
//     model only has to know that the sequence consumed no cells, which is
//     the bug this repo has seen elsewhere ("[1;32m" printed literally).
//   * OSC (ESC ] ... BEL/ST). Titles and hyperlinks. Ignoring them entirely
//     would print the title text into the grid.
//   * CR/LF/BS/TAB and the DEC private modes that change the meaning of
//     those (`DECAWM` for autowrap, alternate screen for full-screen apps).
//   * UTF-8 decoding across chunk boundaries, because a PTY read can split
//     a multi-byte character and a naive per-chunk decode corrupts it.
//   * Scrollback, capped, with the invariant that lines scrolled off are
//     kept and lines erased below the cursor are not.
//
// Deliberately not modelled: sixel/kitty graphics, mouse reporting, and the
// full SGR state machine. Each is a project of its own; none is needed to
// run a build or a test.

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::Terminal {

/// One cell's attributes, as far as the model cares. The renderer decides
/// what "bright magenta" looks like; the model only records which SGR codes
/// were active when the cell was written.
struct Attributes {
    quint32 fg = 0;        ///< 0 = default; else 0xRRGGBB or a palette index
    quint32 bg = 0;
    bool fgIsPalette = false;
    bool bgIsPalette = false;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;
    bool blink = false;
    bool inverse = false;
    bool hidden = false;
    bool strike = false;

    [[nodiscard]] bool operator==(const Attributes& other) const;
    [[nodiscard]] bool operator!=(const Attributes& other) const
    {
        return !(*this == other);
    }
};

/// A renderable run of cells: text plus the attributes it was written with.
/// One row becomes several runs when the attributes change mid-line — which
/// is what a syntax-coloured compiler message needs.
struct StyledRun {
    QString text;
    Attributes attributes;
};

struct Cell {
    QString text;      ///< one grapheme, or empty for the right half of a wide char
    Attributes attributes;
};

/// The result of feeding bytes: what changed, so the renderer does not have
/// to redraw the whole screen on every keystroke.
struct EmulatorUpdate {
    /// True when the whole visible screen must be repainted (a clear, a
    /// scroll, an alternate-screen switch).
    bool fullRepaint = false;
    /// Rows that changed, when `fullRepaint` is false.
    QList<int> dirtyRows;
    /// The window title, when the application set one this time.
    QString title;
};

class Emulator {
public:
    Emulator(int columns = 80, int rows = 24, int scrollback = 5000);

    // --- geometry ----------------------------------------------------------

    [[nodiscard]] int columns() const { return columns_; }
    [[nodiscard]] int rows() const { return rows_; }
    /// Resize the viewport. The content is preserved as far as it fits: a
    /// shrink pushes the top rows into scrollback rather than dropping them,
    /// because the output that scrolled away is usually the interesting part.
    void resize(int columns, int rows);

    // --- input -------------------------------------------------------------

    /// Feed raw bytes from the PTY. Bytes may split a UTF-8 character or an
    /// escape sequence across calls; the remainder is kept.
    [[nodiscard]] EmulatorUpdate feed(const QByteArray& bytes);

    /// Feed text the user typed, for tests and for "type this for me".
    [[nodiscard]] EmulatorUpdate feedText(const QString& text);

    // --- reading -----------------------------------------------------------

    /// The window title as last set by OSC 0/2, empty until something sets
    /// one. A renderer that polls this does not need to hold the update it
    /// came in.
    [[nodiscard]] QString title() const { return title_; }

    [[nodiscard]] int cursorColumn() const { return cursorColumn_; }
    [[nodiscard]] int cursorRow() const { return cursorRow_; }
    [[nodiscard]] bool cursorVisible() const { return cursorVisible_; }
    [[nodiscard]] bool isAlternateScreen() const { return alternateScreen_; }
    [[nodiscard]] bool cursorKeysApplicationMode() const { return applicationCursorKeys_; }
    [[nodiscard]] bool bracketedPaste() const { return bracketedPaste_; }

    /// The visible screen, one string per row, trailing blanks trimmed. This
    /// is what "copy the terminal" copies.
    [[nodiscard]] QStringList screenLines() const;
    /// The visible screen with attributes, one run list per row.
    [[nodiscard]] QList<QList<StyledRun>> screenRuns() const;
    /// Scrollback plus the visible screen, oldest first. Capped at the
    /// scrollback limit.
    [[nodiscard]] QStringList scrollbackLines() const;
    [[nodiscard]] int scrollbackCount() const;
    /// How many lines the view is scrolled back from the bottom. 0 means the
    /// live screen.
    [[nodiscard]] int scrollOffset() const { return scrollOffset_; }
    void setScrollOffset(int offset);

    /// Forget the scrollback (the "clear scrollback" action). The screen is
    /// untouched: a user who clears the history does not expect the command
    /// they are looking at to disappear.
    void clearScrollback();

    /// The text under a screen cell, for "copy selection": (row, column) to
    /// (row, column), inclusive, in screen coordinates.
    [[nodiscard]] QString textBetween(int startRow, int startColumn, int endRow,
                                      int endColumn) const;

    /// The row the cursor is on, with the cells after it trimmed — what a
    /// shell prompt looks like. Used by the shell-integration tests and by
    /// the "copy command" action.
    [[nodiscard]] QString currentLine() const;

    /// The palette entry a 256-colour SGR index refers to (0xRRGGBB). The
    /// first 16 are the xterm defaults, 16..231 the 6x6x6 cube, 232..255 the
    /// grey ramp — the same mapping every terminal uses, because tools pick
    /// indices expecting it.
    [[nodiscard]] static quint32 paletteColor(int index);

private:
    struct Room {
        QList<QList<Cell>> lines;  ///< `rows_` lines, may be shorter while growing
    };

    /// True when every cell is a blank. Used by resize() to decide which end
    /// of the screen to drop.
    [[nodiscard]] static bool isBlankRow(const QList<Cell>& line);
    void ensureLine(int row);
    QList<Cell>& lineAt(int row);
    const QList<Cell>& lineAt(int row) const;
    void putGlyph(const QString& glyph, int width);
    void newline();
    void carriageReturn();
    void scrollUp(int count = 1);
    void scrollDown(int count = 1);
    void eraseInLine(int mode);
    void eraseInDisplay(int mode);
    void applySgr(const QList<int>& parameters);
    void reset();
    void switchAlternateScreen(bool on);
    /// Handle one complete escape sequence. Returns false for sequences this
    /// model does not implement, so the caller can log rather than guess.
    bool handleCsi(const QString& parameters, QChar final);
    bool handleEsc(QChar final);
    void handleOsc(const QString& payload);
    void handleControl(QChar ch);
    void markDirty(int row);
    QString rowText(int row, bool trimTrailing) const;

    int columns_;
    int rows_;
    int scrollbackLimit_;
    QList<QList<Cell>> screen_;
    QList<QList<Cell>> alternate_;
    QList<QStringList> scrollback_;
    int cursorColumn_ = 0;
    int cursorRow_ = 0;
    Attributes current_;
    bool cursorVisible_ = true;
    bool alternateScreen_ = false;
    bool applicationCursorKeys_ = false;
    bool bracketedPaste_ = false;
    bool autoWrap_ = true;
    bool pendingWrap_ = false;  ///< the "wrap on next glyph" flag after a full row
    int scrollTop_ = 0;
    int scrollBottom_ = 0;      ///< inclusive
    int scrollOffset_ = 0;
    QString title_;
    QString savedTitle_;
    QByteArray pending_;        ///< incomplete UTF-8 or escape sequence
    /// A pending DCS/OSC-string: when set, bytes are swallowed until the
    /// terminator so the payload cannot reach the grid.
    enum class StringState { None, Osc, Dcs };
    StringState stringState_ = StringState::None;
    QString stringBuffer_;
    EmulatorUpdate update_;
    /// The character set in effect. Only the `G0` selection matters for the
    /// box-drawing substitution some tools still use.
    bool lineDrawing_ = false;
};

}  // namespace LightCpp::Workbench::Terminal
