// CodeEditor — QPlainTextEdit with line-number gutter and syntax highlight.

#include "LightCpp/App/CodeEditor.hpp"
#include "LightCpp/App/CompletionPopup.hpp"
#include "LightCpp/App/CppSyntaxHighlighter.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTextBlock>
#include <QResizeEvent>
#include <QTimer>
#include <QToolTip>
#include <QAbstractTextDocumentLayout>

#include <utility>
#include <vector>

namespace {

/// The gutter is one column wider than the line numbers need, holding the
/// fold markers to the left of the diagnostic dots.
constexpr int kFoldColumnWidth = 12;
/// Side of the square fold marker drawn in that column.
constexpr int kFoldMarkerSize = 9;

}  // namespace

// ---------------------------------------------------------------------------
// LineNumberArea — a thin widget painted by CodeEditor.
// ---------------------------------------------------------------------------
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor* editor)
        : QWidget(editor), editor_(editor) {}

    [[nodiscard]] QSize sizeHint() const override {
        return {editor_->lineNumberAreaWidth(), 0};
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        editor_->lineNumberAreaPaintEvent(event);
    }

    // The gutter is a plain widget, so folding has to be driven from here:
    // clicks on the fold markers never reach the editor's own handlers.
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            editor_->handleFoldMarkerClick(event->position().toPoint());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

private:
    CodeEditor* editor_;
};

// ---------------------------------------------------------------------------
// CodeEditor
// ---------------------------------------------------------------------------
CodeEditor::CodeEditor(QWidget* parent)
    : QPlainTextEdit(parent)
    , lineNumberArea_(new LineNumberArea(this))
    , highlighter_(nullptr)
    , popup_(new LightCpp::App::CompletionPopup(this))
{
    // Monospace font for a code-editor feel.
    QFont font("Cascadia Code", 10);
    if (!font.exactMatch()) {
        font = QFont("Consolas", 10);
        if (!font.exactMatch()) {
            font = QFont("Courier New", 10);
        }
    }
    font.setStyleHint(QFont::Monospace);
    setFont(font);

    // A tab is exactly four monospace cells. Qt's own default (80px) works
    // out to about 10.5 cells at this size, which is not what C++ code is
    // normally indented with — and it makes a visual column stop line up with
    // nothing. Measured from a space so it stays correct if the font changes.
    setTabStopDistance(QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')) * 4.0);

    // Attach the C++ highlighter to the underlying document.
    highlighter_ = new CppSyntaxHighlighter(document());

    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged,
            this, &CodeEditor::highlightCurrentLine);
    // Folding is derived from the text, so it is recomputed whenever the text
    // changes. The analysis is a single pass over the document, and the fold
    // state itself survives re-parsing (see FoldAnchor).
    connect(this, &QPlainTextEdit::textChanged,
            this, &CodeEditor::rebuildFoldRegions);

    // Force the line-number gutter to be visible and opaque: creating it
    // as a child of an un-shown widget can leave it hidden by default.
    lineNumberArea_->setAutoFillBackground(true);
    lineNumberArea_->setAttribute(Qt::WA_ShowWithoutActivating, true);
    lineNumberArea_->setMinimumWidth(lineNumberAreaWidth());
    lineNumberArea_->show();

    // Wire the popup: accept an item when the user picks one.
    connect(popup_, &LightCpp::App::CompletionPopup::itemAccepted,
            this, [this](const LightCpp::App::LspCompletionItem& it) {
        applyCompletionItem(it);
    });
    connect(popup_, &LightCpp::App::CompletionPopup::dismissed,
            this, [this] {
        // The popup hides itself; nothing else to do here.
    });

    // "As-you-type" completion: after a short idle following ANY text edit
    // (identifier keystroke or backspace), re-request completions from
    // clangd — regardless of whether the popup is currently visible. This
    // keeps the item list fresh for the actual prefix and makes the popup
    // appear/reappear predictably on every keystroke pause. clangd returns
    // nothing in comments/strings, so the popup stays hidden there.
    completionIdleTimer_ = new QTimer(this);
    completionIdleTimer_->setSingleShot(true);
    completionIdleTimer_->setInterval(120);
    connect(completionIdleTimer_, &QTimer::timeout,
            this, [this] { requestCompletionAtCursor(QString()); });

    updateLineNumberAreaWidth(0);
    highlightCurrentLine();
    rebuildFoldRegions();
}

CodeEditor::~CodeEditor() = default;

void CodeEditor::loadUtf8(const QByteArray& utf8)
{
    setPlainText(QString::fromUtf8(utf8));
    document()->setModified(false);
}

QByteArray CodeEditor::toUtf8() const
{
    return toPlainText().toUtf8();
}

void CodeEditor::setModified(bool modified)
{
    document()->setModified(modified);
}

bool CodeEditor::isModified() const
{
    return document()->isModified();
}

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    // One spare digit's worth of room keeps the line numbers clear of the fold
    // markers and diagnostic dots drawn on the left.
    int space = 3 + fontMetrics().horizontalAdvance(u'9') * (digits + 1);
    return space + kFoldColumnWidth;
}

void CodeEditor::updateLineNumberAreaWidth(int)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect& rect, int dy)
{
    if (dy > 0) {
        lineNumberArea_->scroll(0, dy);
    } else {
        lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
    }
    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth(0);
    }
}

void CodeEditor::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    lineNumberArea_->setGeometry(
        {cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()});
}

void CodeEditor::highlightCurrentLine()
{
    updateExtraSelections();
}

