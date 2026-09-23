// OutputPanel — build / output panel (in-layout widget, not a dock).

#include "LightCpp/App/OutputPanel.hpp"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

OutputPanel::OutputPanel(QWidget* parent)
    : QWidget(parent)
    , header_(new QLabel(tr("OUTPUT"), this))
    , clearButton_(new QToolButton(this))
    , text_(new QPlainTextEdit(this))
{
    text_->setReadOnly(true);
    text_->setMaximumBlockCount(10000);
    text_->setFont(QFont(QStringLiteral("Cascadia Code"), 9));

    // A dock used to provide the caption bar; a plain widget needs its own
    // section header (plus a way to wipe the log, which the dock's context
    // menu used to offer nowhere else).
    QFont headerFont = header_->font();
    headerFont.setBold(true);
    headerFont.setPointSizeF(headerFont.pointSizeF() * 0.9);
    header_->setFont(headerFont);
    header_->setContentsMargins(6, 2, 6, 2);

    clearButton_->setText(tr("Clear"));
    clearButton_->setAutoRaise(true);
    clearButton_->setToolTip(tr("Clear the output log"));
    clearButton_->setFocusPolicy(Qt::NoFocus);
    connect(clearButton_, &QToolButton::clicked, this, &OutputPanel::clearOutput);

    auto* headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 4, 0);
    headerRow->setSpacing(4);
    headerRow->addWidget(header_);
    headerRow->addStretch(1);
    headerRow->addWidget(clearButton_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(headerRow);
    layout->addWidget(text_, 1);

    setObjectName(QStringLiteral("OutputPanel"));
}

void OutputPanel::appendOutput(const QString& text)
{
    text_->appendPlainText(text);
}

void OutputPanel::clearOutput()
{
    text_->clear();
}
