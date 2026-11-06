// TerminalWidget — implementation.
//
// The rule this file follows: the widget may look at the emulator but must
// never decide anything the emulator could have decided. Grid, wrapping,
// erasing and UTF-8 are already answered; what is left here is mapping cells
// to characters, attributes to formats, and keystrokes to bytes.

#include "LightCpp/App/TerminalWidget.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "LightCpp/Workbench/ShellIntegration.hpp"

namespace Term = LightCpp::Workbench::Terminal;
namespace Shell = LightCpp::Workbench::Shell;

namespace {

constexpr const char* kBackground = "#1e1e1e";
constexpr const char* kForeground = "#d4d4d4";

/// Bytes a cursor key sends. The interesting part is the *difference*
/// between modes: in application-cursor mode an arrow is `ESC O A`,
/// otherwise `ESC [ A`, and a program that asked for the former breaks
/// visibly if it gets the latter (readline in vi mode does).
QByteArray cursorSequence(char final, bool application)
{
    QByteArray out;
    out.append(char(0x1b));
    out.append(application ? 'O' : '[');
    out.append(final);
    return out;
}

/// xterm's "CSI 1;<mod> X" form: mod is 1 + a bitmask of shift(1), alt(2),
/// ctrl(4). Returns empty when no modifier is held, so an unmodified key
/// never takes this path.
QByteArray modifierSequence(char final, Qt::KeyboardModifiers mods)
{
    int mask = 0;
    if (mods.testFlag(Qt::ShiftModifier)) {
        mask |= 1;
    }
    if (mods.testFlag(Qt::AltModifier)) {
        mask |= 2;
    }
    if (mods.testFlag(Qt::ControlModifier)) {
        mask |= 4;
    }
    if (mask == 0) {
        return {};
    }
    QByteArray out;
    out.append("\x1b[1;");
    out.append(QByteArray::number(mask + 1));
    out.append(final);
    return out;
}

QTextCharFormat baseFormat(const QFont& font)
{
    QTextCharFormat format;
    if (!font.family().isEmpty()) {
        format.setFontFamilies({font.family()});
    }
    format.setFontPointSize(font.pointSizeF() > 0 ? font.pointSizeF() : 10.0);
    format.setFontFixedPitch(true);
    return format;
}

QTextBlockFormat fixedLineHeight(int pixels)
{
    QTextBlockFormat format;
    format.setLineHeight(pixels, QTextBlockFormat::FixedHeight);
    return format;
}

}  // namespace

TerminalWidget::TerminalWidget(const QString& title, QWidget* parent)
    : QWidget(parent), title_(title)
{
    emulator_ = std::make_unique<Term::Emulator>(80, 24, 5000);
    tracker_ = std::make_unique<Shell::Tracker>();

    font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (font_.family().isEmpty() || font_.pointSizeF() <= 0) {
        font_ = QFont(QStringLiteral("monospace"), 10);
    }
    font_.setStyleHint(QFont::Monospace);
    font_.setFixedPitch(true);

    view_ = new QPlainTextEdit(this);
    view_->setObjectName(QStringLiteral("terminalView"));
    view_->setReadOnly(true);
    view_->setUndoRedoEnabled(false);
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The terminal has its own scroll bar, because the scrollback belongs to
    // the emulator: the document shows one screenful at a time, and the bar
    // scrolls the model, not the text.
    view_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view_->setFrameShape(QFrame::NoFrame);
    view_->setWordWrapMode(QTextOption::NoWrap);
    view_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    view_->document()->setDocumentMargin(0);
    // The view is a rendering surface: no caret, and the palette is set here
    // rather than through a stylesheet so a light theme does not repaint the
    // terminal white behind the cells.
    view_->setCursorWidth(0);
    QPalette palette = view_->palette();
    palette.setColor(QPalette::Base, QColor(QLatin1String(kBackground)));
    palette.setColor(QPalette::Text, QColor(QLatin1String(kForeground)));
    view_->setPalette(palette);
    view_->installEventFilter(this);

    scrollBar_ = new QScrollBar(Qt::Vertical, this);
    scrollBar_->setObjectName(QStringLiteral("terminalScrollBar"));
    scrollBar_->setSingleStep(1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("terminalStatus"));
    status_->setVisible(false);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(view_, 1);
    row->addWidget(scrollBar_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(row, 1);
    layout->addWidget(status_);

    repaintTimer_ = new QTimer(this);
    repaintTimer_->setSingleShot(true);
    repaintTimer_->setInterval(0);
    connect(repaintTimer_, &QTimer::timeout, this, &TerminalWidget::repaintNow);

    connect(scrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
        if (fillingScrollBar_) {
            return;
        }
        const int total = emulator_->rows() + emulator_->scrollbackCount();
        const int offset = total - emulator_->rows() - value;
        emulator_->setScrollOffset(std::max(0, offset));
        userScrolledBack_ = offset > 0;
        scheduleRepaint();
    });

    setTerminalFont(font_);
    resizeEmulatorToViewport();
    updateScrollBar();
    scheduleRepaint();
}

