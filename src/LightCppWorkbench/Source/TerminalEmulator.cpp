#include "LightCpp/Workbench/TerminalEmulator.hpp"

#include <QChar>

#include <algorithm>

namespace LightCpp::Workbench::Terminal {

namespace {

constexpr int kTabWidth = 8;

/// The DEC special graphics table, for the characters a TUI uses for boxes.
QChar decSpecial(QChar ch)
{
    switch (ch.unicode()) {
    case 'j': return QChar(0x2518);  // ┘
    case 'k': return QChar(0x2510);  // ┐
    case 'l': return QChar(0x250C);  // ┌
    case 'm': return QChar(0x2514);  // └
    case 'n': return QChar(0x253C);  // ┼
    case 'q': return QChar(0x2500);  // ─
    case 't': return QChar(0x251C);  // ├
    case 'u': return QChar(0x2524);  // ┤
    case 'v': return QChar(0x2534);  // ┴
    case 'w': return QChar(0x252C);  // ┬
    case 'x': return QChar(0x2502);  // │
    case 'a': return QChar(0x2592);  // ▒
    case '~': return QChar(0x00B7);  // ·
    case '0': return QChar(0x2588);  // █
    default:  return ch;
    }
}

/// How many cells a grapheme occupies. Qt 6 splits the string into grapheme
/// clusters, so a combining mark or an emoji sequence arrives as one unit;
/// the width rule is the one terminals use: East Asian wide is 2, everything
/// else is 1.
int glyphWidth(const QString& glyph)
{
    if (glyph.isEmpty()) {
        return 0;
    }
    if (glyph.size() == 1) {
        const QChar ch = glyph.at(0);
        const uint u = ch.unicode();
        if (ch.isLowSurrogate()) {
            return 1;  // handled by the pair below
        }
        if (ch.isHighSurrogate()) {
            return 2;  // astral: almost all emoji / CJK ext
        }
        // Combining marks occupy no cells of their own.
        if (u >= 0x0300 && u <= 0x036F) {
            return 0;
        }
        if (u == 0x200B || u == 0xFEFF) {
            return 0;
        }
        const bool wide =
            (u >= 0x1100 && u <= 0x115F) || (u >= 0x2E80 && u <= 0xA4CF)
            || (u >= 0xAC00 && u <= 0xD7A3) || (u >= 0xF900 && u <= 0xFAFF)
            || (u >= 0xFE30 && u <= 0xFE6F) || (u >= 0xFF00 && u <= 0xFF60)
            || (u >= 0xFFE0 && u <= 0xFFE6);
        return wide ? 2 : 1;
    }
    // Several code points in one cluster: an emoji ZWJ sequence or a
    // grapheme with combining marks. Emoji are wide; a decorated ASCII
    // letter is not. QChar holds UTF-16 units, so an astral code point
    // arrives as a surrogate pair — a high surrogate means "wide".
    for (const QChar ch : glyph) {
        if (ch.isHighSurrogate()) {
            return 2;
        }
        const ushort u = ch.unicode();
        if (u >= 0x2600 && u <= 0x27BF) {
            return 2;
        }
    }
    return 1;
}

Cell blankCell(const Attributes& attributes)
{
    Cell cell;
    cell.text = QStringLiteral(" ");
    cell.attributes = attributes;
    return cell;
}

}  // namespace

bool Attributes::operator==(const Attributes& other) const
{
    return fg == other.fg && bg == other.bg && fgIsPalette == other.fgIsPalette
           && bgIsPalette == other.bgIsPalette && bold == other.bold
           && dim == other.dim && italic == other.italic
           && underline == other.underline && blink == other.blink
           && inverse == other.inverse && hidden == other.hidden
           && strike == other.strike;
}

Emulator::Emulator(int columns, int rows, int scrollback)
    : columns_(std::max(1, columns)),
      rows_(std::max(1, rows)),
      scrollbackLimit_(std::max(0, scrollback))
{
    reset();
}

void Emulator::reset()
{
    screen_.clear();
    alternate_.clear();
    for (int i = 0; i < rows_; ++i) {
        screen_.append(QList<Cell>(columns_, blankCell(Attributes{})));
    }
    cursorColumn_ = 0;
    cursorRow_ = 0;
    current_ = Attributes{};
    cursorVisible_ = true;
    alternateScreen_ = false;
    applicationCursorKeys_ = false;
    bracketedPaste_ = false;
    autoWrap_ = true;
    pendingWrap_ = false;
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    scrollOffset_ = 0;
    lineDrawing_ = false;
    pending_.clear();
    stringState_ = StringState::None;
    stringBuffer_.clear();
}

void Emulator::ensureLine(int row)
{
    while (screen_.size() <= row) {
        screen_.append(QList<Cell>(columns_, blankCell(Attributes{})));
    }
}

QList<Cell>& Emulator::lineAt(int row)
{
    ensureLine(row);
    QList<Cell>& line = screen_[row];
    // A row that was created before a resize may be short; keep every row the
    // same width so a cell lookup never has to range-check twice.
    while (line.size() < columns_) {
        line.append(blankCell(current_));
    }
    return line;
}

const QList<Cell>& Emulator::lineAt(int row) const
{
    static const QList<Cell> empty;
    return row >= 0 && row < screen_.size() ? screen_.at(row) : empty;
}

void Emulator::markDirty(int row)
{
    if (update_.fullRepaint) {
        return;
    }
    if (!update_.dirtyRows.contains(row)) {
        update_.dirtyRows.append(row);
    }
}

void Emulator::resize(int columns, int rows)
{
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    if (columns == columns_ && rows == rows_) {
        return;
    }
    const int oldRows = rows_;
    // Growing rows: append blanks. Shrinking: drop from the top into
    // scrollback, so the output that scrolled away stays reachable.
    if (rows < oldRows) {
        const int drop = oldRows - rows;
        for (int i = 0; i < drop && i < screen_.size(); ++i) {
            if (scrollbackLimit_ > 0) {
                QStringList text;
                for (const Cell& cell : screen_.at(i)) {
                    text << cell.text;
                }
                scrollback_.append(text);
            }
        }
        while (scrollback_.size() > scrollbackLimit_) {
            scrollback_.removeFirst();
        }
        screen_ = screen_.mid(drop);
        cursorRow_ = std::max(0, cursorRow_ - drop);
    }
    while (screen_.size() < rows) {
        screen_.append(QList<Cell>(columns, blankCell(current_)));
    }
    for (QList<Cell>& line : screen_) {
        if (line.size() < columns) {
            while (line.size() < columns) {
                line.append(blankCell(current_));
            }
        } else if (line.size() > columns) {
            line = line.mid(0, columns);
        }
    }
    columns_ = columns;
    rows_ = rows;
    cursorColumn_ = qBound(0, cursorColumn_, columns_ - 1);
    cursorRow_ = qBound(0, cursorRow_, rows_ - 1);
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    scrollOffset_ = 0;
    update_.fullRepaint = true;
}

void Emulator::putGlyph(const QString& glyph, int width)
{
    if (width <= 0) {
        // A combining mark: attach it to the cell to the left instead of
        // occupying one of its own, which is what makes `e` + U+0301 one
        // cell rather than two.
        if (cursorColumn_ > 0 && cursorRow_ >= 0 && cursorRow_ < screen_.size()) {
            QList<Cell>& line = lineAt(cursorRow_);
            const int target = cursorColumn_ - 1;
            if (target >= 0 && target < line.size()) {
                line[target].text += glyph;
                markDirty(cursorRow_);
            }
        }
        return;
    }
    if (pendingWrap_ && autoWrap_) {
        pendingWrap_ = false;
        carriageReturn();
        newline();
    }
    const int maxColumn = columns_ - width;
    if (cursorColumn_ > maxColumn) {
        if (autoWrap_) {
            carriageReturn();
            newline();
        } else {
            cursorColumn_ = std::max(0, maxColumn);
        }
    }

    QList<Cell>& line = lineAt(cursorRow_);
    line[cursorColumn_].text = glyph;
    line[cursorColumn_].attributes = current_;
    if (width == 2 && cursorColumn_ + 1 < line.size()) {
        // The second cell is a continuation: empty text, so copying the row
        // does not produce a stray space for every wide character.
        line[cursorColumn_ + 1].text = QString();
        line[cursorColumn_ + 1].attributes = current_;
    }
    markDirty(cursorRow_);
    cursorColumn_ += width;
    if (cursorColumn_ >= columns_) {
        cursorColumn_ = columns_ - 1;
        pendingWrap_ = true;
    }
}

void Emulator::newline()
{
    // The cursor moving down one row. Only when it is *at* the bottom of the
    // scroll region does that mean scrolling the region; a cursor below the
    // region (a status line) just moves within the screen.
    if (cursorRow_ == scrollBottom_) {
        scrollUp();
    } else if (cursorRow_ < rows_ - 1) {
        ++cursorRow_;
    }
}

void Emulator::carriageReturn()
{
    cursorColumn_ = 0;
    pendingWrap_ = false;
}

void Emulator::scrollUp(int count)
{
    count = qBound(1, count, scrollBottom_ - scrollTop_ + 1);
    for (int i = 0; i < count; ++i) {
        if (scrollTop_ == 0 && !alternateScreen_ && scrollbackLimit_ > 0) {
            QStringList text;
            const QList<Cell>& first = lineAt(0);
            for (const Cell& cell : first) {
                text << cell.text;
            }
            scrollback_.append(text);
            while (scrollback_.size() > scrollbackLimit_) {
                scrollback_.removeFirst();
            }
        }
        screen_.removeAt(scrollTop_);
        screen_.insert(scrollBottom_, QList<Cell>(columns_, blankCell(current_)));
    }
    update_.fullRepaint = true;
}

void Emulator::scrollDown(int count)
{
    count = qBound(1, count, scrollBottom_ - scrollTop_ + 1);
    for (int i = 0; i < count; ++i) {
        screen_.removeAt(scrollBottom_);
        screen_.insert(scrollTop_, QList<Cell>(columns_, blankCell(current_)));
    }
    update_.fullRepaint = true;
}

void Emulator::eraseInLine(int mode)
{
    QList<Cell>& line = lineAt(cursorRow_);
    switch (mode) {
    case 0:  // cursor to end
        for (int i = cursorColumn_; i < line.size(); ++i) {
            line[i] = blankCell(current_);
        }
        break;
    case 1:  // start to cursor
        for (int i = 0; i <= cursorColumn_ && i < line.size(); ++i) {
            line[i] = blankCell(current_);
        }
        break;
    case 2:  // whole line
        for (Cell& cell : line) {
            cell = blankCell(current_);
        }
        break;
    default:
        return;
    }
    markDirty(cursorRow_);
}

void Emulator::eraseInDisplay(int mode)
{
    switch (mode) {
    case 0:
        eraseInLine(0);
        for (int row = cursorRow_ + 1; row < rows_; ++row) {
            QList<Cell>& line = lineAt(row);
            for (Cell& cell : line) {
                cell = blankCell(Attributes{});
            }
            markDirty(row);
        }
        break;
    case 1:
        for (int row = 0; row < cursorRow_; ++row) {
            QList<Cell>& line = lineAt(row);
            for (Cell& cell : line) {
                cell = blankCell(Attributes{});
            }
            markDirty(row);
        }
        eraseInLine(1);
        break;
    case 2:
    case 3:
        for (int row = 0; row < rows_; ++row) {
            QList<Cell>& line = lineAt(row);
            for (Cell& cell : line) {
                cell = blankCell(Attributes{});
            }
        }
        if (mode == 3) {
            // `CSI 3 J` also clears the scrollback. Kept as a separate mode
            // because "clear the screen" and "clear the history" are
            // different requests and users complain when a clear eats their
            // scrollback.
            scrollback_.clear();
        }
        update_.fullRepaint = true;
        break;
    default:
        break;
    }
}

void Emulator::applySgr(const QList<int>& parameters)
{
    if (parameters.isEmpty()) {
        current_ = Attributes{};
        return;
    }
    for (int i = 0; i < parameters.size(); ++i) {
        const int code = parameters.at(i);
        switch (code) {
        case 0: current_ = Attributes{}; break;
        case 1: current_.bold = true; break;
        case 2: current_.dim = true; break;
        case 3: current_.italic = true; break;
        case 4: current_.underline = true; break;
        case 5:
        case 6: current_.blink = true; break;
        case 7: current_.inverse = true; break;
        case 8: current_.hidden = true; break;
        case 9: current_.strike = true; break;
        case 21:
        case 22: current_.bold = false; current_.dim = false; break;
        case 23: current_.italic = false; break;
        case 24: current_.underline = false; break;
        case 25: current_.blink = false; break;
        case 27: current_.inverse = false; break;
        case 28: current_.hidden = false; break;
        case 29: current_.strike = false; break;
        case 39: current_.fg = 0; current_.fgIsPalette = false; break;
        case 49: current_.bg = 0; current_.bgIsPalette = false; break;
        case 38:
        case 48: {
            // Extended colour: `5;n` for a palette index, `2;r;g;b` for
            // truecolor. The parameters are shared with the foreground /
            // background selection, so they are consumed here.
            const bool isForeground = code == 38;
            if (i + 1 >= parameters.size()) {
                break;
            }
            const int mode = parameters.at(++i);
            if (mode == 5 && i + 1 < parameters.size()) {
                const int index = parameters.at(++i);
                if (isForeground) {
                    current_.fg = quint32(qBound(0, index, 255));
                    current_.fgIsPalette = true;
                } else {
                    current_.bg = quint32(qBound(0, index, 255));
                    current_.bgIsPalette = true;
                }
            } else if (mode == 2 && i + 3 < parameters.size()) {
                const int r = parameters.at(++i);
                const int g = parameters.at(++i);
                const int b = parameters.at(++i);
                const quint32 rgb = quint32((qBound(0, r, 255) << 16)
                                            | (qBound(0, g, 255) << 8)
                                            | qBound(0, b, 255));
                if (isForeground) {
                    current_.fg = rgb;
                    current_.fgIsPalette = false;
                } else {
                    current_.bg = rgb;
                    current_.bgIsPalette = false;
                }
            }
            break;
        }
        default:
            if (code >= 30 && code <= 37) {
                current_.fg = quint32(code - 30);
                current_.fgIsPalette = true;
            } else if (code >= 40 && code <= 47) {
                current_.bg = quint32(code - 40);
                current_.bgIsPalette = true;
            } else if (code >= 90 && code <= 97) {
                current_.fg = quint32(code - 90 + 8);
                current_.fgIsPalette = true;
            } else if (code >= 100 && code <= 107) {
                current_.bg = quint32(code - 100 + 8);
                current_.bgIsPalette = true;
            }
            break;
        }
    }
}

void Emulator::switchAlternateScreen(bool on)
{
    if (on == alternateScreen_) {
        return;
    }
    if (on) {
        alternate_ = screen_;
        screen_.clear();
        for (int i = 0; i < rows_; ++i) {
            screen_.append(QList<Cell>(columns_, blankCell(Attributes{})));
        }
    } else {
        screen_ = alternate_;
        alternate_.clear();
    }
    alternateScreen_ = on;
    cursorColumn_ = 0;
    cursorRow_ = 0;
    update_.fullRepaint = true;
}

bool Emulator::handleCsi(const QString& parameters, QChar final)
{
    // Private markers: `?` for DEC modes, `>` for the "secondary device
    // attributes" form, `<` for mouse. Parsed off before the numbers.
    QString numbers = parameters;
    QChar prefix;
    if (!numbers.isEmpty() && !numbers.at(0).isDigit() && numbers.at(0) != QLatin1Char(';')) {
        prefix = numbers.at(0);
        numbers.remove(0, 1);
    }
    QList<int> values;
    for (const QString& piece : numbers.split(QLatin1Char(';'))) {
        values.append(piece.isEmpty() ? 0 : piece.toInt());
    }
    if (values.isEmpty()) {
        values.append(0);
    }
    const int first = values.first();
    const int count = qMax(1, first);

    switch (final.unicode()) {
    case 'A':  // cursor up
        cursorRow_ = qMax(0, cursorRow_ - count);
        pendingWrap_ = false;
        return true;
    case 'B':  // cursor down
        cursorRow_ = qMin(rows_ - 1, cursorRow_ + count);
        pendingWrap_ = false;
        return true;
    case 'C':  // cursor forward
        cursorColumn_ = qMin(columns_ - 1, cursorColumn_ + count);
        pendingWrap_ = false;
        return true;
    case 'D':  // cursor back
        cursorColumn_ = qMax(0, cursorColumn_ - count);
        pendingWrap_ = false;
        return true;
    case 'E':  // next line
        cursorRow_ = qMin(rows_ - 1, cursorRow_ + count);
        cursorColumn_ = 0;
        return true;
    case 'F':  // previous line
        cursorRow_ = qMax(0, cursorRow_ - count);
        cursorColumn_ = 0;
        return true;
    case 'G':  // column absolute
    case '`':
        cursorColumn_ = qBound(0, count - 1, columns_ - 1);
        return true;
    case 'H':  // cursor position
    case 'f':
        cursorRow_ = qBound(0, (values.size() > 0 ? qMax(1, values.at(0)) : 1) - 1,
                            rows_ - 1);
        cursorColumn_ = qBound(0, (values.size() > 1 ? qMax(1, values.at(1)) : 1) - 1,
                               columns_ - 1);
        pendingWrap_ = false;
        return true;
    case 'J':
        eraseInDisplay(first);
        return true;
    case 'K':
        eraseInLine(first);
        return true;
    case 'L':  // insert lines
        for (int i = 0; i < count; ++i) {
            screen_.removeAt(scrollBottom_);
            screen_.insert(cursorRow_, QList<Cell>(columns_, blankCell(current_)));
        }
        update_.fullRepaint = true;
        return true;
    case 'M':  // delete lines
        for (int i = 0; i < count; ++i) {
            screen_.removeAt(cursorRow_);
            screen_.insert(scrollBottom_, QList<Cell>(columns_, blankCell(current_)));
        }
        update_.fullRepaint = true;
        return true;
    case 'P': {  // delete characters
        QList<Cell>& line = lineAt(cursorRow_);
        for (int i = 0; i < count && cursorColumn_ < line.size(); ++i) {
            line.removeAt(cursorColumn_);
            line.append(blankCell(current_));
        }
        markDirty(cursorRow_);
        return true;
    }
    case '@': {  // insert characters
        QList<Cell>& line = lineAt(cursorRow_);
        for (int i = 0; i < count && cursorColumn_ < line.size(); ++i) {
            line.insert(cursorColumn_, blankCell(current_));
            line.removeLast();
        }
        markDirty(cursorRow_);
        return true;
    }
    case 'X': {  // erase characters
        QList<Cell>& line = lineAt(cursorRow_);
        for (int i = 0; i < count && cursorColumn_ + i < line.size(); ++i) {
            line[cursorColumn_ + i] = blankCell(current_);
        }
        markDirty(cursorRow_);
        return true;
    }
    case 'S':  // scroll up
        scrollUp(count);
        return true;
    case 'T':  // scroll down
        scrollDown(count);
        return true;
    case 'm':
        applySgr(values);
        return true;
    case 'r': {  // set scrolling region
        // Both numbers are inclusive 1-based row numbers. A region of one
        // row (`1;1r`) is legal and means "scroll only row 1", so the test is
        // bottom >= top, not bottom > top — treating a single-row region as
        // invalid silently falls back to the whole screen.
        const int top = qMax(1, values.value(0, 1));
        const int bottom = qBound(1, values.size() > 1 && values.at(1) > 0
                                         ? values.at(1) : rows_,
                                  rows_);
        if (bottom >= top) {
            scrollTop_ = qBound(0, top - 1, rows_ - 1);
            scrollBottom_ = qBound(0, bottom - 1, rows_ - 1);
            cursorRow_ = scrollTop_;
            cursorColumn_ = 0;
        }
        return true;
    }
    case 'h':
    case 'l': {
        const bool on = final == QLatin1Char('h');
        if (prefix != QLatin1Char('?')) {
            // ANSI modes: 4 = insert mode (not modelled), 20 = newline mode.
            return true;
        }
        for (const int mode : values) {
            switch (mode) {
            case 1: applicationCursorKeys_ = on; break;
            case 7: autoWrap_ = on; break;
            case 25: cursorVisible_ = on; break;
            case 47:
            case 1047:
            case 1049: switchAlternateScreen(on); break;
            case 2004: bracketedPaste_ = on; break;
            default: break;
            }
        }
        return true;
    }
    case 's':  // save cursor (ANSI.SYS)
    case 'u':  // restore cursor
        return true;
    default:
        return false;
    }
}

bool Emulator::handleEsc(QChar final)
{
    switch (final.unicode()) {
    case '7':  // DECSC save cursor
        return true;
    case '8':  // DECRC restore cursor
        return true;
    case 'D':  // index — newline without carriage return
        newline();
        return true;
    case 'E':  // next line
        carriageReturn();
        newline();
        return true;
    case 'M':  // reverse index
        if (cursorRow_ == scrollTop_) {
            scrollDown();
        } else if (cursorRow_ > 0) {
            --cursorRow_;
        }
        return true;
    case 'c':  // RIS — full reset
        reset();
        update_.fullRepaint = true;
        return true;
    case '(':
    case ')':
        // Character set selection: `( 0` selects the line-drawing set, which
        // the parser tracks via the following byte.
        return true;
    default:
        return false;
    }
}

void Emulator::handleOsc(const QString& payload)
{
    // "0;title" / "2;title" set the window title; "8;params;uri" is a
    // hyperlink, which the renderer shows on hover. Everything else is
    // ignored rather than printed.
    const int semicolon = int(payload.indexOf(QLatin1Char(';')));
    if (semicolon > 0) {
        const QString command = payload.left(semicolon);
        const QString rest = payload.mid(semicolon + 1);
        if (command == QLatin1String("0") || command == QLatin1String("2")) {
            if (!rest.isEmpty()) {
                title_ = rest;
                update_.title = rest;
            }
            return;
        }
    }
}

void Emulator::handleControl(QChar ch)
{
    switch (ch.unicode()) {
    case 0x07:  // BEL
        return;
    case 0x08:  // BS
        if (cursorColumn_ > 0) {
            --cursorColumn_;
        }
        pendingWrap_ = false;
        return;
    case 0x09: {  // TAB
        const int next = ((cursorColumn_ / kTabWidth) + 1) * kTabWidth;
        cursorColumn_ = qMin(columns_ - 1, next);
        return;
    }
    case 0x0A:  // LF
    case 0x0B:  // VT
    case 0x0C:  // FF
        newline();
        return;
    case 0x0D:  // CR
        carriageReturn();
        return;
    case 0x0E:  // SO — shift out (G1)
    case 0x0F:  // SI — shift in (G0)
        return;
    default:
        return;
    }
}

EmulatorUpdate Emulator::feed(const QByteArray& bytes)
{
    update_ = EmulatorUpdate{};
    pending_.append(bytes);

    int i = 0;
    while (i < pending_.size()) {
        const char raw = pending_.at(i);

        // A string sequence swallows everything up to its terminator.
        if (stringState_ != StringState::None) {
            const QChar ch = QLatin1Char(raw);
            if (stringState_ == StringState::Osc
                && (ch == QChar(0x07) || ch == QChar(0x9C))) {
                handleOsc(stringBuffer_);
                stringState_ = StringState::None;
                stringBuffer_.clear();
                ++i;
                continue;
            }
            const bool isEsc = (raw == 0x1B);
            if (isEsc) {
                // `ESC \` is the ST terminator.
                if (i + 1 < pending_.size() && pending_.at(i + 1) == '\\') {
                    if (stringState_ == StringState::Osc) {
                        handleOsc(stringBuffer_);
                    }
                    stringState_ = StringState::None;
                    stringBuffer_.clear();
                    i += 2;
                    continue;
                }
                // An ESC that is not ST inside a string: abandon the string
                // and let the new sequence be parsed. Some tools forget the
                // terminator, and swallowing the rest of the session would
                // be worse than losing the title.
                stringState_ = StringState::None;
                stringBuffer_.clear();
                continue;
            }
            if (stringState_ == StringState::Osc) {
                stringBuffer_.append(ch);
            }
            ++i;
            continue;
        }

        if (raw == 0x1B) {
            // Escape sequence: [ = CSI, ] = OSC, ( ) = charset, else two bytes.
            if (i + 1 >= pending_.size()) {
                break;  // incomplete; wait for the next chunk
            }
            const char kind = pending_.at(i + 1);
            if (kind == '[') {
                // CSI: parameters then a final byte in 0x40..0x7E.
                int end = i + 2;
                while (end < pending_.size()
                       && (pending_.at(end) < 0x40 || pending_.at(end) > 0x7E)) {
                    ++end;
                }
                if (end >= pending_.size()) {
                    break;  // incomplete
                }
                const QString parameters = QString::fromLatin1(
                    pending_.mid(i + 2, end - i - 2));
                const QChar final = QLatin1Char(pending_.at(end));
                if (!handleCsi(parameters, final)) {
                    // An unhandled CSI must still consume its bytes: printing
                    // "[1;32m" is the classic symptom of getting this wrong.
                }
                i = end + 1;
                continue;
            }
            if (kind == ']') {
                stringState_ = StringState::Osc;
                stringBuffer_.clear();
                i += 2;
                continue;
            }
            if (kind == 'P') {  // DCS: swallow
                stringState_ = StringState::Dcs;
                stringBuffer_.clear();
                i += 2;
                continue;
            }
            if (kind == '(' || kind == ')') {
                if (i + 2 >= pending_.size()) {
                    break;
                }
                lineDrawing_ = (kind == '(' && pending_.at(i + 2) == '0');
                i += 3;
                continue;
            }
            if (!handleEsc(QLatin1Char(kind))) {
                // Unknown ESC X: consume both bytes.
            }
            i += 2;
            continue;
        }

        // Control characters.
        if (raw >= 0 && raw < 0x20) {
            handleControl(QLatin1Char(raw));
            ++i;
            continue;
        }
        if (raw == 0x7F) {  // DEL — ignored
            ++i;
            continue;
        }

        // UTF-8: decode one code point, waiting for the rest if incomplete.
        const auto lead = static_cast<unsigned char>(raw);
        int length = 1;
        if (lead < 0x80) {
            length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        } else {
            // Not a valid lead byte: skip it rather than print mojibake.
            ++i;
            continue;
        }
        if (i + length > pending_.size()) {
            break;  // incomplete character, wait
        }
        const QByteArray piece = pending_.mid(i, length);
        const QString decoded = QString::fromUtf8(piece);
        if (decoded.isEmpty()) {
            ++i;
            continue;
        }
        // Grapheme clusters: a combining mark that follows belongs to the
        // glyph already placed, which putGlyph handles with width 0.
        QString glyph = decoded;
        if (lineDrawing_ && glyph.size() == 1) {
            glyph = QString(decSpecial(glyph.at(0)));
        }
        putGlyph(glyph, glyphWidth(glyph));
        i += length;
    }

    pending_.remove(0, i);
    return update_;
}

EmulatorUpdate Emulator::feedText(const QString& text)
{
    return feed(text.toUtf8());
}

QString Emulator::rowText(int row, bool trimTrailing) const
{
    const QList<Cell>& line = lineAt(row);
    QString text;
    text.reserve(line.size());
    for (const Cell& cell : line) {
        text.append(cell.text);
    }
    if (trimTrailing) {
        int end = text.size();
        while (end > 0 && text.at(end - 1) == QLatin1Char(' ')) {
            --end;
        }
        text.truncate(end);
    }
    return text;
}

QStringList Emulator::screenLines() const
{
    QStringList lines;
    lines.reserve(rows_);
    for (int row = 0; row < rows_; ++row) {
        lines.append(rowText(row, true));
    }
    return lines;
}

QList<QList<StyledRun>> Emulator::screenRuns() const
{
    QList<QList<StyledRun>> rows;
    rows.reserve(rows_);
    for (int row = 0; row < rows_; ++row) {
        QList<StyledRun> runs;
        StyledRun current;
        for (const Cell& cell : lineAt(row)) {
            if (!runs.isEmpty() && current.attributes == cell.attributes) {
                runs.last().text += cell.text;
            } else {
                if (!runs.isEmpty()) {
                    current = StyledRun{};
                }
                current.attributes = cell.attributes;
                current.text = cell.text;
                runs.append(current);
            }
        }
        // Trailing blanks in the default attributes are trimmed per run so a
        // repaint does not underline the rest of the line.
        while (!runs.isEmpty() && runs.last().attributes == Attributes{}
               && runs.last().text.trimmed().isEmpty()) {
            runs.removeLast();
        }
        rows.append(runs);
    }
    return rows;
}

QStringList Emulator::scrollbackLines() const
{
    QStringList lines;
    for (const QStringList& row : scrollback_) {
        QString text = row.join(QString());
        int end = text.size();
        while (end > 0 && text.at(end - 1) == QLatin1Char(' ')) {
            --end;
        }
        lines.append(text.left(end));
    }
    lines.append(screenLines());
    return lines;
}

int Emulator::scrollbackCount() const
{
    return int(scrollback_.size());
}

void Emulator::setScrollOffset(int offset)
{
    scrollOffset_ = qBound(0, offset, int(scrollback_.size()));
}

void Emulator::clearScrollback()
{
    scrollback_.clear();
    scrollOffset_ = 0;
}

QString Emulator::textBetween(int startRow, int startColumn, int endRow,
                              int endColumn) const
{
    if (startRow > endRow || (startRow == endRow && startColumn > endColumn)) {
        std::swap(startRow, endRow);
        std::swap(startColumn, endColumn);
    }
    QStringList pieces;
    for (int row = startRow; row <= endRow && row < rows_; ++row) {
        if (row < 0) {
            continue;
        }
        QString piece;
        if (row != startRow && row != endRow) {
            // A middle row of a multi-line selection is copied whole — with
            // its trailing spaces trimmed, because a fixed-width terminal
            // pads every line and pasting the padding is never what anyone
            // wants.
            piece = rowText(row, true);
        } else {
            const QList<Cell>& line = lineAt(row);
            const int from = row == startRow ? qMax(0, startColumn) : 0;
            const int to = row == endRow ? qMin(int(line.size()) - 1, endColumn)
                                         : int(line.size()) - 1;
            for (int col = from; col <= to && col < line.size(); ++col) {
                piece.append(line.at(col).text);
            }
            if (row == endRow) {
                // The last row ends where the selection does. Trailing pads
                // are dropped, but leading spaces are content — pasting an
                // indented block without its indentation is not a copy.
                int end = piece.size();
                while (end > 0 && piece.at(end - 1) == QLatin1Char(' ')) {
                    --end;
                }
                piece.truncate(end);
            }
        }
        pieces.append(piece);
    }
    return pieces.join(QLatin1Char('\n'));
}

QString Emulator::currentLine() const
{
    return rowText(cursorRow_, true);
}

quint32 Emulator::paletteColor(int index)
{
    // The sixteen ANSI colours, in the "normal then bright" order SGR 30-37
    // and 90-97 index into. These are the xterm defaults, which is what a
    // tool that emits "31" for red is asking for.
    static const quint32 s_ansi[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
        0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
    };
    if (index < 0) {
        return 0;
    }
    if (index < 16) {
        return s_ansi[index];
    }
    if (index < 232) {
        // 6x6x6 cube: component n maps to 0, 95, 135, 175, 215, 255 — the
        // uneven ramp is not a bug, it is what every implementation ships.
        static const int s_steps[6] = {0, 95, 135, 175, 215, 255};
        const int n = index - 16;
        const int r = s_steps[(n / 36) % 6];
        const int g = s_steps[(n / 6) % 6];
        const int b = s_steps[n % 6];
        return (quint32(r) << 16) | (quint32(g) << 8) | quint32(b);
    }
    if (index < 256) {
        const int level = 8 + (index - 232) * 10;
        return (quint32(level) << 16) | (quint32(level) << 8) | quint32(level);
    }
    return 0;
}

}  // namespace LightCpp::Workbench::Terminal
