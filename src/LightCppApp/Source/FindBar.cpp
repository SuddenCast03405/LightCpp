#include "LightCpp/App/FindBar.hpp"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>

#include "LightCpp/App/CodeEditor.hpp"
#include "LightCpp/App/FindScan.hpp"

namespace {

constexpr int kCountCap = 9999;  // matches shown before "9999+"

// This file is in the global namespace (App-layer widget convention), so the
// shared resume decision has to be named: it lives in LightCpp::App::FindScan
// and is what keeps the three scans below from walking off the same spot.
namespace FindScan = LightCpp::App::FindScan;

}  // namespace

FindBar::FindBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("findBar"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    // --- find row: field + options + prev/next ----------------------------
    findEdit_ = new QLineEdit(this);
    findEdit_->setPlaceholderText(tr("Find"));
    findEdit_->setClearButtonEnabled(true);
    findEdit_->installEventFilter(this);
    layout->addWidget(findEdit_, 3);

    regexCheck_ = new QCheckBox(tr(".*"), this);
    regexCheck_->setToolTip(tr("Treat the pattern as a regular expression"));
    layout->addWidget(regexCheck_);

    caseCheck_ = new QCheckBox(tr("Aa"), this);
    caseCheck_->setToolTip(tr("Match case"));
    layout->addWidget(caseCheck_);

    wordCheck_ = new QCheckBox(tr("W"), this);
    wordCheck_->setToolTip(tr("Whole words only"));
    layout->addWidget(wordCheck_);

    auto* prevBtn = new QPushButton(tr("Previous"), this);
    auto* nextBtn = new QPushButton(tr("Next"), this);
    layout->addWidget(prevBtn);
    layout->addWidget(nextBtn);

    // --- replace row: field + buttons -------------------------------------
    replaceEdit_ = new QLineEdit(this);
    replaceEdit_->setPlaceholderText(tr("Replace with"));
    replaceEdit_->installEventFilter(this);
    layout->addWidget(replaceEdit_, 2);

    replaceBtn_ = new QPushButton(tr("Replace"), this);
    replaceAllBtn_ = new QPushButton(tr("Replace All"), this);
    layout->addWidget(replaceBtn_);
    layout->addWidget(replaceAllBtn_);

    statusLabel_ = new QLabel(this);
    statusLabel_->setMinimumWidth(70);

    auto* closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setToolTip(tr("Close (Esc)"));
    layout->addWidget(statusLabel_, 0);
    layout->addWidget(closeBtn);

    // --- behaviour ---------------------------------------------------------
    connect(findEdit_, &QLineEdit::returnPressed, this, &FindBar::findNext);
    connect(findEdit_, &QLineEdit::textChanged,
            this, &FindBar::updateMatchCount);
    for (QCheckBox* box : {regexCheck_, caseCheck_, wordCheck_}) {
        connect(box, &QCheckBox::toggled, this, &FindBar::updateMatchCount);
    }
    connect(prevBtn, &QPushButton::clicked, this, &FindBar::findPrev);
    connect(nextBtn, &QPushButton::clicked, this, &FindBar::findNext);
    connect(replaceBtn_, &QPushButton::clicked, this, &FindBar::replaceOne);
    connect(replaceAllBtn_, &QPushButton::clicked, this, &FindBar::replaceAll);
    connect(closeBtn, &QPushButton::clicked, this, &FindBar::closeBar);

    hide();
}

void FindBar::openFor(CodeEditor* editor, bool replaceMode)
{
    editor_ = editor;
    lastZeroWidthAt_ = -1;
    if (editor != nullptr) {
        // Preload a single-line selection: that is what "search for this"
        // means, and a multi-line blob would only make the field unreadable.
        const QString selected = editor->textCursor().selectedText();
        if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator)) {
            findEdit_->setText(selected);
        }
        findEdit_->selectAll();
    }
    replaceEdit_->setVisible(replaceMode);
    replaceBtn_->setVisible(replaceMode);
    replaceAllBtn_->setVisible(replaceMode);
    show();
    raise();
    findEdit_->setFocus();
    updateMatchCount();
}

