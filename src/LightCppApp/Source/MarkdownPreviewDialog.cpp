// MarkdownPreviewDialog — implementation.

#include "LightCpp/App/MarkdownPreviewDialog.hpp"

#include "LightCpp/Workbench/MarkupRender.hpp"

#include <QFileInfo>
#include <QTextBrowser>
#include <QVBoxLayout>

MarkdownPreviewDialog::MarkdownPreviewDialog(const QString& filePath,
                                             const QString& text,
                                             bool darkBackground, QWidget* parent)
    : QDialog(parent), filePath_(filePath), text_(text), dark_(darkBackground)
{
    setWindowTitle(tr("Preview: %1").arg(QFileInfo(filePath).fileName()));
    resize(820, 680);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    browser_ = new QTextBrowser(this);
    browser_->setOpenExternalLinks(true);
    layout->addWidget(browser_);
    render();
}

void MarkdownPreviewDialog::setText(const QString& text)
{
    text_ = text;
    render();
}

void MarkdownPreviewDialog::render()
{
    const QString body = LightCpp::Workbench::renderForFile(filePath_, text_);
    // QTextBrowser takes a CSS subset through setDefaultStyleSheet; the body
    // must be wrapped in a document for the rules to apply.
    browser_->document()->setDefaultStyleSheet(
        LightCpp::Workbench::previewStyleSheet(dark_));
    browser_->setHtml(QStringLiteral("<html><body>%1</body></html>").arg(body));
}
