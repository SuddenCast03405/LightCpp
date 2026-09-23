#pragma once

#include <QPlainTextEdit>
#include <QWidget>

class QLabel;
class QToolButton;

/// Build / output panel.  A thin wrapper around QPlainTextEdit providing
/// appendOutput() and clearOutput().
///
/// Deliberately a plain QWidget (not a QDockWidget): the panel lives inside
/// the main window's splitter layout and can never be undocked into a
/// floating window of its own.
class OutputPanel : public QWidget {
    Q_OBJECT
public:
    explicit OutputPanel(QWidget* parent = nullptr);

    void appendOutput(const QString& text);
    void clearOutput();

private:
    QLabel* header_;
    QToolButton* clearButton_;
    QPlainTextEdit* text_;
};