LightCpp::App::LspDiagnostic
CodeEditor::normalizedDiagnostic(const LightCpp::App::LspDiagnostic& d) const
{
    auto copy = d;
    // clangd reports some errors (e.g. "#include" without a filename) as a
    // zero-width range at column 0 of the EMPTY line after the mistake.
    // Visually that belongs to the end of the previous line — move the
    // anchor there so the gutter dot and squiggle show on the right line.
    if (copy.startLine == copy.endLine
        && copy.startColumn == copy.endColumn
        && copy.startColumn == 0 && copy.startLine > 0) {
        const QTextBlock prev = document()->findBlockByNumber(copy.startLine - 1);
        const QTextBlock cur = document()->findBlockByNumber(copy.startLine);
        if (prev.isValid() && cur.isValid() && cur.length() <= 1) {
            copy.startLine = copy.endLine = copy.startLine - 1;
            copy.startColumn = copy.endColumn = qMax(prev.length() - 1, 0);
        }
    }
    return copy;
}

void CodeEditor::updateExtraSelections()
{
    QList<QTextEdit::ExtraSelection> selections;

    // Current-line highlight (subtle, palette-driven).
    if (!isReadOnly()) {
        QTextEdit::ExtraSelection sel;
        QColor hl = palette().color(QPalette::AlternateBase);
        if (!hl.isValid()) {
            hl = palette().color(QPalette::Base).lighter(105);
        }
        sel.format.setBackground(hl);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = textCursor();
        sel.cursor.clearSelection();
        selections.append(sel);
    }

    // Diagnostic underlines: red for errors, amber for warnings, muted for
    // info/hints. The message becomes the format tooltip.
    for (const LightCpp::App::LspDiagnostic& diag : std::as_const(diagnostics_)) {
        const LightCpp::App::LspDiagnostic d = normalizedDiagnostic(diag);
        const QTextBlock startBlock = document()->findBlockByNumber(d.startLine);
        if (!startBlock.isValid()) {
            continue;
        }
        const QTextBlock endBlock = document()->findBlockByNumber(d.endLine);
        int start = startBlock.position()
                    + qMin<qsizetype>(d.startColumn, qMax<qsizetype>(startBlock.length() - 1, 0));
        int end = (endBlock.isValid() ? endBlock.position() : startBlock.position())
                  + qMin<qsizetype>(d.endColumn, qMax<qsizetype>((endBlock.isValid() ? endBlock.length() : 1) - 1, 0));
        if (end <= start) {
            // Zero-width range: select one visible character so the wavy
            // underline actually renders. At/past the end of a line,
            // underline the LAST character (matches how other editors
            // squiggle the token tail for "expected X" preprocessor
            // errors); otherwise underline the character at the caret.
            const int blockEnd = startBlock.position()
                                 + qMax<qsizetype>(startBlock.length() - 1, 0);
            if (start >= blockEnd && start > startBlock.position()) {
                start = blockEnd - 1;
                end = blockEnd;
            } else {
                end = start + 1;
            }
            if (start >= document()->characterCount()) {
                start = qMax(0, document()->characterCount() - 2);
                end = start + 1;
            }
            // An empty block has no visible character to underline — the
            // gutter dot still marks the line.
            if (start < startBlock.position()
                || start >= startBlock.position() + startBlock.length() - 1) {
                continue;
            }
        }

        QTextCursor cursor(document());
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);

        QTextEdit::ExtraSelection sel;
        sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        QColor color;
        switch (d.severity) {
        case 1:  color = QColor(Qt::red); break;                       // Error
        case 2:  color = QColor(0xE0, 0xA0, 0x00); break;               // Warning
        default: color = palette().color(QPalette::Text); break;        // Info/Hint
        }
        sel.format.setUnderlineColor(color);
        sel.format.setToolTip(d.message);
        sel.cursor = cursor;
        selections.append(sel);
    }

    setExtraSelections(selections);
    lineNumberArea_->update();
}

void CodeEditor::setDiagnostics(const QList<LightCpp::App::LspDiagnostic>& diagnostics)
{
    diagnostics_ = diagnostics;
    updateExtraSelections();
    // Publish the normalised positions: the problem list must point at the
    // same line the squiggle landed on.
    emit diagnosticsChanged(lintPath(), normalizedDiagnostics());
}

QList<LightCpp::App::LspDiagnostic> CodeEditor::normalizedDiagnostics() const
{
    QList<LightCpp::App::LspDiagnostic> out;
    out.reserve(diagnostics_.size());
    for (const LightCpp::App::LspDiagnostic& d : diagnostics_) {
        out.append(normalizedDiagnostic(d));
    }
    return out;
}

void CodeEditor::gotoLocation(int line, int column)
{
    // A jump into collapsed code would otherwise put the caret somewhere it
    // cannot be seen — expand whatever hides it first.
    unfoldLine(line);
    const QTextBlock block = document()->findBlockByNumber(qMax(line, 0));
    if (!block.isValid()) {
        return;
    }
    const int maxColumn = int(qMax<qsizetype>(block.length() - 1, 0));
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + qBound(0, column, maxColumn));
    setTextCursor(cursor);
    centerCursor();
    setFocus(Qt::OtherFocusReason);
}

void CodeEditor::applyFormattedText(const QString& text)
{
    if (text == toPlainText()) {
        return;
    }

    // Where the caret was, remembered as line + column rather than as an
    // offset: a formatter moves text around, and the line is what stops the
    // view from jumping somewhere unrelated.
    const QTextCursor before = textCursor();
    const int line = before.blockNumber();
    const int column = before.positionInBlock();

    {
        // One edit block, so a single undo puts the whole file back.
        QTextCursor cursor(document());
        cursor.beginEditBlock();
        cursor.select(QTextCursor::Document);
        cursor.insertText(text);
        cursor.endEditBlock();
    }

    if (const QTextBlock block = document()->findBlockByNumber(line); block.isValid()) {
        const int clamped = qBound(0, column, qMax(int(block.length()) - 1, 0));
        QTextCursor restored(block);
        restored.setPosition(block.position() + clamped);
        setTextCursor(restored);
    }
    // The replacement marks the document modified on its own, which is right:
    // the formatted text is not on disk until the user saves.
}

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

void CodeEditor::presentPopup(const QList<LightCpp::App::LspCompletionItem>& items)
{
    if (items.isEmpty()) {
        dismissPopup();
        return;
    }
    // Position the popup just below the cursor line.
    const QRect cr = cursorRect();
    QPoint pos = viewport()->mapToGlobal(cr.bottomLeft());
    popup_->present(items, pos);
}