TerminalWidget::~TerminalWidget() = default;

// --- geometry -----------------------------------------------------------

void TerminalWidget::setTerminalFont(const QFont& font)
{
    if (font.family().isEmpty()) {
        // A default-constructed QFont is not a request for a specific font;
        // applying it would silently replace the monospace face with the
        // application default and break the column grid.
        return;
    }
    font_ = font;
    font_.setStyleHint(QFont::Monospace);
    font_.setFixedPitch(true);
    view_->setFont(font_);
    rebuildFontMetrics();
    resizeEmulatorToViewport();
    scheduleRepaint();
}

void TerminalWidget::rebuildFontMetrics()
{
    const QFontMetricsF metrics(font_);
    // A monospace font can still report a fractional advance; rounding *up*
    // keeps the grid from drifting left over eighty columns.
    cellWidth_ = std::max(1, int(std::ceil(metrics.horizontalAdvance(QLatin1Char('M')))));
    cellHeight_ = std::max(1, int(std::ceil(metrics.lineSpacing())));
    // A fixed line height is what makes a row of the document the same
    // height as a row of the grid; without it, wide characters that fall
    // back to another font grow the row and the cursor drifts.
    QTextCursor cursor(view_->document());
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(fixedLineHeight(cellHeight_));
    cursor.mergeCharFormat(baseFormat(font_));
}

QSize TerminalWidget::gridSize() const
{
    const QSize area = view_->viewport()->size();
    return QSize(std::max(1, area.width() / cellWidth_),
                 std::max(1, area.height() / cellHeight_));
}

void TerminalWidget::resizeEmulatorToViewport()
{
    const QSize grid = gridSize();
    if (grid.width() == emulator_->columns() && grid.height() == emulator_->rows()) {
        return;
    }
    emulator_->resize(grid.width(), grid.height());
    updateScrollBar();
    scheduleRepaint();
    Q_EMIT gridResized(grid.width(), grid.height());
}

void TerminalWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    resizeEmulatorToViewport();
}

// --- feeding ------------------------------------------------------------

void TerminalWidget::feed(const QByteArray& bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    const Term::EmulatorUpdate update = emulator_->feed(bytes);
    if (!update.title.isEmpty() && update.title != title_) {
        title_ = update.title;
        Q_EMIT titleChanged(title_);
    }
    scrollToLive();
    scheduleRepaint();
}

void TerminalWidget::setExited(int exitCode)
{
    exited_ = true;
    // A shell that exits leaves the screen looking live. Saying so is the
    // difference between "the terminal is broken" and "the command ended".
    setStatus(exitCode == 0 ? tr("Process finished")
                            : tr("Process finished with exit code %1").arg(exitCode));
}

void TerminalWidget::setExitedWithError(const QString& reason)
{
    exited_ = true;
    setStatus(reason.isEmpty() ? tr("Process failed to start") : reason);
}

void TerminalWidget::setStatus(const QString& text)
{
    status_->setText(text);
    status_->setVisible(!text.isEmpty());
}

void TerminalWidget::addMark(const Shell::Mark& mark)
{
    tracker_->feed(mark);
}

// --- rendering ----------------------------------------------------------

void TerminalWidget::scheduleRepaint()
{
    if (!repaintTimer_->isActive()) {
        repaintTimer_->start();
    }
}

