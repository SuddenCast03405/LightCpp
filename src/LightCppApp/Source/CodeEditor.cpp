// CodeEditor — QPlainTextEdit with line-number gutter and syntax highlight.

#include "LightCpp/App/CodeEditor.hpp"
#include "LightCpp/App/CompletionPopup.hpp"
#include "LightCpp/App/CppSyntaxHighlighter.hpp"
#include "LightCpp/Core/BracketMatch.hpp"
#include "LightCpp/Core/WordOccurrences.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QScreen>
#include <QStringList>
#include <QTextBlock>
#include <QResizeEvent>
#include <QTimer>
#include <QToolTip>
#include <QWindow>
#include <QAbstractTextDocumentLayout>

#include <utility>
#include <algorithm>
#include <vector>

namespace {

namespace {

/// The colour a semantic role is drawn in. The lexical highlighter owns
/// keywords and literals; these are the roles a lexer cannot distinguish, so
/// the two sets deliberately overlap only on "type" and "function".
QColor colourForRole(int role)
{
    using Role = LightCpp::Workbench::Lsp::TokenRole;
    switch (static_cast<Role>(role)) {
    case Role::Type:
    case Role::TypeParameter:
        return QColor(0x2A, 0x9D, 0x8F);   // teal
    case Role::EnumMember:
        return QColor(0x1E, 0x88, 0x5E);
    case Role::Namespace:
        return QColor(0x7A, 0x6A, 0xC0);
    case Role::Function:
        return QColor(0x79, 0x5E, 0x26);
    case Role::Macro:
        return QColor(0xB0, 0x50, 0x8A);
    case Role::Parameter:
        return QColor(0x9A, 0x67, 0x00);
    case Role::Variable:
    case Role::Property:
        return QColor(0x1F, 0x5F, 0xA8);
    case Role::Keyword:
    case Role::Modifier:
        return QColor(0x00, 0x33, 0x99);
    case Role::Comment:
        return QColor(0x60, 0x80, 0x60);
    case Role::String:
        return QColor(0xA3, 0x1D, 0x1D);
    case Role::Number:
        return QColor(0x17, 0x50, 0x8A);
    case Role::Label:
        return QColor(0x60, 0x60, 0x60);
    // Operators, brackets, regexps and decorators keep the lexical colour:
    // changing their hue makes dense code harder to read, not easier.
    case Role::Operator:
    case Role::Bracket:
    case Role::Regexp:
    case Role::Decorator:
    case Role::Event:
    case Role::Unknown:
        return QColor();
    }
    return QColor();
}

}  // namespace


/// The gutter is one column wider than the line numbers need, holding the
/// fold markers to the left of the diagnostic dots.
constexpr int kFoldColumnWidth = 12;
/// Side of the square fold marker drawn in that column.
constexpr int kFoldMarkerSize = 9;
/// Same-symbol highlight caps: past either limit the caret stops lighting up
/// occurrences, so a huge file cannot pay a full scan on every cursor move.
constexpr int kWordHighlightMaxBlocks = 2000;
constexpr std::size_t kWordHighlightMaxHits = 200;

/// Past this many blocks the fold analysis is not run at all.
///
/// `rebuildFoldRegions()` copies the whole document out (`toPlainText()`),
/// converts every line to a `std::string` and scans the lot, and it runs from
/// a 120 ms idle timer after *every* edit — so on a generated file of a few
/// hundred thousand lines each pause in typing paid a full-document pass.
/// Folding is a convenience; 20000 blocks is already far past the point where
/// a human scrolls to a fold marker, and the word-highlight cap above is the
/// same trade (silently cheaper, never slower).
constexpr int kFoldMaxBlocks = 20000;

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
    applyTabWidthForCurrentFont();
    // Qt wraps by default; the IDE's default is files.longLineMode == "scroll"
    // (one row per line, sideways scrolling), so the widget has to be told
    // before it lays anything out. setLongLinesWrap() is a no-op while
    // longLinesWrap_ is already false, hence the direct call.
    setLineWrapMode(QPlainTextEdit::NoWrap);

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
    // state itself survives re-parsing (see FoldAnchor). It runs on a short
    // idle timer rather than inside textChanged: a keystroke or a large paste
    // must not pay for a whole-document analysis inline.
    foldRebuildTimer_ = new QTimer(this);
    foldRebuildTimer_->setSingleShot(true);
    foldRebuildTimer_->setInterval(120);
    connect(foldRebuildTimer_, &QTimer::timeout,
            this, &CodeEditor::rebuildFoldRegionsOnIdle);
    connect(this, &QPlainTextEdit::textChanged,
            this, [this] { foldRebuildTimer_->start(); });

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

void CodeEditor::setEditorFont(const QFont& font)
{
    setFont(font);
    applyTabWidthForCurrentFont();
    // The gutter width is derived from the font metrics, so it has to be
    // recomputed after the font changes.
    updateLineNumberAreaWidth(0);
    lineNumberArea_->update();
}

void CodeEditor::setSyntaxHighlightEnabled(bool enabled)
{
    if (syntaxHighlightEnabled_ == enabled) {
        return;
    }
    syntaxHighlightEnabled_ = enabled;
    if (highlighter_ == nullptr) {
        return;
    }
    if (enabled) {
        highlighter_->setDocument(document());
        highlighter_->rehighlight();
    } else {
        // Detaching the document leaves the text plain; the highlighter
        // object itself stays alive for a later re-enable.
        highlighter_->setDocument(nullptr);
    }
}

void CodeEditor::applyTabWidthForCurrentFont()
{
    // A tab is exactly four monospace cells. Qt's own default (80px) works
    // out to about 10.5 cells at this size, which is not what C++ code is
    // normally indented with — and it makes a visual column stop line up with
    // nothing. Measured from a space so it stays correct if the font changes.
    setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')) * 4.0);
}

void CodeEditor::setLongLinesWrap(bool wrap)
{
    if (longLinesWrap_ == wrap) {
        return;
    }
    longLinesWrap_ = wrap;
    // Switching the wrap mode re-lays out the whole document, so the folds
    // have to be re-applied against the new block geometry.
    setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth
                         : QPlainTextEdit::NoWrap);
    applyFoldState();
}