void CodeEditor::dismissPopup()
{
    popup_->dismiss();
}

// ---------------------------------------------------------------------------
// Bracket auto-pairing
// ---------------------------------------------------------------------------
QChar CodeEditor::closingBracketFor(QChar opener)
{
    switch (opener.unicode()) {
    case '(':  return QLatin1Char(')');
    case '[':  return QLatin1Char(']');
    case '{':  return QLatin1Char('}');
    case '"':  return QLatin1Char('"');
    case '\'': return QLatin1Char('\'');
    default:   return QChar();
    }
}

bool CodeEditor::isClosingBracket(QChar ch)
{
    return ch == QLatin1Char(')') || ch == QLatin1Char(']')
           || ch == QLatin1Char('}');
}

bool CodeEditor::isQuote(QChar ch)
{
    return ch == QLatin1Char('"') || ch == QLatin1Char('\'');
}

QChar CodeEditor::characterBefore(const QTextCursor& cursor)
{
    if (cursor.document() == nullptr || cursor.position() <= 0) {
        return QChar();
    }
    return cursor.document()->characterAt(cursor.position() - 1);
}

QChar CodeEditor::characterAfter(const QTextCursor& cursor)
{
    if (cursor.document() == nullptr
        || cursor.position() >= cursor.document()->characterCount() - 1) {
        return QChar();
    }
    return cursor.document()->characterAt(cursor.position());
}

bool CodeEditor::autoCloseBracket(QChar typed)
{
    if (isReadOnly()) {
        return false;
    }
    const QChar closer = closingBracketFor(typed);
    const bool isOpener = !closer.isNull();
    const bool isCloser = isClosingBracket(typed);
    const bool quote = isQuote(typed);
    if (!isOpener && !isCloser) {
        return false;  // not a bracket or quote — not our business
    }

    QTextCursor cursor = textCursor();

    // A quote only pairs at the start of a token. After a letter or digit it
    // is C++ punctuation rather than a delimiter — the digit separator in
    // `1'000`, or an apostrophe inside a comment — and pairing it would just
    // insert a character the user has to delete again.
    // The rule only applies to typing into the middle of existing text: a
    // selection is always wrapped, whatever precedes it.
    if (quote && !cursor.hasSelection()) {
        const QChar prev = characterBefore(cursor);
        const bool prevIsWord = prev.isLetterOrNumber()
                                || prev == QLatin1Char('_');
        // …unless the closing quote of an existing pair is being typed,
        // where stepping over it is still the right thing to do.
        if (prevIsWord && characterAfter(cursor) != typed) {
            return false;
        }
    }

    // --- a selection gets wrapped: (foo) / "foo" --------------------------
    if (cursor.hasSelection()) {
        if (isCloser) {
            return false;  // typing `)` over a selection replaces it normally
        }
        const int start = cursor.selectionStart();
        const int end = cursor.selectionEnd();
        // Insert around the selection instead of re-inserting the selected
        // text: QTextCursor::selectedText() turns paragraph breaks into
        // U+2029, which would corrupt multi-line selections.
        cursor.beginEditBlock();
        cursor.setPosition(start);
        cursor.insertText(QString(typed));
        cursor.setPosition(end + 1);
        cursor.insertText(QString(closer));
        cursor.setPosition(start + 1);
        cursor.setPosition(end + 1, QTextCursor::KeepAnchor);
        cursor.endEditBlock();
        setTextCursor(cursor);
        return true;
    }

    // --- step over a closing half that is already there -------------------
    // This is what makes typing the auto-inserted `)` end up after it
    // instead of producing `())`.
    if (characterAfter(cursor) == typed && (isCloser || quote)) {
        cursor.movePosition(QTextCursor::NextCharacter);
        setTextCursor(cursor);
        return true;
    }

    if (isCloser) {
        return false;  // a lone `)` types normally
    }

    // --- insert both halves, caret in between ----------------------------
    cursor.beginEditBlock();
    cursor.insertText(QString(typed));
    cursor.insertText(QString(closer));
    cursor.movePosition(QTextCursor::PreviousCharacter);
    cursor.endEditBlock();
    setTextCursor(cursor);
    return true;
}

bool CodeEditor::backspaceEmptyPair()
{
    if (isReadOnly()) {
        return false;
    }
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        return false;
    }
    const QChar prev = characterBefore(cursor);
    const QChar next = characterAfter(cursor);
    if (prev.isNull() || next.isNull()) {
        return false;
    }
    // Only a genuinely empty pair is removed: `(|)` goes away as a unit,
    // while `{` … `}` spanning real content is left alone.
    if (closingBracketFor(prev) != next) {
        return false;
    }
    // Select from the opener through the closer. Both directions are needed:
    // moving backwards alone would stop at the start of the document and
    // leave a stray closing bracket behind.
    const int pos = cursor.position();
    cursor.beginEditBlock();
    cursor.setPosition(pos - 1);
    cursor.setPosition(pos + 1, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.endEditBlock();
    setTextCursor(cursor);
    return true;
}