void TerminalWidget::repaintNow()
{
    const int rows = emulator_->rows();
    const int columns = emulator_->columns();
    const int offset = emulator_->scrollOffset();
    const QStringList history = emulator_->scrollbackLines();
    const int historyRows = int(history.size());

    // Row list for the viewport. When scrolled back the scrollback is what
    // is on screen: the rows above the live screen are exactly what the
    // user asked to read.
    QList<QList<Term::StyledRun>> runs;
    QStringList plain;
    if (offset > 0) {
        const QList<QList<Term::StyledRun>> screen = emulator_->screenRuns();
        const int start = std::max(0, historyRows - offset);
        for (int i = 0; i < rows; ++i) {
            const int index = start + i;
            if (index < historyRows) {
                // The scrollback keeps plain text only, so a row that has
                // scrolled off loses its colour — the same compromise every
                // terminal that caps its history makes, and better than
                // dropping the line.
                plain.append(history.at(index));
                runs.append(QList<Term::StyledRun>{});
            } else {
                const int screenRow = index - historyRows;
                if (screenRow >= 0 && screenRow < screen.size()) {
                    runs.append(screen.at(screenRow));
                } else {
                    runs.append(QList<Term::StyledRun>{});
                }
                plain.append(QString());
            }
        }
    } else {
        runs = emulator_->screenRuns();
        for (int i = 0; i < runs.size(); ++i) {
            QString text;
            for (const Term::StyledRun& run : runs.at(i)) {
                text += run.text;
            }
            plain.append(text);
        }
    }
    while (runs.size() < rows) {
        runs.append(QList<Term::StyledRun>{});
        plain.append(QString());
    }
    if (runs.size() > rows) {
        runs = runs.mid(0, rows);
    }

    view_->setUpdatesEnabled(false);
    QTextCursor cursor(view_->document());
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.removeSelectedText();

    const QTextBlockFormat block = fixedLineHeight(cellHeight_);
    const QTextCharFormat normal = formatFor(Term::Attributes{});
    bool first = true;
    for (int row = 0; row < rows; ++row) {
        if (!first) {
            cursor.insertBlock(block, normal);
        }
        first = false;
        const QList<Term::StyledRun> rowRuns = runs.at(row);
        if (rowRuns.isEmpty()) {
            // Pad with spaces so a row is still one full line: an empty
            // block would collapse to zero height and shift every row below.
            cursor.insertText(QString(columns, QLatin1Char(' ')), normal);
            continue;
        }
        for (const Term::StyledRun& run : rowRuns) {
            if (run.text.isEmpty()) {
                continue;
            }
            cursor.insertText(run.text, formatFor(run.attributes));
        }
    }
    cursor.endEditBlock();
    view_->setUpdatesEnabled(true);

    scrollToLive();
    updateScrollBar();
}

QTextCharFormat TerminalWidget::formatFor(const Term::Attributes& attributes) const
{
    const QColor background = QColor(QLatin1String(kBackground));
    QColor foreground = QColor(QLatin1String(kForeground));

    if (attributes.fgIsPalette) {
        foreground = QColor::fromRgb(Term::Emulator::paletteColor(int(attributes.fg)));
    } else if (attributes.fg != 0) {
        foreground = QColor::fromRgb(attributes.fg);
    }
    QColor cellBackground = background;
    if (attributes.bgIsPalette) {
        cellBackground = QColor::fromRgb(Term::Emulator::paletteColor(int(attributes.bg)));
    } else if (attributes.bg != 0) {
        cellBackground = QColor::fromRgb(attributes.bg);
    }
    if (attributes.inverse) {
        const QColor swapped = foreground;
        foreground = cellBackground;
        cellBackground = swapped;
    }
    if (attributes.hidden) {
        foreground = cellBackground;
    } else if (attributes.dim) {
        // Dim is a 50% blend towards the background — the closest Qt gets
        // without a custom painter, and what most terminals do.
        foreground = QColor((foreground.red() + cellBackground.red()) / 2,
                            (foreground.green() + cellBackground.green()) / 2,
                            (foreground.blue() + cellBackground.blue()) / 2);
    }

    QTextCharFormat format = baseFormat(font_);
    format.setForeground(foreground);
    // Only paint a background where the cell asked for one: a per-character
    // background on every cell would make selection paint nothing visible.
    if (cellBackground != background) {
        format.setBackground(cellBackground);
    }
    format.setFontWeight(attributes.bold ? QFont::Bold : QFont::Normal);
    format.setFontItalic(attributes.italic);
    format.setFontUnderline(attributes.underline);
    if (attributes.strike) {
        format.setFontStrikeOut(true);
    }
    return format;
}

void TerminalWidget::scrollToLive()
{
    if (userScrolledBack_ || !view_->isVisible()) {
        return;
    }
    QTextCursor cursor(view_->document());
    cursor.movePosition(QTextCursor::End);
    view_->setTextCursor(cursor);
    view_->ensureCursorVisible();
}