void FindBar::setEditor(CodeEditor* editor)
{
    editor_ = editor;
    lastZeroWidthAt_ = -1;  // a different document: the old position means nothing
    if (isVisible()) {
        updateMatchCount();
    }
}

bool FindBar::eventFilter(QObject* watched, QEvent* event)
{
    // Esc closes from anywhere inside the bar, and Shift+Enter in the find
    // field searches backwards — Enter alone already goes forward via
    // returnPressed().
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            closeBar();
            return true;
        }
        if (watched == findEdit_ && key->key() == Qt::Key_Return
            && key->modifiers() & Qt::ShiftModifier) {
            findPrev();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QTextDocument::FindFlags FindBar::textFlags(bool backward) const
{
    QTextDocument::FindFlags flags;
    if (backward) {
        flags |= QTextDocument::FindBackward;
    }
    if (caseCheck_->isChecked()) {
        flags |= QTextDocument::FindCaseSensitively;
    }
    if (wordCheck_->isChecked()) {
        flags |= QTextDocument::FindWholeWords;
    }
    return flags;
}

QRegularExpression FindBar::regexPattern() const
{
    // Case sensitivity and whole words are baked into the pattern here:
    // QTextDocument::find only honours FindBackward for a regex, the rest
    // would silently be ignored.
    QRegularExpression::PatternOptions options
        = QRegularExpression::NoPatternOption;
    if (!caseCheck_->isChecked()) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    QString pattern = findEdit_->text();
    if (wordCheck_->isChecked() && !pattern.isEmpty()) {
        pattern = QStringLiteral("\\b(?:%1)\\b").arg(pattern);
    }
    return QRegularExpression(pattern, options);
}

QString FindBar::expandReplacement(const QStringList& captured,
                                   const QString& replacement) const
{
    QString out;
    out.reserve(replacement.size() * 2);
    for (int i = 0; i < replacement.size(); ++i) {
        const QChar c = replacement.at(i);
        if ((c == u'\\' || c == u'$') && i + 1 < replacement.size()) {
            const QChar next = replacement.at(i + 1);
            if (next.isDigit()) {
                const int group = next.digitValue();
                if (group < captured.size()) {
                    out += captured.at(group);
                }
                ++i;  // consume the digit; a group beyond the match is dropped
                continue;
            }
            if (c == u'\\' && (next == u'\\' || next == u'$')) {
                out += next;  // an escaped separator survives as a literal
                ++i;
                continue;
            }
        }
        out += c;
    }
    return out;
}

void FindBar::findNext()
{
    find(false);
}

void FindBar::find(bool backward)
{
    if (editor_.isNull() || findEdit_->text().isEmpty()) {
        return;
    }

    const QRegularExpression re = regexPattern();
    if (regexCheck_->isChecked() && !re.isValid()) {
        statusLabel_->setStyleSheet(QStringLiteral("color: red;"));
        statusLabel_->setText(tr("bad regex"));
        return;
    }
    const QTextDocument::FindFlags flags = backward
        ? textFlags(true) & QTextDocument::FindBackward  // case/words live in the regex
        : textFlags(false);
    auto search = [&](const QTextCursor& start) {
        return regexCheck_->isChecked()
            ? editor_->document()->find(re, start, flags)
            : editor_->document()->find(findEdit_->text(), start, textFlags(backward));
    };

    const int docEnd = editor_->document()->characterCount() - 1;

    // Start just past (or before) the current selection so pressing Next
    // repeatedly walks through the matches instead of sitting on one. A
    // zero-width match selects nothing, so after landing on one the caret is
    // still AT it: step over it, or this press finds the very same spot and
    // Next looks broken.
    QTextCursor from = editor_->textCursor();
    bool wrapOnly = false;
    if (from.hasSelection()) {
        from.setPosition(backward ? from.selectionStart() : from.selectionEnd());
    } else if (from.position() == lastZeroWidthAt_) {
        const int resume
            = FindScan::resumeAfter(from.position(), 0, docEnd, backward);
        if (resume < 0) {
            wrapOnly = true;  // nothing left in that direction: wrap
        } else {
            from.setPosition(resume);
        }
    }

    QTextCursor found = wrapOnly ? QTextCursor() : search(from);
    if (found.isNull()) {
        // Wrap around: first/last match in the document, so Next from the
        // last match lands on the first one again.
        QTextCursor wrap(editor_->document());
        wrap.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        found = search(wrap);
    }
    if (found.isNull()) {
        lastZeroWidthAt_ = -1;
        statusLabel_->setStyleSheet(QStringLiteral("color: red;"));
        statusLabel_->setText(tr("not found"));
        return;
    }
    lastZeroWidthAt_ = found.selectionEnd() == found.selectionStart()
                           ? found.selectionStart()
                           : -1;
    editor_->setTextCursor(found);
    editor_->centerCursor();
    updateMatchCount();
}

void FindBar::findPrev()
{
    find(true);
}

void FindBar::replaceOne()
{
    if (editor_.isNull() || findEdit_->text().isEmpty()) {
        return;
    }

    // If the caret sits on a match, replace it in place; otherwise jump to
    // the next match first and replace that one.
    QTextCursor cur = editor_->textCursor();
    if (!cur.hasSelection() || !selectionMatches(cur)) {
        find(false);
        cur = editor_->textCursor();
        if (!cur.hasSelection()) {
            return;  // no match anywhere (find() already said so)
        }
    }
    QString replacement = replaceEdit_->text();
    if (regexCheck_->isChecked()) {
        replacement = expandReplacement(
            regexPattern().match(cur.selectedText()).capturedTexts(),
            replacement);
    }
    cur.beginEditBlock();
    cur.removeSelectedText();
    cur.insertText(replacement);
    cur.endEditBlock();
    find(false);
    updateMatchCount();
}

void FindBar::replaceAll()
{
    if (editor_.isNull() || findEdit_->text().isEmpty()) {
        return;
    }

    const QRegularExpression re = regexPattern();
    if (regexCheck_->isChecked() && !re.isValid()) {
        statusLabel_->setStyleSheet(QStringLiteral("color: red;"));
        statusLabel_->setText(tr("bad regex"));
        return;
    }

    // Collect every range first, then apply the edits back to front: writing
    // into the document while searching would let a replacement that itself
    // matches the pattern grow without end ("a" -> "aa").
    struct Planned { int start = 0; int length = 0; QString text; };
    QList<Planned> plan;
    QTextCursor it(editor_->document());
    const int docEnd = editor_->document()->characterCount() - 1;
    while (plan.size() <= kCountCap) {
        it = regexCheck_->isChecked()
                 ? editor_->document()->find(re, it)
                 : editor_->document()->find(findEdit_->text(),
                                             it, textFlags(false));
        if (it.isNull()) {
            break;
        }
        Planned entry;
        entry.start = it.selectionStart();
        entry.length = it.selectionEnd() - it.selectionStart();
        if (regexCheck_->isChecked()) {
            entry.text = expandReplacement(
                regexPattern().match(it.selectedText()).capturedTexts(),
                replaceEdit_->text());
        } else {
            entry.text = replaceEdit_->text();
        }
        plan.append(entry);
        // A zero-width match ("x*", "a|", "^") has nothing to move past:
        // without the step below this same spot is collected again and again
        // until the cap — then the apply pass inserts the replacement there
        // thousands of times and the document blows up.
        const int resume = FindScan::resumeAfter(entry.start, entry.length,
                                                 docEnd);
        if (resume < 0) {
            break;
        }
        it.setPosition(resume);
    }
    if (plan.isEmpty()) {
        statusLabel_->setStyleSheet(QStringLiteral("color: red;"));
        statusLabel_->setText(tr("not found"));
        return;
    }

    QTextCursor edit(editor_->document());
    edit.beginEditBlock();
    for (int i = plan.size() - 1; i >= 0; --i) {
        edit.setPosition(plan.at(i).start);
        edit.setPosition(plan.at(i).start + plan.at(i).length,
                         QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        edit.insertText(plan.at(i).text);
    }
    edit.endEditBlock();

    // One undo step puts the whole file back.
    QTextCursor after(editor_->document());
    after.setPosition(plan.constLast().start
                      + plan.constLast().text.length());
    editor_->setTextCursor(after);
    // Re-count FIRST, then report: "N replaced" is the answer to what the
    // user just asked for, and updateMatchCount() would otherwise overwrite
    // it — for a literal replacement (the common case) with a red "not
    // found", because the pattern no longer matches anything afterwards.
    updateMatchCount();
    statusLabel_->setStyleSheet(QString());
    statusLabel_->setText(tr("%1 replaced").arg(plan.size()));
}

bool FindBar::selectionMatches(const QTextCursor& cur) const
{
    const QString selected = cur.selectedText();
    if (selected.isEmpty() || editor_.isNull()) {
        return false;
    }
    bool isMatch = false;
    if (regexCheck_->isChecked()) {
        const QRegularExpressionMatch match = regexPattern().match(selected);
        isMatch = match.hasMatch() && match.capturedStart() == 0
                  && match.capturedLength() == selected.length();
    } else {
        const Qt::CaseSensitivity cs = caseCheck_->isChecked()
            ? Qt::CaseSensitive : Qt::CaseInsensitive;
        isMatch = selected.compare(findEdit_->text(), cs) == 0;
        if (isMatch && wordCheck_->isChecked()) {
            // Whole words: the neighbours must not be identifier characters.
            QTextDocument* doc = editor_->document();
            const QChar before = cur.selectionStart() > 0
                ? doc->characterAt(cur.selectionStart() - 1) : QChar();
            const QChar after = doc->characterAt(cur.selectionEnd());
            isMatch = !(before.isLetterOrNumber() || before == u'_')
                      && !(after.isLetterOrNumber() || after == u'_');
        }
    }
    return isMatch;
}

void FindBar::updateMatchCount()
{
    if (editor_.isNull()) {
        return;
    }
    if (findEdit_->text().isEmpty()) {
        statusLabel_->clear();
        return;
    }

    const QRegularExpression re = regexPattern();
    if (regexCheck_->isChecked() && !re.isValid()) {
        statusLabel_->setStyleSheet(QStringLiteral("color: red;"));
        statusLabel_->setText(tr("bad regex"));
        return;
    }

    // Count from the top so the number does not depend on the caret.
    int count = 0;
    QTextCursor it(editor_->document());
    const int docEnd = editor_->document()->characterCount() - 1;
    while (count <= kCountCap) {
        it = regexCheck_->isChecked()
                 ? editor_->document()->find(re, it)
                 : editor_->document()->find(findEdit_->text(),
                                             it, textFlags(false));
        if (it.isNull()) {
            break;
        }
        ++count;
        // Same step as the other two scans: a zero-width match leaves the
        // cursor in place, and re-finding it would count the same position
        // until the cap and report "9999+" for a pattern like "^".
        const int resume = FindScan::resumeAfter(
            it.selectionStart(), it.selectionEnd() - it.selectionStart(),
            docEnd);
        if (resume < 0) {
            break;
        }
        it.setPosition(resume);
    }
    if (count == 0) {
        statusLabel_->setStyleSheet(QStringLiteral("color: red;"));
        statusLabel_->setText(tr("not found"));
        return;
    }
    statusLabel_->setStyleSheet(QString());
    statusLabel_->setText(count > kCountCap
                              ? tr("%1+").arg(kCountCap)
                              : tr("%1 matches").arg(count));
}

void FindBar::closeBar()
{
    hide();
    if (!editor_.isNull()) {
        editor_->setFocus();
    }
    emit closed();
}

// Every search goes through QTextDocument::find, so text hidden inside a
// folded region never matches and never gets replaced.