bool CodeEditor::autoIndentOnReturn()
{
    if (isReadOnly()) {
        return false;
    }
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        return false;  // Enter over a selection replaces it — default rules
    }
    const int col = cursor.positionInBlock();
    const QString line = cursor.block().text();
    // The indentation is whatever whitespace precedes the caret; it is
    // carried over verbatim so mixed tab/space files keep their shape.
    int indentEnd = 0;
    while (indentEnd < col && (line.at(indentEnd) == QLatin1Char('\t')
                               || line.at(indentEnd) == QLatin1Char(' '))) {
        ++indentEnd;
    }
    const QString indent = line.left(indentEnd);
    // Right-trim the text between the indentation and the caret: the block
    // opens when its last non-blank character is `{`.
    int last = col - 1;
    while (last >= indentEnd && (line.at(last) == QLatin1Char(' ')
                                 || line.at(last) == QLatin1Char('\t'))) {
        --last;
    }
    const bool opensBlock =
        last >= indentEnd && line.at(last) == QLatin1Char('{');
    // The auto-pairing closer sits right after the caret (`{|}`): Enter
    // expands the pair into the three-line shape every C++ editor uses.
    const bool expandsPair =
        opensBlock && characterAfter(cursor) == QLatin1Char('}');

    cursor.beginEditBlock();
    if (expandsPair) {
        cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();  // take the closer out of the line…
        // …and put it back on its own line, at the `{` line's indentation.
        cursor.insertText(QString(QLatin1Char('\n')) + indent
                          + QLatin1Char('\t'));
        cursor.insertText(QString(QLatin1Char('\n')) + indent
                          + QLatin1Char('}'));
        // Park the caret at the end of the middle (body) line: back over
        // the closer, its newline, and the middle line's indentation.
        cursor.setPosition(cursor.position() - indent.size() - 2);
    } else {
        cursor.insertText(QString(QLatin1Char('\n')) + indent
                          + (opensBlock ? QString(QLatin1Char('\t')) : QString()));
    }
    cursor.endEditBlock();
    setTextCursor(cursor);
    return true;
}

bool CodeEditor::autoDedentOnCloseBrace(QChar typed)
{
    if (typed != QLatin1Char('}') || isReadOnly()) {
        return false;
    }
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        return false;
    }
    const int col = cursor.positionInBlock();
    const QString line = cursor.block().text();
    // Only a `}` typed at the start of a line (leading whitespace only)
    // dedents; mid-line it is an expression, not a block closer.
    for (int i = 0; i < col; ++i) {
        const QChar ch = line.at(i);
        if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t')) {
            return false;
        }
    }
    // Peel one indentation level off the line: a leading tab, or up to four
    // leading spaces (the tab width this editor displays).
    int remove = 0;
    if (col > 0 && line.at(0) == QLatin1Char('\t')) {
        remove = 1;
    } else {
        while (remove < col && remove < 4
               && line.at(remove) == QLatin1Char(' ')) {
            ++remove;
        }
    }
    if (remove == 0) {
        return false;  // nothing to dedent — let the key insert normally
    }

    QTextDocument* doc = document();
    const int pos = cursor.position();
    // When the auto-inserted closer sits on the next line (`{|}` + Enter
    // put it there), this keystroke means "close the block I just wrote",
    // not "insert another `}`": drop the empty indented line under the
    // caret and step onto the closer that is already there.
    int p = pos;
    // Block separators are U+2029 inside a QTextDocument — '\n' only
    // exists in toPlainText().
    const QChar paragraphSeparator = QChar(QChar::ParagraphSeparator);
    if (doc->characterAt(p) == paragraphSeparator) {
        ++p;
        while (p < doc->characterCount() - 1
               && (doc->characterAt(p) == QLatin1Char(' ')
                   || doc->characterAt(p) == QLatin1Char('\t'))) {
            ++p;
        }
        if (doc->characterAt(p) == QLatin1Char('}')) {
            const int blockStart = cursor.block().position();
            cursor.beginEditBlock();
            cursor.setPosition(blockStart);
            cursor.setPosition(p, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();  // the `}` rides up to this line
            cursor.setPosition(blockStart + 1);
            cursor.endEditBlock();
            setTextCursor(cursor);
            return true;
        }
    }

    // The closer belongs after the line's *full* leading indentation, so
    // insert it there first and only then strip the peeled level in front
    // of it — inserting at the caret (or the block start) would land it
    // inside the leftover whitespace.
    int indentEnd = 0;
    while (indentEnd < line.size()
           && (line.at(indentEnd) == QLatin1Char('\t')
               || line.at(indentEnd) == QLatin1Char(' '))) {
        ++indentEnd;
    }
    cursor.beginEditBlock();
    const int blockStart = cursor.block().position();
    cursor.setPosition(blockStart + indentEnd);
    cursor.insertText(QString(typed));
    QTextCursor trim = cursor;
    trim.setPosition(blockStart);
    trim.setPosition(blockStart + remove, QTextCursor::KeepAnchor);
    trim.removeSelectedText();  // ahead of the caret: it shifts back itself
    cursor.endEditBlock();
    setTextCursor(cursor);
    return true;
}