void TerminalWidget::updateScrollBar()
{
    const int total = emulator_->rows() + emulator_->scrollbackCount();
    const int maximum = std::max(0, total - emulator_->rows());
    fillingScrollBar_ = true;
    scrollBar_->setRange(0, maximum);
    scrollBar_->setPageStep(emulator_->rows());
    scrollBar_->setValue(maximum - emulator_->scrollOffset());
    scrollBar_->setEnabled(maximum > 0);
    fillingScrollBar_ = false;
}

// --- input --------------------------------------------------------------

void TerminalWidget::sendText(const QString& text)
{
    if (text.isEmpty()) {
        return;
    }
    // Typed text becomes bytes for the PTY; the emulator does not see it
    // until the shell echoes it back, which is what keeps local echo from
    // duplicating every character.
    Q_EMIT bytesTyped(text.toUtf8());
    typed_ += text;
}

void TerminalWidget::sendKey(const QByteArray& sequence)
{
    if (!sequence.isEmpty()) {
        Q_EMIT bytesTyped(sequence);
    }
}

void TerminalWidget::handleKeyPress(QKeyEvent* event)
{
    if (exited_) {
        event->accept();
        return;
    }
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool control = mods.testFlag(Qt::ControlModifier) && !mods.testFlag(Qt::AltModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);

    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        sendKey("\r");
        Q_EMIT commandEntered(typed_);
        typed_.clear();
        event->accept();
        return;
    case Qt::Key_Backspace:
        if (!typed_.isEmpty()) {
            typed_.chop(1);
        }
        // Ctrl+Backspace deletes a word, which readline spells as ^W.
        sendKey(control ? QByteArray("\x17") : QByteArray("\x7f"));
        event->accept();
        return;
    case Qt::Key_Tab:
        sendKey("\t");
        event->accept();
        return;
    case Qt::Key_Escape:
        sendKey("\x1b");
        typed_.clear();
        event->accept();
        return;
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Right:
    case Qt::Key_Left:
    case Qt::Key_Home:
    case Qt::Key_End: {
        char final = 'A';
        switch (event->key()) {
        case Qt::Key_Up:
            final = 'A';
            break;
        case Qt::Key_Down:
            final = 'B';
            break;
        case Qt::Key_Right:
            final = 'C';
            break;
        case Qt::Key_Left:
            final = 'D';
            break;
        case Qt::Key_Home:
            final = 'H';
            break;
        default:
            final = 'F';
            break;
        }
        QByteArray sequence = modifierSequence(final, mods);
        if (sequence.isEmpty()) {
            if (final == 'H' || final == 'F') {
                // Home/End are "H"/"F" only in application mode; every shell
                // expects the "1~"/"4~" form otherwise.
                sequence = QByteArray("\x1b[") + (final == 'H' ? QByteArray("1~")
                                                               : QByteArray("4~"));
            } else {
                sequence = cursorSequence(final, emulator_->cursorKeysApplicationMode());
            }
        }
        sendKey(sequence);
        event->accept();
        return;
    }
    case Qt::Key_Delete:
        sendKey("\x1b[3~");
        event->accept();
        return;
    case Qt::Key_Insert:
        sendKey("\x1b[2~");
        event->accept();
        return;
    case Qt::Key_PageUp:
        sendKey("\x1b[5~");
        event->accept();
        return;
    case Qt::Key_PageDown:
        sendKey("\x1b[6~");
        event->accept();
        return;
    default:
        break;
    }

    if (control && event->key() == Qt::Key_Space) {
        // Ctrl+Space is NUL, which readline uses for "set the mark".
        sendKey(QByteArray(1, char(0)));
        event->accept();
        return;
    }
    if (event->text().isEmpty()) {
        event->ignore();
        return;
    }
    if (control) {
        // Ctrl+letter is the control code at the letter's ASCII position
        // minus '@': ^A = 0x01, ^Z = 0x1a.
        const char16_t ch = event->text().at(0).toUpper().unicode();
        if (ch >= u'A' && ch <= u'Z') {
            sendKey(QByteArray(1, char(int(ch) - int(u'@'))));
            event->accept();
            return;
        }
        if (ch == u'[') {
            sendKey(QByteArray("\x1b"));
            event->accept();
            return;
        }
    }
    const QByteArray text = event->text().toUtf8();
    if (alt) {
        // Alt+key is ESC followed by the key, the convention every shell
        // tries first.
        QByteArray sequence("\x1b");
        sequence.append(text);
        sendKey(sequence);
        event->accept();
        return;
    }
    sendText(QString::fromUtf8(text));
    event->accept();
}