void CodeEditor::setShowWhitespaces(bool show)
{
    if (showWhitespaces_ == show) {
        return;
    }
    showWhitespaces_ = show;
    QTextOption option = document()->defaultTextOption();
    if (show) {
        option.setFlags(option.flags() | QTextOption::ShowTabsAndSpaces);
    } else {
        option.setFlags(option.flags() & ~QTextOption::ShowTabsAndSpaces);
    }
    document()->setDefaultTextOption(option);
}

void CodeEditor::setShowIndentGuides(bool show)
{
    if (showIndentGuides_ == show) {
        return;
    }
    showIndentGuides_ = show;
    viewport()->update();
}

void CodeEditor::paintEvent(QPaintEvent* event)
{
    QPlainTextEdit::paintEvent(event);
    if (!showIndentGuides_) {
        return;
    }

    // A muted vertical line at every indent stop a line's leading whitespace
    // passes: spaces advance by their own width, tabs snap to the next tab
    // stop, and tabStopDistance() is the guide spacing. The content-start
    // position always gets a guide too, so a 2-space indent is not invisible
    // just because it never reaches a tab stop.
    QPainter painter(viewport());
    QColor color = palette().color(QPalette::Text);
    color.setAlpha(40);
    painter.setPen(QPen(color, 1));

    const qreal spaceWidth
        = qreal(fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    const qreal tabStop = qreal(tabStopDistance());
    if (spaceWidth <= 0 || tabStop <= 0) {
        return;
    }

    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int viewBottom = viewport()->rect().bottom();
    while (block.isValid() && top <= viewBottom) {
        if (block.isVisible() && bottom >= viewport()->rect().top()) {
            const QString text = block.text();
            qreal x = 0;
            int i = 0;
            while (i < text.size()
                   && (text.at(i) == u' ' || text.at(i) == u'\t')) {
                x += text.at(i) == u' ' ? spaceWidth
                                        : tabStop - std::fmod(x, tabStop);
                ++i;
            }
            if (x > 0) {
                // Tab stops passed on the way in, then the content start.
                // Counted rather than accumulated: `g += tabStop` on a qreal
                // drifts, and the drift decides whether a guide is drawn one
                // pixel before the content or not at all.
                for (int stop = 1; stop * tabStop < x; ++stop) {
                    const int guideX = qRound(stop * tabStop) - 1;
                    painter.drawLine(guideX, top, guideX, bottom);
                }
                painter.drawLine(qRound(x) - 1, top, qRound(x) - 1, bottom);
            }
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
    }
}

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

int CodeEditor::blameAreaWidth() const
{
    if (blame_.isEmpty()) {
        return 0;
    }
    // Author column, a gap, then the fixed-width day. Measured with the
    // widest digit so a proportional font still fits, plus the right margin
    // the blame text is drawn with.
    const int unit = fontMetrics().horizontalAdvance(u'9');
    return (blameAuthorWidth_ + 2 + 10) * unit + 10;
}

void CodeEditor::setBlame(const LightCpp::Workbench::Git::BlameLookup& lookup)
{
    blame_ = lookup;
    updateLineNumberAreaWidth(blockCount());
    lineNumberArea_->update();
}

void CodeEditor::setBlameAuthorWidth(int characters)
{
    const int clamped = std::clamp(characters, 4, 40);
    if (clamped == blameAuthorWidth_) {
        return;
    }
    blameAuthorWidth_ = clamped;
    updateLineNumberAreaWidth(blockCount());
    lineNumberArea_->update();
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
    return space + kFoldColumnWidth + blameAreaWidth();
}

void CodeEditor::updateLineNumberAreaWidth(int)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect& rect, int dy)
{
    // Both scroll directions: deleting lines above the viewport produces a
    // negative dy, and repainting just the changed band then leaves stale
    // line numbers below it — the gutter briefly shows pre-delete numbering.
    if (dy != 0) {
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

    // Bookmarked lines: a soft blue full-width background, so a bookmark
    // stays visible while scrolling even without looking at the gutter.
    for (const int line : std::as_const(bookmarkLines_)) {
        const QTextBlock block = document()->findBlockByNumber(line);
        if (!block.isValid() || !block.isVisible()) {
            continue;  // stale line after an edit
        }
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(QColor(0x66, 0x99, 0xFF, 45));
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = QTextCursor(block);
        sel.cursor.clearSelection();
        selections.append(sel);
    }

    // --- bracket match + same-symbol highlight ---------------------------
    // Both only make sense on a bare caret; with an active selection the
    // user is pointing at something already. The plain text and its UTF-8
    // byte image are cached per document revision: they used to be rebuilt
    // (two whole-document copies) on every single cursor move.
    const QTextCursor caret = textCursor();
    if (!caret.hasSelection()) {
        const int revision = document()->revision();
        if (selectionTextRevision_ != revision) {
            selectionText_ = toPlainText();
            const QByteArray utf8 = selectionText_.toUtf8();
            selectionTextBytes_.assign(utf8.constData(),
                                       utf8.constData() + utf8.size());
            // Char index -> UTF-8 byte offset. Surrogate halves each advance
            // the byte counter by 2 (a non-BMP pair encodes to 4 bytes), so
            // the map stays monotonic and every char index lands on a
            // boundary.
            selectionCharToByte_.clear();
            selectionCharToByte_.reserve(selectionText_.size() + 1);
            selectionCharToByte_.push_back(0);
            int byte = 0;
            for (const QChar c : selectionText_) {
                const char16_t u = c.unicode();
                byte += u < 0x80   ? 1
                        : u < 0x800 ? 2
                        : u >= 0xD800 && u <= 0xDFFF
                            ? 2  // half of a surrogate pair
                            : 3;
                selectionCharToByte_.push_back(byte);
            }
            selectionTextRevision_ = revision;
        }
        const QString& plain = selectionText_;
        const std::string& text = selectionTextBytes_;
        // Byte positions from the Core scanners are UTF-8 offsets — they are
        // only equal to document positions for pure ASCII. Convert via the
        // map (the scanners return ASCII boundaries, so an exact entry
        // always exists); -1 means "outside the document", skip that hit.
        const auto charForByte = [this](int byteOffset) -> int {
            if (byteOffset < 0
                || byteOffset > int(selectionTextBytes_.size())) {
                return -1;
            }
            const auto it = std::lower_bound(
                selectionCharToByte_.begin(), selectionCharToByte_.end(),
                byteOffset);
            if (it == selectionCharToByte_.end() || *it != byteOffset) {
                return -1;
            }
            return int(it - selectionCharToByte_.begin());
        };

        // The bracket at the caret, or the one just left of it — either way
        // the match gets an outline box on both ends.
        const int pos = caret.position();
        if (pos >= 0 && pos <= plain.size()) {
            int bracketByte = selectionCharToByte_[pos];
            bool atBracket = false;
            if (bracketByte < int(text.size())
                && LightCpp::Core::Brackets::isBracket(
                       text[std::size_t(bracketByte)])) {
                atBracket = true;
            } else if (bracketByte > 0
                       && LightCpp::Core::Brackets::isBracket(
                              text[std::size_t(bracketByte) - 1])) {
                --bracketByte;
                atBracket = true;
            }
            if (atBracket) {
                const auto match = LightCpp::Core::Brackets::matchBracket(
                    text, std::size_t(bracketByte));
                if (match.has_value()) {
                    const int openChar = charForByte(bracketByte);
                    const int closeChar = charForByte(int(*match));
                    if (openChar >= 0 && closeChar >= 0) {
                        QColor edge = palette().color(QPalette::Text);
                        edge.setAlpha(160);
                        for (const int index : {openChar, closeChar}) {
                            QTextCursor one(document());
                            one.setPosition(index);
                            one.setPosition(index + 1,
                                            QTextCursor::KeepAnchor);
                            QTextEdit::ExtraSelection sel;
                            sel.format.setProperty(QTextFormat::OutlinePen,
                                                   QPen(edge, 1));
                            sel.cursor = one;
                            selections.append(sel);
                        }
                    }
                }
            }
        }

        // The identifier under the caret lights up everywhere it occurs,
        // whole words only. Skipped on very large files: a full-document
        // scan per caret move is exactly what the large-file gates avoid.
        QTextCursor wordCur = caret;
        wordCur.select(QTextCursor::WordUnderCursor);
        const QString word = wordCur.selectedText();
        bool identifier = word.size() >= 2;
        for (const QChar c : word) {
            const char ch = c.toLatin1();
            if (!LightCpp::Core::Words::isIdentChar(ch)) {
                identifier = false;
                break;
            }
        }
        if (identifier
            && blockCount() <= kWordHighlightMaxBlocks) {
            const auto hits = LightCpp::Core::Words::wordOccurrences(
                text, word.toStdString(), kWordHighlightMaxHits);
            QColor tint(0xFF, 0xD8, 0x66, 70);  // soft amber, both themes
            for (const auto& [start, end] : hits) {
                const int startChar = charForByte(int(start));
                const int endChar = charForByte(int(end));
                if (startChar < 0 || endChar < 0) {
                    continue;
                }
                QTextCursor hit(document());
                hit.setPosition(startChar);
                hit.setPosition(endChar, QTextCursor::KeepAnchor);
                QTextEdit::ExtraSelection sel;
                sel.format.setBackground(tint);
                sel.cursor = hit;
                selections.append(sel);
            }
        }
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

    // Semantic tokens, last so their foreground wins over the lexical colour
    // of a keyword used as an identifier. Only the foreground is set: an
    // existing bold/italic and the diagnostic underline stay, which is the
    // whole point of layering instead of replacing.
    if (semanticHighlighting_ && !semanticSpans_.isEmpty()) {
        for (const SemanticSpan& span : std::as_const(semanticSpans_)) {
            const QColor colour = colourForRole(static_cast<int>(span.role));
            if (!colour.isValid() || span.length <= 0) {
                continue;
            }
            const QTextBlock block = document()->findBlockByNumber(span.line);
            if (!block.isValid()) {
                continue;
            }
            // Trust the buffer for bounds: the server's answer is one edit
            // behind, so a span can outlive the text it described.
            const int lineLength = qMax(block.length() - 1, 0);
            const int column = qBound(0, span.column, lineLength);
            const int length = qBound(0, span.length, lineLength - column);
            if (length <= 0) {
                continue;
            }
            QTextCursor cursor(block);
            cursor.setPosition(block.position() + column);
            cursor.setPosition(block.position() + column + length,
                               QTextCursor::KeepAnchor);
            QTextEdit::ExtraSelection sel;
            sel.format.setForeground(colour);
            sel.cursor = cursor;
            selections.append(sel);
        }
    }

    setExtraSelections(selections);
    lineNumberArea_->update();
}

void CodeEditor::setBreakpointLines(const QSet<int>& lines)
{
    if (breakpointLines_ == lines) {
        return;
    }
    breakpointLines_ = lines;
    updateExtraSelections();  // keeps the gutter repaint simple and central
}

// ---------------------------------------------------------------------------
// Bookmarks
// ---------------------------------------------------------------------------
void CodeEditor::toggleBookmark()
{
    const int line = textCursor().blockNumber();
    if (!bookmarkLines_.remove(line)) {
        bookmarkLines_.insert(line);
    }
    updateExtraSelections();
}

void CodeEditor::nextBookmark()
{
    if (bookmarkLines_.isEmpty()) {
        return;
    }
    QList<int> lines = bookmarkLines_.values();
    std::sort(lines.begin(), lines.end());
    const int current = textCursor().blockNumber();
    // First bookmark strictly below the caret, wrapping to the top.
    const auto it = std::upper_bound(lines.constBegin(), lines.constEnd(), current);
    const int target = (it != lines.constEnd())
                           ? *it
                           : lines.first();
    const QTextBlock block = document()->findBlockByNumber(target);
    if (!block.isValid()) {
        return;  // stale line after an edit; harmless
    }
    QTextCursor cursor(block);
    setTextCursor(cursor);
    centerCursor();
}

void CodeEditor::prevBookmark()
{
    if (bookmarkLines_.isEmpty()) {
        return;
    }
    QList<int> lines = bookmarkLines_.values();
    std::sort(lines.begin(), lines.end());
    const int current = textCursor().blockNumber();
    // First bookmark strictly above the caret, wrapping to the bottom.
    const auto it = std::lower_bound(lines.constBegin(), lines.constEnd(), current);
    const int target = (it != lines.constBegin())
                           ? *std::prev(it)
                           : lines.last();
    const QTextBlock block = document()->findBlockByNumber(target);
    if (!block.isValid()) {
        return;
    }
    QTextCursor cursor(block);
    setTextCursor(cursor);
    centerCursor();
}

void CodeEditor::setSemanticSpans(const QList<SemanticSpan>& spans)
{
    semanticSpans_ = spans;
    updateExtraSelections();
}

void CodeEditor::clearSemanticSpans()
{
    if (semanticSpans_.isEmpty()) {
        return;
    }
    semanticSpans_.clear();
    updateExtraSelections();
}

void CodeEditor::setSemanticHighlightingEnabled(bool enabled)
{
    if (semanticHighlighting_ == enabled) {
        return;
    }
    semanticHighlighting_ = enabled;
    updateExtraSelections();
}

void CodeEditor::setLineMarks(const QList<int>& marks)
{
    if (lineMarks_ == marks) {
        return;
    }
    lineMarks_ = marks;
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
    // Watch the top-level window so the popup follows it around: the popup
    // is its own top-level widget, and nothing else repositions it when the
    // main window is dragged or resized.
    QObject* topLevel = window();
    if (topLevel != nullptr && popupWatchWindow_ != topLevel) {
        if (popupWatchWindow_ != nullptr) {
            popupWatchWindow_->removeEventFilter(this);
        }
        topLevel->installEventFilter(this);
        popupWatchWindow_ = topLevel;
    }
    // Position the popup just below the cursor line.
    const QRect cr = cursorRect();
    QPoint pos = viewport()->mapToGlobal(cr.bottomLeft());
    popup_->present(items, pos);
    // libclang hands over the raw, unfiltered candidate list, and a
    // language-server answer may have been computed for an earlier prefix
    // (typing continued while the request was in flight). Filter against
    // the prefix the caret sits on right now — an empty prefix keeps the
    // list as-is.
    refreshPopupFilter();
}

void CodeEditor::repositionPopup()
{
    if (!popup_ || !popup_->isVisible()) {
        return;
    }
    const QRect cr = cursorRect();
    QPoint pos = viewport()->mapToGlobal(cr.bottomLeft());
    // Same on-screen clamping present() applies on first show.
    QScreen* screen = nullptr;
    if (auto* handle = popup_->windowHandle()) {
        screen = handle->screen();
    }
    if (screen == nullptr) {
        screen = QApplication::primaryScreen();
    }
    if (screen != nullptr) {
        const QRect g = screen->availableGeometry();
        const QSize size = popup_->size();
        if (pos.x() + size.width() > g.right()) {
            pos.setX(qMax(g.left(), g.right() - size.width()));
        }
        if (pos.y() + size.height() > g.bottom()) {
            // Same rule as present(): flip above the caret, else dock to the
            // bottom edge — never clamp to the top (the popup would detach).
            const int flipped = pos.y() - size.height() - 20;
            pos.setY(flipped >= g.top()
                         ? flipped
                         : qMax(g.top(), g.bottom() - size.height() + 1));
        }
    }
    popup_->move(pos);
}

bool CodeEditor::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == popupWatchWindow_ && popup_ && popup_->isVisible()) {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            repositionPopup();
        }
    }
    return QPlainTextEdit::eventFilter(watched, event);
}