void CodeEditor::keyPressEvent(QKeyEvent* event)
{
    const int key = event->key();
    const QString text = event->text();

    // --- go to definition --------------------------------------------------
    // F12 is the keyboard twin of Ctrl+Click. Handled before everything else
    // so it works regardless of what is under the caret.
    if (key == Qt::Key_F12) {
        const QTextCursor c = textCursor();
        emit definitionRequested(c.blockNumber(), c.positionInBlock());
        return;
    }

    // --- bracket auto-pairing --------------------------------------------
    // Runs before the completion logic so an auto-inserted pair behaves like
    // ordinary typing ("()" with the caret in between, one undo step).
    const bool plainTextKey = !text.isEmpty()
                              && !(event->modifiers()
                                   & (Qt::ControlModifier | Qt::AltModifier
                                      | Qt::MetaModifier));
    if (plainTextKey && autoCloseBracket(text.at(0))) {
        completionIdleTimer_->stop();
        if (text.at(0) == QLatin1Char('"')) {
            // `"` doubles as clangd's include-completion trigger, so ask
            // from inside the fresh pair instead of falling through (which
            // would insert a second quote). Outside an #include clangd
            // answers with an empty list, which is harmless.
            requestCompletionAtCursor(QStringLiteral("\""));
        } else if (popup_ && popup_->isVisible()) {
            dismissPopup();
        }
        return;
    }

    // Backspace between the two halves of an empty pair removes both.
    if (key == Qt::Key_Backspace && !event->isAutoRepeat()
        && backspaceEmptyPair()) {
        completionIdleTimer_->stop();
        if (popup_ && popup_->isVisible()) {
            dismissPopup();
        }
        return;
    }

    // A `}` typed at the start of an indented line dedents first, so it
    // lines up with the `{` that opened the block.
    if (plainTextKey && autoDedentOnCloseBrace(text.at(0))) {
        completionIdleTimer_->stop();
        if (popup_ && popup_->isVisible()) {
            dismissPopup();
        }
        return;
    }

    // Characters that ask clangd for a fresh completion list. `.` `:` `>`
    // cover member/namespace/pointer access; `<` and `"` trigger clangd's
    // include completion after `#include<` / `#include"`. Outside the
    // include context clangd simply returns an empty list, so no harm.
    const auto isTriggerText = [&text] {
        if (text.isEmpty()) return false;
        const QChar ch = text.at(0);
        return ch == QLatin1Char('.') || ch == QLatin1Char(':')
            || ch == QLatin1Char('>') || ch == QLatin1Char('<')
            || ch == QLatin1Char('"');
    };
    const auto isIdentText = [&text] {
        if (text.isEmpty()) return false;
        const QChar ch = text.at(0);
        return ch.isLetterOrNumber() || ch == QLatin1Char('_');
    };

    // Ctrl+Space — manual completion request at the current cursor.
    if ((event->modifiers() & Qt::ControlModifier)
        && key == Qt::Key_Space) {
        completionIdleTimer_->stop();
        requestCompletionAtCursor(QString());
        return;
    }

    // While the popup is open, handle navigation specially.
    const bool popupVisible = (popup_ && popup_->isVisible());
    if (popupVisible) {
        if (key == Qt::Key_Return || key == Qt::Key_Enter
            || key == Qt::Key_Tab) {
            // Apply the selected item and swallow the key.
            completionIdleTimer_->stop();
            if (const auto* it = popup_->selectedItem()) {
                applyCompletionItem(*it);
            }
            dismissPopup();
            return;
        }
        if (key == Qt::Key_Escape) {
            completionIdleTimer_->stop();
            dismissPopup();
            return;
        }
        if (key == Qt::Key_Up || key == Qt::Key_Down
            || key == Qt::Key_PageUp || key == Qt::Key_PageDown) {
            QApplication::sendEvent(popup_, event);
            return;
        }
        if (key == Qt::Key_Backspace) {
            QPlainTextEdit::keyPressEvent(event);  // delete a char
            refreshPopupFilter();                  // instant feedback
            completionIdleTimer_->start();         // re-request fresh items
            return;
        }
        if (!text.isEmpty()) {
            if (isTriggerText()) {
                completionIdleTimer_->stop();
                QPlainTextEdit::keyPressEvent(event);  // insert trigger char
                requestCompletionAtCursor(text);  // start a fresh popup
                return;
            }
            if (isIdentText()) {
                QPlainTextEdit::keyPressEvent(event);  // insert the char
                refreshPopupFilter();                  // instant feedback
                completionIdleTimer_->start();         // re-request fresh items
                return;
            }
            // Any other char (space, operator, ...): close popup and
            // let the editor handle the keystroke normally.
            completionIdleTimer_->stop();
            dismissPopup();
            // fall through to default handling
        }
    } else {
        // No popup yet.
        if (!text.isEmpty()) {
            if (isTriggerText()) {
                completionIdleTimer_->stop();
                QPlainTextEdit::keyPressEvent(event);  // insert trigger char
                requestCompletionAtCursor(text);
                return;
            }
            if (isIdentText()) {
                QPlainTextEdit::keyPressEvent(event);  // insert the char
                // Ask for completions once typing pauses briefly — this is
                // what makes completion appear while typing, not only on
                // trigger characters.
                completionIdleTimer_->start();
                return;
            }
            completionIdleTimer_->stop();
        }
    }

    // Auto-indent: a plain newline inherits the current line's indentation,
    // one level deeper when the line ends with `{` (and expands the
    // auto-inserted `{|}` pair into three lines). The completion popup
    // consumes Enter above, so this only runs for ordinary newlines.
    if ((key == Qt::Key_Return || key == Qt::Key_Enter)
        && autoIndentOnReturn()) {
        completionIdleTimer_->stop();
        return;
    }

    QPlainTextEdit::keyPressEvent(event);
}

void CodeEditor::mousePressEvent(QMouseEvent* event)
{
    // Clicking inside the editor dismisses the popup and cancels any
    // pending as-you-type request (the timer would otherwise fire at the
    // new — possibly unrelated — cursor position).
    completionIdleTimer_->stop();
    if (popup_ && popup_->isVisible()) {
        dismissPopup();
    }

    // Ctrl+Click over an identifier means "go to definition". The document
    // position is resolved before the base class runs, because the base class
    // moves the caret — and with a selection in play that would change which
    // character sits under the pointer. The request itself is emitted after,
    // so the caret has already settled here when the answer jumps away.
    const bool ctrlClick = event->button() == Qt::LeftButton
                           && event->modifiers().testFlag(Qt::ControlModifier);
    int defLine = 0;
    int defColumn = 0;
    if (ctrlClick) {
        // cursorForPosition() takes viewport coordinates, which is the space
        // a mouse event carries (QMouseEvent::pos() is deprecated in Qt 6).
        const QTextCursor cursor = cursorForPosition(event->position().toPoint());
        defLine = cursor.blockNumber();
        defColumn = cursor.positionInBlock();
    }

    QPlainTextEdit::mousePressEvent(event);

    if (ctrlClick) {
        emit definitionRequested(defLine, defColumn);
    }
}