bool TerminalWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != view_) {
        return QWidget::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::KeyPress: {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool shift = key->modifiers().testFlag(Qt::ShiftModifier);
        const bool control = key->modifiers().testFlag(Qt::ControlModifier);
        // Ctrl+Shift+C / Ctrl+Shift+V are the terminal's copy and paste.
        // Plain Ctrl+C must reach the shell as an interrupt — the one
        // keystroke a user cannot afford to have the IDE eat.
        if (control && shift && key->key() == Qt::Key_C) {
            copySelection();
            return true;
        }
        if (control && shift && key->key() == Qt::Key_V) {
            pasteFromClipboard();
            return true;
        }
        if (control && !shift && !key->modifiers().testFlag(Qt::AltModifier)
            && key->key() == Qt::Key_L) {
            // Local clear, like a terminal's own Ctrl+L: it does not reach
            // the shell, because a shell would print a new prompt over the
            // old one and leave the scrollback in place.
            clearScreen();
            return true;
        }
        handleKeyPress(key);
        return true;
    }
    case QEvent::MouseButtonPress:
        // A click focuses the terminal, not the read-only text view, so the
        // next keystroke is not lost; and it snaps back to the live screen.
        setFocus(Qt::MouseFocusReason);
        userScrolledBack_ = false;
        emulator_->setScrollOffset(0);
        updateScrollBar();
        scheduleRepaint();
        return false;
    case QEvent::Wheel:
        // A wheel turn means the user wants the history; the next byte must
        // not yank them back to the bottom. The scroll happens on the next
        // turn of the event loop, after Qt has moved the view.
        QTimer::singleShot(0, this, [this]() {
            if (emulator_->scrollOffset() > 0) {
                userScrolledBack_ = true;
            }
            updateScrollBar();
            scheduleRepaint();
        });
        return false;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void TerminalWidget::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    view_->setFocus(Qt::OtherFocusReason);
}

void TerminalWidget::copySelection()
{
    const QTextCursor cursor = view_->textCursor();
    QString text;
    if (cursor.hasSelection()) {
        text = cursor.selectedText();
        // QTextDocument stores paragraph breaks as U+2029; a shell wants a
        // newline.
        text.replace(QChar(0x2029), QLatin1Char('\n'));
    }
    if (text.isEmpty()) {
        text = emulator_->textBetween(0, 0, emulator_->rows() - 1, emulator_->columns() - 1);
    }
    if (text.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(text);
    setStatus(tr("Copied %1 characters").arg(text.size()));
}

void TerminalWidget::pasteFromClipboard()
{
    QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) {
        return;
    }
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (emulator_->bracketedPaste()) {
        // Bracketed paste is the only way a shell can tell a pasted newline
        // from a typed one, and therefore the only way pasting a script does
        // not execute half of it line by line.
        QByteArray bytes("\x1b[200~");
        bytes.append(text.toUtf8());
        bytes.append("\x1b[201~");
        sendKey(bytes);
        return;
    }
    sendText(text);
}

void TerminalWidget::clearScreen()
{
    // The update is discarded on purpose: a manual clear repaints the whole
    // viewport regardless, so there is nothing to take from it.
    const Term::EmulatorUpdate ignored = emulator_->feedText(QStringLiteral("\x1b[2J\x1b[H"));
    Q_UNUSED(ignored)
    userScrolledBack_ = false;
    scheduleRepaint();
    Q_EMIT cleared();
}

void TerminalWidget::clearScrollback()
{
    emulator_->clearScrollback();
    userScrolledBack_ = false;
    updateScrollBar();
    scheduleRepaint();
    Q_EMIT cleared();
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu menu(this);
    QAction* copy = menu.addAction(tr("Copy"));
    connect(copy, &QAction::triggered, this, &TerminalWidget::copySelection);
    QAction* paste = menu.addAction(tr("Paste"));
    connect(paste, &QAction::triggered, this, &TerminalWidget::pasteFromClipboard);
    menu.addSeparator();
    QAction* selectAll = menu.addAction(tr("Select All"));
    connect(selectAll, &QAction::triggered, this, [this]() {
        QTextCursor cursor(view_->document());
        cursor.select(QTextCursor::Document);
        view_->setTextCursor(cursor);
    });
    menu.addSeparator();
    QAction* screen = menu.addAction(tr("Clear Screen"));
    connect(screen, &QAction::triggered, this, &TerminalWidget::clearScreen);
    QAction* history = menu.addAction(tr("Clear Scrollback"));
    connect(history, &QAction::triggered, this, &TerminalWidget::clearScrollback);
    menu.exec(event->globalPos());
}