void CodeEditor::dismissPopup()
{
    popup_->dismiss();
}

void CodeEditor::showSignatureHelp(const QString& html)
{
    if (html.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    // Anchor just below the caret line, like every IDE's parameter hint.
    const QRect r = cursorRect();
    QToolTip::showText(
        viewport()->mapToGlobal(QPoint(r.left(), r.bottom() + 2)), html,
        this);
}

void CodeEditor::showHoverInfo(const QPoint& globalPos, const QString& html)
{
    if (html.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    QToolTip::showText(globalPos, html, this);
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

bool CodeEditor::smartBackspace()
{
    if (isReadOnly()) {
        return false;
    }
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        return false;
    }
    const int col = cursor.positionInBlock();
    if (col <= 0) {
        return false;
    }
    const QString line = cursor.block().text();
    // Only whitespace between the line start and the caret turns Backspace
    // into a dedent; with real text before the caret it stays a normal
    // single-character delete.
    for (int i = 0; i < col; ++i) {
        const QChar ch = line.at(i);
        if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t')) {
            return false;
        }
    }
    // A leading tab already IS one indent unit — the default delete removes
    // it in one press, so there is nothing smarter to do.
    if (line.at(col - 1) == QLatin1Char('\t')) {
        return false;
    }
    // All spaces: delete back to the previous tab stop (the 4-space indent
    // unit this editor displays). col=4 -> 4, col=5..7 -> to col 4, col=2 -> 2.
    const int remove = col - 4 * ((col - 1) / 4);
    if (remove <= 0) {
        return false;
    }
    cursor.beginEditBlock();
    cursor.setPosition(cursor.position() - remove);
    cursor.setPosition(cursor.position() + remove, QTextCursor::KeepAnchor);
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

    // Any Enter leaves the call line — the signature tooltip is stale there.
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        QToolTip::hideText();
    }

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
        // `(` opens a call context: ask for the signature tooltip. `)`
        // leaves it (stepping over a closer included) — hide the tooltip.
        if (text.at(0) == QLatin1Char('(')) {
            emit signatureHelpRequested(textCursor().blockNumber(),
                                        textCursor().columnNumber());
        } else if (text.at(0) == QLatin1Char(')')) {
            QToolTip::hideText();
        }
        return;
    }

    // Backspace between the two halves of an empty pair removes both;
    // over leading whitespace it dedents a whole indent unit.
    if (key == Qt::Key_Backspace && !event->isAutoRepeat()) {
        if (backspaceEmptyPair() || smartBackspace()) {
            completionIdleTimer_->stop();
            if (popup_ && popup_->isVisible()) {
                dismissPopup();
            }
            return;
        }
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
            // A comma continues the parameter list: refresh the signature
            // tooltip after the insertion.
            if (text == QLatin1String(",")) {
                completionIdleTimer_->stop();
                QPlainTextEdit::keyPressEvent(event);
                emit signatureHelpRequested(textCursor().blockNumber(),
                                            textCursor().columnNumber());
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
            // A comma inside a call: refresh the signature tooltip.
            if (text == QLatin1String(",")) {
                QPlainTextEdit::keyPressEvent(event);
                emit signatureHelpRequested(textCursor().blockNumber(),
                                            textCursor().columnNumber());
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

    // Built-in snippet expansion: Tab after a trigger word ("for", "main",
    // ...) replaces the word with the snippet body. Runs before the default
    // Tab indent handling, and only when the completion popup is closed.
    if (key == Qt::Key_Tab && !popupVisible) {
        if (expandSnippetAtCursor()) {
            completionIdleTimer_->stop();
            return;
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

// ---------------------------------------------------------------------------
// Built-in snippets
// ---------------------------------------------------------------------------

const QHash<QString, QString>& CodeEditor::snippetTable()
{
    // Trigger word -> body. A single `|` in the body marks where the caret
    // ends up after the expansion (no marker = caret at the end).
    static const QHash<QString, QString> table = {
        {QStringLiteral("for"),
         QStringLiteral("for (int i = 0; i < n; ++i) {\n|\n}")},
        {QStringLiteral("while"),
         QStringLiteral("while (|) {\n\n}")},
        {QStringLiteral("do"),
         QStringLiteral("do {\n\n} while (|);")},
        {QStringLiteral("if"),
         QStringLiteral("if (|) {\n\n}")},
        {QStringLiteral("switch"),
         QStringLiteral("switch (|) {\ncase 0:\nbreak;\ndefault:\nbreak;\n}")},
        {QStringLiteral("try"),
         QStringLiteral("try {\n\n} catch (const std::exception& e) {\n|\n}")},
        {QStringLiteral("main"),
         QStringLiteral("int main(int argc, char* argv[]) {\n|\nreturn 0;\n}")},
        {QStringLiteral("class"),
         QStringLiteral("class | {\npublic:\nprivate:\n};")},
        {QStringLiteral("struct"),
         QStringLiteral("struct | {\n\n};")},
        {QStringLiteral("namespace"),
         QStringLiteral("namespace | {\n\n}")},
        {QStringLiteral("guard"),
         QStringLiteral("#ifndef NAME_H\n#define NAME_H\n\n|\n\n#endif // NAME_H")},
    };
    return table;
}

bool CodeEditor::expandSnippetAtCursor()
{
    // Programmatic edits bypass Qt's read-only protection, so the guard has
    // to be explicit. A selection means "indent/replace deliberately" — a
    // Tab must not silently eat it for a snippet.
    if (isReadOnly() || textCursor().hasSelection()) {
        return false;
    }

    const QTextCursor c = textCursor();
    const QString before = c.block().text().left(c.positionInBlock());
    const QString after = c.block().text().mid(c.positionInBlock());

    // The trigger word must own its line: only leading whitespace before it
    // and only trailing whitespace after it. Otherwise `int main` + Tab
    // would expand into `int int main(...)`, and every `for`/`if` sitting
    // inside other code (or a comment) would be fair game for replacement.
    if (!after.trimmed().isEmpty()) {
        return false;
    }

    // The identifier immediately before the caret is the trigger word.
    static const QRegularExpression ident(
        QStringLiteral("[A-Za-z_][A-Za-z0-9_]*$"));
    const QRegularExpressionMatch m = ident.match(before);
    if (!m.hasMatch()) {
        return false;
    }
    const QString word = m.captured();
    const auto it = snippetTable().constFind(word);
    if (it == snippetTable().constEnd()) {
        return false;
    }
    // `x.for` / `a->b` territory: a member access, not a snippet.
    const int wordStart = m.capturedStart();
    if (wordStart > 0) {
        const QChar prev = before.at(wordStart - 1);
        if (prev == QLatin1Char('.') || prev == QLatin1Char(':')
            || prev == QLatin1Char('>') || !prev.isSpace()) {
            return false;
        }
    }

    // The trigger line's leading whitespace indents every body line; the
    // `|` marker is stripped and its final caret offset remembered.
    int ws = 0;
    while (ws < before.size() && before.at(ws).isSpace()) {
        ++ws;
    }
    const QString indent = before.left(ws);

    QString expanded;
    int caretOffset = -1;
    const QStringList lines = it.value().split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        const int local = line.indexOf(QLatin1Char('|'));
        if (local >= 0) {
            line.remove(local, 1);
        }
        if (i > 0) {
            expanded += QLatin1Char('\n');
        }
        if (!line.isEmpty()) {
            expanded += indent;
        }
        if (local >= 0 && caretOffset < 0) {
            caretOffset = expanded.size() + local;
        }
        expanded += line;
    }
    if (caretOffset < 0) {
        caretOffset = expanded.size();  // no marker -> end of the snippet
    }

    // One edit block: deleting the trigger and inserting the body is a
    // single undo step, not two.
    QTextCursor edit = textCursor();
    edit.beginEditBlock();
    edit.setPosition(c.position() - word.size());
    edit.setPosition(c.position(), QTextCursor::KeepAnchor);
    edit.removeSelectedText();
    edit.insertText(expanded);
    edit.endEditBlock();
    edit.setPosition(edit.position() - expanded.size() + caretOffset);
    setTextCursor(edit);
    return true;
}

void CodeEditor::insertFromMimeData(const QMimeData* source)
{
    // A pending as-you-type request belongs to the text before the paste, and
    // a popup built from it would filter against the wrong prefix.
    completionIdleTimer_->stop();
    if (popup_ && popup_->isVisible()) {
        dismissPopup();
    }

    QPlainTextEdit::insertFromMimeData(source);

    // The debounced didChange that textChanged schedules is enough to get the
    // buffer to clangd, but a paste is a deliberate, complete edit: asking for
    // a fresh completion list right away means the popup offers the symbols of
    // the pasted text instead of staying silent until the next keystroke.
    const QTextCursor c = textCursor();
    emit completionRequested(c.blockNumber(), c.positionInBlock(), QString());
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
        // No diagnostic here: offer the identifier to the language server.
        // Whether an answer arrives in time is not this class's problem —
        // a tooltip that never appears is better than one that appears
        // after the mouse has moved on, so the receiver decides.
        Q_EMIT hoverRequested(line, col, helpEvent->globalPos());
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

    // The server's textEdit range wins when it names exactly the caret's
    // line: it replaces the span the server chose (which may differ from the
    // identifier heuristic below, e.g. "#incl" + "ude <...>"). A stale range
    // (the answer computed before more typing) or a multi-line one falls
    // back to the local prefix handling.
    const QString insert = item.insertText.isEmpty() ? item.label : item.insertText;
    if (item.hasTextEditRange && item.rangeStartLine == item.rangeEndLine
        && item.rangeStartLine == block.blockNumber()) {
        const int startCol = qBound(0, item.rangeStartColumn,
                                    qMax(blockText.length() - 1, 0));
        const int endCol = qBound(startCol, item.rangeEndColumn,
                                  blockText.length());
        c.setPosition(blockPos + startCol);
        c.setPosition(blockPos + endCol, QTextCursor::KeepAnchor);
    } else {
        // If the cursor is right after a trigger char (".", ":", ">"), do not
        // treat the trigger as a partial identifier — leave it alone.
        int i = posInBlock - 1;
        bool replacePrefix = true;
        if (i >= 0) {
            const QChar prevCh = blockText.at(i);
            if (prevCh == '.' || prevCh == ':' || prevCh == '>') {
                // Insert after the trigger; no prefix to replace.
                replacePrefix = false;
            }
        }
        if (replacePrefix) {
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
        }
    }

    // A function/method/constructor completes with its call parentheses and
    // the caret parked between them, ready for the arguments. An insertion
    // that already carries "(" (snippets do) or an existing "(" right after
    // the caret are left alone.
    const bool callable = item.kind == 2   // Method
                       || item.kind == 3   // Function
                       || item.kind == 4;  // Constructor
    const bool addParens = callable && !insert.contains(QLatin1Char('('))
                           && characterAfter(c) != QLatin1Char('(');
    c.insertText(addParens ? insert + QStringLiteral("()") : insert);
    if (addParens) {
        c.movePosition(QTextCursor::PreviousCharacter);
    }
    setTextCursor(c);
}

// ---------------------------------------------------------------------------
// Code folding
//
// The analysis itself lives in LightCppCore (FoldModel) and knows nothing about
// widgets; everything here is about keeping a fold attached to its block while
// the text moves underneath it.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void CodeEditor::rebuildFoldRegionsOnIdle()
{
    rebuildFoldRegions();
}

void CodeEditor::rebuildFoldRegions()
{
    // See kFoldMaxBlocks. Past the cap the analysis is skipped *and* whatever
    // was folded is released: keeping the previous regions would hide line
    // ranges that the edits have since moved, which is worse than having no
    // folding at all (and the gutter stops offering markers, so nothing looks
    // broken — it simply is not available).
    if (blockCount() > kFoldMaxBlocks) {
        if (!foldRegions_.isEmpty() || !folded_.isEmpty()) {
            foldRegions_.clear();
            folded_.clear();
            applyFoldState();
        }
        return;
    }

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

    // Re-attach the folds that are currently collapsed. The primary key is
    // the anchor's header cursor, whose position adjusts through every edit
    // — its current block number is the header line wherever the text has
    // moved to. The header text is the safety net: if that line now opens
    // something else, the fold it described is gone and is dropped rather
    // than snapping onto an unrelated block.
    QList<FoldAnchor> kept;
    for (FoldAnchor& anchor : folded_) {
        int matched = -1;
        if (anchor.headerCursor.isNull()
            || !anchor.headerCursor.block().isValid()) {
            continue;
        }
        const int headerLine = anchor.headerCursor.blockNumber();
        const QTextBlock headerBlock =
            document()->findBlockByNumber(headerLine);
        if (!headerBlock.isValid() || headerBlock.text() != anchor.headerText) {
            continue;
        }
        for (qsizetype i = 0; i < next.size(); ++i) {
            if (int(next.at(i).startLine) == headerLine) {
                matched = int(i);
                break;
            }
        }
        if (matched < 0) {
            continue;
        }
        anchor.regionIndex = matched;
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
            // A hidden block must occupy no lines, or the document keeps the
            // space and the fold does not look folded.
            //
            // A block that becomes *visible* is different: the number of lines
            // it occupies is whatever the layout produced. With word wrap on
            // that can be several, so pinning it to 1 made an unfolded block
            // claim one line while it drew five, and every block below it sat
            // too high until something forced a full relayout. Leave the count
            // alone there — markContentsDirty below is what makes the layout
            // recompute it, wrapping included. Without wrapping a visible block
            // really is one line, and setting it keeps the gutter arithmetic
            // exact before the layout runs.
            if (!visible) {
                block.setLineCount(0);
            } else if (lineWrapMode() == QPlainTextEdit::NoWrap) {
                block.setLineCount(1);
            }
            // markContentsDirty invalidates the cached layout so the change is
            // actually reflected on screen.
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
        anchor.headerCursor = QTextCursor(header);
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
        anchor.headerCursor = QTextCursor(header);
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
    // Nested folds can stack over the same line; unfolding only the outermost
    // would still leave it hidden, so keep peeling until the line shows.
    while (hiddenLines_.contains(line)) {
        bool peeled = false;
        for (const FoldAnchor& anchor : std::as_const(folded_)) {
            if (anchor.regionIndex < 0 || anchor.regionIndex >= foldRegions_.size()) {
                continue;
            }
            const LightCpp::Core::Fold::FoldRegion& region =
                foldRegions_.at(anchor.regionIndex);
            if (int(region.startLine) < line && line <= int(region.endLine)) {
                setFolded(anchor.regionIndex, false);
                peeled = true;
                break;  // setFolded mutates folded_; restart the scan
            }
        }
        if (!peeled) {
            break;  // nothing left that covers the line
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
        // The guide runs down only as far as the folded range does: it must
        // stop at the first visible block after the region's end, not run to
        // the bottom of the gutter — otherwise, with visible code below the
        // fold, the hairline would cut straight through it.
        int stopY = lineNumberArea_->rect().bottom();
        QTextBlock after = document()->findBlockByNumber(
            int(foldRegions_.at(regionIndex).endLine) + 1);
        while (after.isValid() && !after.isVisible()) {
            after = after.next();  // covered by another (nested) folded range
        }
        if (after.isValid()) {
            const int yAfter = qRound(
                blockBoundingGeometry(after).translated(contentOffset()).top());
            if (yAfter < stopY) {
                stopY = yAfter;  // the fold ends inside the viewport
            }
        }
        if (stopY > y + box + 1) {
            const int cx = x + box / 2;
            painter->setPen(QPen(color, 1));
            painter->drawLine(cx, y + box + 1, cx, stopY);
        }
    }
}

void CodeEditor::paintBlameArea(QPainter* painter, QPaintEvent* event)
{
    const int width = blameAreaWidth();
    if (width <= 0) {
        return;
    }
    // One tint per commit, folded into a translucent wash so the text beside
    // it stays the highest-contrast thing on screen. Twelve hues at 40%
    // separate two authors without turning the gutter into stripes.
    static const QColor kTints[12] = {
        QColor(0x4f, 0x9a, 0xe0), QColor(0x5f, 0xb8, 0x6a), QColor(0xc9, 0x8a, 0x3d),
        QColor(0xb5, 0x6c, 0xc9), QColor(0x4b, 0xb8, 0xb0), QColor(0xd0, 0x6a, 0x6a),
        QColor(0x7d, 0x8a, 0xd6), QColor(0xa8, 0xa8, 0x4f), QColor(0xcf, 0x7f, 0x9e),
        QColor(0x6f, 0xa8, 0x8f), QColor(0x9a, 0x8a, 0x6a), QColor(0x8a, 0x8a, 0x8a),
    };

    painter->save();
    painter->setClipRect(QRect(0, event->rect().top(), width, event->rect().height()));

    const QColor base = palette().color(QPalette::Window);
    const QColor text = palette().color(QPalette::Text);

    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int lineHeight = qMax(bottom - top, 1);

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const int lineNumber = block.blockNumber() + 1;
            const int colourIndex = blame_.colourIndex(lineNumber);
            if (colourIndex >= 0) {
                QColor wash = kTints[colourIndex % 12];
                wash.setAlpha(56);
                painter->fillRect(QRect(0, top, width, lineHeight), wash);
            }
            QColor foreground = text;
            foreground.setAlpha(blame_.line(lineNumber) != nullptr ? 190 : 110);
            painter->setPen(foreground);
            const QString label = blame_.gutterText(lineNumber, blameAuthorWidth_);
            painter->drawText(QRect(0, top, width - 6, lineHeight),
                              Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                              label);
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
    }

    // A hairline between the blame column and the line numbers: without it
    // the two columns read as one ragged string.
    painter->setPen(QPen(base.darker(130), 1));
    painter->drawLine(width - 1, event->rect().top(), width - 1, event->rect().bottom());
    painter->restore();
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent* event)
{
    QPainter painter(lineNumberArea_);
    painter.fillRect(event->rect(), palette().color(QPalette::Window));

    const int blameWidth = blameAreaWidth();
    if (blameWidth > 0) {
        paintBlameArea(&painter, event);
    }

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

            // Breakpoint marker: a filled red dot at the far left of the
            // gutter, left of the fold strip.
            if (breakpointLines_.contains(blockNumber)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(Qt::red));
                painter.drawEllipse(2, top + qMax(0, int((bottom - top) / 2) - 4),
                                    8, 8);
                painter.setBrush(Qt::NoBrush);
            }

            // Bookmark marker: a blue square beside/above the breakpoint
            // dot in the same leftmost slot.
            if (bookmarkLines_.contains(blockNumber)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0x33, 0x66, 0xCC));
                if (breakpointLines_.contains(blockNumber)) {
                    // Sharing the slot with a breakpoint: a smaller square
                    // above the dot keeps both visible.
                    painter.drawRect(3, top + 2, 6, 6);
                } else {
                    painter.drawRect(2, top + qMax(0, int((bottom - top) / 2) - 4),
                                     8, 8);
                }
                painter.setBrush(Qt::NoBrush);
            }

            // Change stripe: a 3px bar at the very left of the gutter, the
            // same place every editor puts it. Painted before the markers so
            // a breakpoint dot still reads on top of it.
            if (blockNumber < lineMarks_.size()) {
                static const QColor kAdded(0x28, 0xA7, 0x45);
                static const QColor kModified(0xE0, 0xA0, 0x00);
                static const QColor kDeleted(0xD0, 0x40, 0x40);
                const int mark = lineMarks_.at(blockNumber);
                QColor colour;
                switch (mark) {
                case 1: colour = kAdded; break;
                case 2: colour = kModified; break;
                case 3: colour = kDeleted; break;
                default: break;
                }
                if (colour.isValid()) {
                    painter.fillRect(0, top, 3, qMax(bottom - top, 1), colour);
                }
            }

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
