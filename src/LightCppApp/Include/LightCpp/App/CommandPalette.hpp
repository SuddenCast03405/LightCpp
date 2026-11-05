// CommandPalette — Ctrl+Shift+P over every action the window owns.
//
// lightcpp.1.md §九 names "命令面板" and the 0.2.0 review found 76 menu
// actions with no way to reach them except by remembering the menu. The
// palette searches *actions*, not commands invented for it: whatever has a
// text, a shortcut and a trigger is findable, including the ones a plugin
// registered.
//
// The matcher is deliberately a subsequence score rather than a regex: typing
// "fld" should find "Format Document", and typing "fd" should rank it above
// "Find in Files". `fuzzyScore()` is a free function so the ranking is
// testable without showing a dialog.

#pragma once

#include <QDialog>
#include <QList>
#include <QString>

class QLineEdit;
class QListWidget;
class QAction;

/// -1 when `needle` is not a subsequence of `haystack`; otherwise a score
/// where higher is better. Consecutive matches, word starts and a short
/// haystack all score higher.
[[nodiscard]] int fuzzyScore(const QString& needle, const QString& haystack);

class CommandPalette : public QDialog {
    Q_OBJECT
public:
    /// `actions` is not owned; the palette reads text/shortcut and triggers
    /// the chosen one. Entries without text are ignored.
    explicit CommandPalette(const QList<QAction*>& actions, QWidget* parent = nullptr);

    /// The action the user chose, or nullptr.
    [[nodiscard]] QAction* chosenAction() const { return chosen_; }

private:
    void refresh(const QString& needle);
    void acceptCurrent();

    struct Entry {
        QAction* action = nullptr;
        QString label;
        QString shortcut;
        int score = 0;
    };

    QLineEdit* input_ = nullptr;
    QListWidget* list_ = nullptr;
    QList<Entry> entries_;
    QAction* chosen_ = nullptr;
};