bool CodeEditor::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* helpEvent = static_cast<QHelpEvent*>(event);
        // ToolTip events arrive with viewport-relative positions, which is
        // exactly what cursorForPosition() expects.
        const QTextCursor cursor = cursorForPosition(helpEvent->pos());
        const int line = cursor.blockNumber();
        const int col = cursor.positionInBlock();

        // Find the most severe diagnostic covering this position.
        // LSP line/column are 0-based UTF-16 code units — the same space
        // QTextCursor uses, so the values compare directly. `found` guards
        // the "nothing here" case (severity alone cannot: the struct
        // defaults to 1 = Error).
        LightCpp::App::LspDiagnostic hovered;
        bool found = false;
        for (const auto& diag : std::as_const(diagnostics_)) {
            const LightCpp::App::LspDiagnostic d = normalizedDiagnostic(diag);
            if (line < d.startLine || line > d.endLine) {
                continue;
            }
            bool inRange = false;
            if (d.startLine == d.endLine) {
                if (d.endColumn > d.startColumn) {
                    inRange = (col >= d.startColumn && col < d.endColumn);
                } else {
                    // Zero-width range (clangd emits these for some
                    // diagnostics) — accept anywhere on the line.
                    inRange = true;
                }
            } else {
                // Multi-line range: bounds matter only on the edge lines.
                if (line == d.startLine && col < d.startColumn) continue;
                if (line == d.endLine && d.endColumn > d.startColumn
                    && col >= d.endColumn) continue;
                inRange = true;
            }
            if (!inRange) continue;
            if (!found || d.severity < hovered.severity) {
                hovered = d;  // 1=Error is most severe
                found = true;
            }
        }

        if (found) {
            QString severity;
            switch (hovered.severity) {
            case 1:  severity = QStringLiteral("Error");   break;
            case 2:  severity = QStringLiteral("Warning"); break;
            case 3:  severity = QStringLiteral("Info");    break;
            case 4:  severity = QStringLiteral("Hint");    break;
            default: severity = QStringLiteral("Message"); break;
            }
            QString text = QStringLiteral("[%1] %2")
                               .arg(severity, hovered.message);
            if (!hovered.code.isEmpty()) {
                text += QStringLiteral(" (%1)").arg(hovered.code);
            }
            QToolTip::showText(helpEvent->globalPos(), text, this);
            return true;
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    if (event->type() == QEvent::Leave) {
        QToolTip::hideText();
    }
    return QPlainTextEdit::event(event);
}

void CodeEditor::requestCompletionAtCursor(const QString& triggerChar)
{
    const QTextCursor c = textCursor();
    const int line = c.blockNumber();
    const int column = c.positionInBlock();
    emit completionRequested(line, column, triggerChar);
}

void CodeEditor::refreshPopupFilter()
{
    if (!popup_ || !popup_->isVisible()) {
        return;
    }
    // Find the identifier prefix immediately before the cursor.
    QTextCursor c = textCursor();
    QTextBlock block = c.block();
    const QString blockText = block.text();
    const int posInBlock = c.position() - block.position();
    int i = posInBlock - 1;
    while (i >= 0) {
        const QChar ch = blockText.at(i);
        if (ch.isLetterOrNumber() || ch == '_') {
            --i;
        } else {
            break;
        }
    }
    const QString prefix = blockText.mid(i + 1, posInBlock - (i + 1));
    if (prefix.isEmpty()) {
        // No prefix — keep the popup as-is.
        return;
    }
    popup_->filterByPrefix(prefix);
}

void CodeEditor::applyCompletionItem(const LightCpp::App::LspCompletionItem& item)
{
    QTextCursor c = textCursor();
    QTextBlock block = c.block();
    const QString blockText = block.text();
    const int blockPos = block.position();
    const int posInBlock = c.position() - blockPos;

    // If the cursor is right after a trigger char (".", ":", ">"), do not
    // treat the trigger as a partial identifier — leave it alone.
    int i = posInBlock - 1;
    if (i >= 0) {
        const QChar prevCh = blockText.at(i);
        if (prevCh == '.' || prevCh == ':' || prevCh == '>') {
            // Insert after the trigger; no prefix to replace.
            c.insertText(item.insertText);
            setTextCursor(c);
            return;
        }
    }
    // Otherwise: select the identifier chars before the cursor and replace.
    while (i >= 0) {
        const QChar ch = blockText.at(i);
        if (ch.isLetterOrNumber() || ch == '_') {
            --i;
        } else {
            break;
        }
    }
    const int wordStart = blockPos + i + 1;
    c.setPosition(wordStart);
    c.setPosition(blockPos + posInBlock, QTextCursor::KeepAnchor);
    c.insertText(item.insertText);
    setTextCursor(c);
}

// ---------------------------------------------------------------------------
// Code folding
//
// The analysis itself lives in LightCppCore (FoldModel) and knows nothing about
// widgets; everything here is about keeping a fold attached to its block while
// the text moves underneath it.
// ---------------------------------------------------------------------------
void CodeEditor::rebuildFoldRegions()
{
    // Splitting the document text once is simpler than translating block
    // positions, and the analysis wants to see the text as a whole anyway — a
    // block comment or a raw string can span blocks.
    const QStringList parts = toPlainText().split(u'\n');
    std::vector<std::string> lines;
    lines.reserve(std::size_t(parts.size()));
    for (const QString& part : parts) {
        lines.push_back(part.toStdString());
    }

    QList<LightCpp::Core::Fold::FoldRegion> next;
    for (const LightCpp::Core::Fold::FoldRegion& region
         : LightCpp::Core::Fold::computeFoldRegions(lines)) {
        next.append(region);
    }

    // Re-attach the folds that are currently collapsed. A fold is remembered
    // as an ordinal position, so an edit above it moves every line without
    // disturbing the fold. The header text is the safety net: if that slot now
    // opens something else, the fold it described is gone and is dropped
    // rather than snapping onto an unrelated block.
    QList<FoldAnchor> kept;
    for (const FoldAnchor& anchor : std::as_const(folded_)) {
        if (anchor.regionIndex < 0 || anchor.regionIndex >= next.size()) {
            continue;
        }
        const QTextBlock block = document()->findBlockByNumber(
            int(next.at(anchor.regionIndex).startLine));
        if (!block.isValid() || block.text() != anchor.headerText) {
            continue;
        }
        kept.append(anchor);
    }

    foldRegions_ = next;
    folded_ = kept;
    applyFoldState();
}

