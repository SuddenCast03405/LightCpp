#pragma once

#include <QPointer>
#include <QTextCursor>
#include <QTextDocument>
#include <QWidget>

class QCheckBox;
class CodeEditor;
class QLabel;
class QLineEdit;
class QPushButton;
class QRegularExpression;

/// The find/replace bar docked under the editor tabs. Covers the whole
/// find-and-replace feature set for the current file: next/previous match,
/// replace, replace all, regular expressions, case sensitivity and whole
/// words. Literal search is the default; the options are session state and
/// are not persisted.
class FindBar : public QWidget {
    Q_OBJECT

public:
    explicit FindBar(QWidget* parent = nullptr);

    /// Show the bar and aim it at `editor`. In replace mode the replace row
    /// is visible too; otherwise only the find row. The find field starts
    /// with the editor's current selection, if it is one line long.
    void openFor(CodeEditor* editor, bool replaceMode);
    /// Follow a tab switch while the bar stays open.
    void setEditor(CodeEditor* editor);

    /// Search once from the caret, wrapping around at the document edge.
    /// The current selection counts as "already seen", so Find Next walks
    /// forward instead of re-selecting the same match.
    void find(bool backward);
    void findNext();
    void findPrev();

signals:
    void closed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void replaceOne();
    void replaceAll();
    void updateMatchCount();
    void closeBar();

    [[nodiscard]] QTextDocument::FindFlags textFlags(bool backward) const;
    [[nodiscard]] QRegularExpression regexPattern() const;
    /// Expand \1..\9 and $1..$9 (and \0/$0) in the replacement against the
    /// match of a regular-expression search; literal otherwise.
    [[nodiscard]] QString expandReplacement(
        const QStringList& captured, const QString& replacement) const;
    /// True when the selection under `cur` is exactly one match of the
    /// current pattern — lets Replace act on it instead of jumping first.
    [[nodiscard]] bool selectionMatches(const QTextCursor& cur) const;

    QPointer<CodeEditor> editor_;
    QLineEdit* findEdit_ = nullptr;
    QLineEdit* replaceEdit_ = nullptr;
    QCheckBox* regexCheck_ = nullptr;
    QCheckBox* caseCheck_ = nullptr;
    QCheckBox* wordCheck_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* replaceBtn_ = nullptr;
    QPushButton* replaceAllBtn_ = nullptr;
};
