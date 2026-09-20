#pragma once

#include <QDockWidget>
#include <QPlainTextEdit>

/// Build / output panel dock.  A thin wrapper around QPlainTextEdit
/// providing appendOutput() and clearOutput().
class OutputPanel : public QDockWidget {
    Q_OBJECT
public:
    explicit OutputPanel(QWidget* parent = nullptr);

    void appendOutput(const QString& text);
    void clearOutput();

private:
    QPlainTextEdit* text_;
};
