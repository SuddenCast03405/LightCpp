// OutputPanel — build / output dock.

#include "LightCpp/App/OutputPanel.hpp"

#include <QPlainTextEdit>

OutputPanel::OutputPanel(QWidget* parent)
    : QDockWidget(tr("Output"), parent)
    , text_(new QPlainTextEdit(this))
{
    text_->setReadOnly(true);
    text_->setMaximumBlockCount(10000);
    text_->setFont(QFont("Cascadia Code", 9));
    setWidget(text_);
    setObjectName("OutputPanel");
}

void OutputPanel::appendOutput(const QString& text)
{
    text_->appendPlainText(text);
}

void OutputPanel::clearOutput()
{
    text_->clear();
}
