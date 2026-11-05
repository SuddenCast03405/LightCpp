// MarkdownPreviewDialog — the §十一 preview, on QTextBrowser.
//
// A dialog rather than a tab: the preview is a *reading* view of the file the
// editor already has open, and a tab that could drift from the buffer would
// be a second copy to keep in sync. Re-opening it re-renders.
//
// The renderer is in LightCppWorkbench (Core-only, tested), the stylesheet is
// there too so the light/dark decision is one function.

#pragma once

#include <QDialog>

class QTextBrowser;

class MarkdownPreviewDialog : public QDialog {
    Q_OBJECT
public:
    MarkdownPreviewDialog(const QString& filePath, const QString& text,
                          bool darkBackground, QWidget* parent = nullptr);

    /// Re-render `text` for the same file (used after a save).
    void setText(const QString& text);

private:
    void render();

    QString filePath_;
    QString text_;
    bool dark_ = false;
    QTextBrowser* browser_ = nullptr;
};