void CodeEditor::applyFoldState()
{
    QSet<int> hidden;
    for (const FoldAnchor& anchor : std::as_const(folded_)) {
        if (anchor.regionIndex < 0 || anchor.regionIndex >= foldRegions_.size()) {
            continue;
        }
        const LightCpp::Core::Fold::FoldRegion& region =
            foldRegions_.at(anchor.regionIndex);
        // The region's own line stays visible: it is the header that says what
        // is inside.
        for (std::size_t line = region.startLine + 1; line <= region.endLine; ++line) {
            hidden.insert(int(line));
        }
    }

    // This runs on every keystroke, so an unchanged answer must cost nothing.
    if (hidden == hiddenLines_) {
        return;
    }
    hiddenLines_ = hidden;

    QTextBlock block = document()->begin();
    while (block.isValid()) {
        const bool visible = !hidden.contains(block.blockNumber());
        if (block.isVisible() != visible) {
            block.setVisible(visible);
            // A block that occupies no lines takes no vertical space, and
            // markContentsDirty invalidates its cached layout so the change is
            // actually reflected on screen.
            block.setLineCount(visible ? 1 : 0);
            document()->markContentsDirty(block.position(), block.length());
        }
        block = block.next();
    }
    viewport()->update();
    lineNumberArea_->update();
}

bool CodeEditor::isLineHidden(int line) const
{
    return hiddenLines_.contains(line);
}

bool CodeEditor::isFolded(int regionIndex) const
{
    for (const FoldAnchor& anchor : std::as_const(folded_)) {
        if (anchor.regionIndex == regionIndex) {
            return true;
        }
    }
    return false;
}

void CodeEditor::setFolded(int regionIndex, bool folded)
{
    if (regionIndex < 0 || regionIndex >= foldRegions_.size()) {
        return;
    }
    if (isFolded(regionIndex) == folded) {
        return;
    }
    if (folded) {
        const QTextBlock header = document()->findBlockByNumber(
            int(foldRegions_.at(regionIndex).startLine));
        FoldAnchor anchor;
        anchor.regionIndex = regionIndex;
        anchor.headerText = header.isValid() ? header.text() : QString();
        folded_.append(anchor);
    } else {
        for (qsizetype i = 0; i < folded_.size(); ++i) {
            if (folded_.at(i).regionIndex == regionIndex) {
                folded_.removeAt(i);
                break;
            }
        }
    }
    applyFoldState();
    keepCaretOutsideFolds();
}

bool CodeEditor::toggleFoldAtLine(int line)
{
    // Innermost first: when several blocks open on one line, the tightest is
    // the one the click was aimed at.
    for (qsizetype i = foldRegions_.size() - 1; i >= 0; --i) {
        if (int(foldRegions_.at(i).startLine) == line) {
            setFolded(int(i), !isFolded(int(i)));
            return true;
        }
    }
    return false;
}

void CodeEditor::foldAtCaret()
{
    const std::size_t caret = std::size_t(qMax(textCursor().blockNumber(), 0));
    int best = -1;
    std::size_t bestStart = 0;
    std::size_t bestSpan = 0;
    for (qsizetype i = 0; i < foldRegions_.size(); ++i) {
        const LightCpp::Core::Fold::FoldRegion& region = foldRegions_.at(i);
        if (region.startLine > caret || region.endLine < caret) {
            continue;
        }
        const std::size_t span = region.endLine - region.startLine;
        if (best < 0 || region.startLine > bestStart
            || (region.startLine == bestStart && span < bestSpan)) {
            best = int(i);
            bestStart = region.startLine;
            bestSpan = span;
        }
    }
    if (best >= 0) {
        setFolded(best, true);
    }
}

void CodeEditor::unfoldAtCaret()
{
    const std::size_t caret = std::size_t(qMax(textCursor().blockNumber(), 0));
    int best = -1;
    std::size_t bestStart = 0;
    std::size_t bestSpan = 0;
    for (const FoldAnchor& anchor : std::as_const(folded_)) {
        if (anchor.regionIndex < 0 || anchor.regionIndex >= foldRegions_.size()) {
            continue;
        }
        const LightCpp::Core::Fold::FoldRegion& region =
            foldRegions_.at(anchor.regionIndex);
        if (region.startLine > caret || region.endLine < caret) {
            continue;
        }
        const std::size_t span = region.endLine - region.startLine;
        if (best < 0 || region.startLine > bestStart
            || (region.startLine == bestStart && span < bestSpan)) {
            best = anchor.regionIndex;
            bestStart = region.startLine;
            bestSpan = span;
        }
    }
    if (best >= 0) {
        setFolded(best, false);
    }
}

void CodeEditor::foldAll()
{
    QSet<int> hidden;
    QList<FoldAnchor> anchors;
    for (qsizetype i = 0; i < foldRegions_.size(); ++i) {
        const LightCpp::Core::Fold::FoldRegion& region = foldRegions_.at(i);
        // Skip anything already inside a fold that is being taken: collapsing
        // a block nobody can see only stores state with no way to reach it.
        if (hidden.contains(int(region.startLine))) {
            continue;
        }
        const QTextBlock header = document()->findBlockByNumber(int(region.startLine));
        FoldAnchor anchor;
        anchor.regionIndex = int(i);
        anchor.headerText = header.isValid() ? header.text() : QString();
        anchors.append(anchor);
        for (std::size_t line = region.startLine + 1; line <= region.endLine; ++line) {
            hidden.insert(int(line));
        }
    }
    folded_ = anchors;
    applyFoldState();
    keepCaretOutsideFolds();
}

void CodeEditor::unfoldAll()
{
    if (folded_.isEmpty()) {
        return;
    }
    folded_.clear();
    applyFoldState();
}

void CodeEditor::unfoldLine(int line)
{
    if (!hiddenLines_.contains(line)) {
        return;
    }
    for (const FoldAnchor& anchor : std::as_const(folded_)) {
        if (anchor.regionIndex < 0 || anchor.regionIndex >= foldRegions_.size()) {
            continue;
        }
        const LightCpp::Core::Fold::FoldRegion& region =
            foldRegions_.at(anchor.regionIndex);
        if (int(region.startLine) < line && line <= int(region.endLine)) {
            setFolded(anchor.regionIndex, false);
            return;
        }
    }
}

