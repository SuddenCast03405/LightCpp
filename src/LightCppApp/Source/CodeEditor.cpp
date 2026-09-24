// CodeEditor — QPlainTextEdit with line-number gutter and syntax highlight.

#include "LightCpp/App/CodeEditor.hpp"
#include "LightCpp/App/CompletionPopup.hpp"
#include "LightCpp/App/CppSyntaxHighlighter.hpp"

#include <QApplication>
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

    // Attach the C++ highlighter to the underlying document.
    highlighter_ = new CppSyntaxHighlighter(document());

    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged,
            this, &CodeEditor::highlightCurrentLine);

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
    int space = 3 + fontMetrics().horizontalAdvance(u'9') * (digits + 1);
    return space;
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

void CodeEditor::keyPressEvent(QKeyEvent* event)
{
    const int key = event->key();
    const QString text = event->text();

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
    QPlainTextEdit::mousePressEvent(event);
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

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            painter.setPen(numColor);
            // The rect height must be at least one line box — an empty
            // (height 0) rect makes QPainter skip drawing the text, which
            // is why the gutter showed but the numbers did not.
            painter.drawText(0, top, lineNumberArea_->width() - 2,
                             fontMetrics().height(),
                             Qt::AlignRight | Qt::AlignVCenter, number);

            // Diagnostic marker on the left edge of the gutter.
            const int sev = lineSeverity.value(blockNumber, 0);
            if (sev > 0 && sev <= 3) {
                QColor mark = (sev == 1) ? QColor(Qt::red)
                               : (sev == 2) ? QColor(0xE0, 0xA0, 0x00)
                                            : QColor(Qt::gray);
                painter.setPen(Qt::NoPen);
                painter.setBrush(mark);
                painter.drawEllipse(2, top + 2, 6, 6);
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