void CodeEditor::keepCaretOutsideFolds()
{
    if (hiddenLines_.isEmpty()) {
        return;
    }
    QTextCursor cursor = textCursor();
    const int line = cursor.blockNumber();
    if (!hiddenLines_.contains(line)) {
        return;
    }
    // Park the caret at the end of the header line that swallowed it, which is
    // where the user was looking when they folded.
    for (const FoldAnchor& anchor : std::as_const(folded_)) {
        if (anchor.regionIndex < 0 || anchor.regionIndex >= foldRegions_.size()) {
            continue;
        }
        const LightCpp::Core::Fold::FoldRegion& region =
            foldRegions_.at(anchor.regionIndex);
        if (int(region.startLine) < line && line <= int(region.endLine)) {
            const QTextBlock header = document()->findBlockByNumber(int(region.startLine));
            if (header.isValid()) {
                cursor.setPosition(header.position()
                                   + qMax<qsizetype>(header.length() - 1, 0));
                setTextCursor(cursor);
            }
            return;
        }
    }
}

void CodeEditor::handleFoldMarkerClick(const QPoint& pos)
{
    if (pos.x() >= kFoldColumnWidth) {
        return;  // that part of the gutter belongs to the line numbers
    }
    // The gutter and the viewport share the same vertical geometry, so a y
    // measured in the gutter is already a y in the viewport.
    const int line = cursorForPosition(QPoint(0, pos.y())).blockNumber();
    toggleFoldAtLine(line);
}

void CodeEditor::paintFoldMarker(QPainter* painter, int top, int bottom, int regionIndex)
{
    if (regionIndex < 0 || regionIndex >= foldRegions_.size()) {
        return;
    }

    QColor color = palette().color(QPalette::Text);
    color.setAlpha(140);

    // drawRect's width already includes the pen, so a size-8 box spanning
    // [x, x+8] is the 9-pixel marker.
    const int box = kFoldMarkerSize - 1;
    const int x = qMax(0, (kFoldColumnWidth - kFoldMarkerSize) / 2);
    const int y = top + qMax(0, (bottom - top - kFoldMarkerSize) / 2);

    painter->setPen(QPen(color, 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(x, y, box, box);

    QFont markerFont = font();
    if (markerFont.pointSizeF() > 0.0) {
        // The glyph lives in a 9-pixel box, so it needs to be markedly
        // smaller than the line numbers next to it.
        markerFont.setPointSizeF(qMax(6.0, markerFont.pointSizeF() - 3.0));
    }
    markerFont.setBold(true);
    painter->setFont(markerFont);
    painter->setPen(color);
    painter->drawText(x, y, box + 1, box + 1, Qt::AlignCenter,
                      isFolded(regionIndex) ? QString(QChar(0x2212)) : QString(u'+'));

    if (isFolded(regionIndex)) {
        // A collapsed range usually has more lines below it, so the marker
        // keeps a hairline running down to the bottom of the visible gutter.
        const int cx = x + box / 2;
        painter->setPen(QPen(color, 1));
        painter->drawLine(cx, y + box + 1, cx, lineNumberArea_->rect().bottom());
    }
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent* event)
{
    QPainter painter(lineNumberArea_);
    painter.fillRect(event->rect(), palette().color(QPalette::Window));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    // Line-number text: muted version of the text color.
    QColor numColor = palette().color(QPalette::Text);
    numColor.setAlpha(120);
    painter.setPen(numColor);

    // Pre-index diagnostics by line for the gutter markers: remember the
    // most severe diagnostic on each line (1=Error beats 2=Warning...).
    QHash<int, int> lineSeverity;
    for (const LightCpp::App::LspDiagnostic& diag : std::as_const(diagnostics_)) {
        const LightCpp::App::LspDiagnostic d = normalizedDiagnostic(diag);
        const int existing = lineSeverity.value(d.startLine, 9);
        if (d.severity < existing) {
            lineSeverity.insert(d.startLine, d.severity);
        }
    }

    // Which lines open a foldable block, and which region that is. The list is
    // ordered outermost-first, so for several regions opening on one line the
    // last write leaves the innermost — which is the one a click should reach.
    QHash<int, int> foldAtLine;
    for (qsizetype i = 0; i < foldRegions_.size(); ++i) {
        foldAtLine.insert(int(foldRegions_.at(i).startLine), int(i));
    }

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            // Fold marker first, then the diagnostic dot, then the number: the
            // three share the gutter left to right in that order.
            const auto foldIt = foldAtLine.constFind(blockNumber);
            if (foldIt != foldAtLine.constEnd()) {
                painter.save();
                paintFoldMarker(&painter, top, bottom, foldIt.value());
                painter.restore();
            }

            const QString number = QString::number(blockNumber + 1);
            painter.setPen(numColor);
            // The rect height must be at least one line box — an empty
            // (height 0) rect makes QPainter skip drawing the text, which
            // is why the gutter showed but the numbers did not.
            painter.drawText(kFoldColumnWidth, top,
                             lineNumberArea_->width() - 2 - kFoldColumnWidth,
                             fontMetrics().height(),
                             Qt::AlignRight | Qt::AlignVCenter, number);

            // Diagnostic marker, just right of the fold strip.
            const int sev = lineSeverity.value(blockNumber, 0);
            if (sev > 0 && sev <= 3) {
                QColor mark = (sev == 1) ? QColor(Qt::red)
                               : (sev == 2) ? QColor(0xE0, 0xA0, 0x00)
                                            : QColor(Qt::gray);
                painter.setPen(Qt::NoPen);
                painter.setBrush(mark);
                painter.drawEllipse(kFoldColumnWidth + 2, top + 2, 6, 6);
                painter.setPen(numColor);
                painter.setBrush(Qt::NoBrush);
            }
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}
