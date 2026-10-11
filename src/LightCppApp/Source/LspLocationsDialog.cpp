// LspLocationsDialog — result list for textDocument/references,
// declaration and implementation answers.
#include "LightCpp/App/LspLocationsDialog.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "LightCpp/Text/TextFile.hpp"

namespace {

/// A runaway "find references" on a common symbol must not build a list of
/// thousands of rows — anything past this cap is summarized in the footer.
constexpr int kMaxRows = 500;

/// Trimmed line text shown per row.
constexpr int kMaxRowTextLength = 240;

}  // namespace

LspLocationsDialog::LspLocationsDialog(
    const QString& title,
    const QList<LightCpp::App::LspLocation>& locations, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    resize(820, 520);

    results_ = new QTreeWidget(this);
    results_->setColumnCount(2);
    results_->setHeaderLabels({tr("Location"), tr("Line")});
    results_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    results_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    results_->setRootIsDecorated(false);
    results_->setUniformRowHeights(true);
    results_->setAlternatingRowColors(true);

    status_ = new QLabel(this);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(results_, 1);
    layout->addWidget(status_);

    connect(results_, &QTreeWidget::itemActivated,
            this, &LspLocationsDialog::onLocationActivated);

    int shown = 0;
    for (const LightCpp::App::LspLocation& location : locations) {
        if (shown >= kMaxRows) {
            break;
        }
        // LSP lines are 0-based; every human-facing number here is 1-based.
        auto* item = new QTreeWidgetItem(results_);
        const QString line1based = QString::number(location.line + 1);
        item->setText(0, QDir::toNativeSeparators(location.filePath)
                             + QLatin1Char(':') + line1based);
        item->setText(1, lineText(location.filePath, location.line + 1));
        item->setData(0, Qt::UserRole, location.filePath);
        item->setData(0, Qt::UserRole + 1, location.line + 1);
        ++shown;
    }

    if (locations.isEmpty()) {
        status_->setText(tr("No results."));
    } else if (locations.size() > shown) {
        status_->setText(tr("%1 results (showing the first %2).")
                             .arg(locations.size())
                             .arg(shown));
    } else {
        status_->setText(tr("%1 results.").arg(shown));
    }
}

QString LspLocationsDialog::lineText(const QString& filePath, int line1based)
{
    // Read each file once; reference lists usually cluster in a handful of
    // files. Detection/decoding reuses the same machinery as the editor.
    if (!fileLines_.contains(filePath)) {
        QStringList lines;
        const auto result =
            LightCpp::Text::readDetectedTextFile(filePath);
        if (result.hasValue()) {
            lines = QString::fromUtf8(result.value().utf8)
                        .split(QLatin1Char('\n'));
        }
        fileLines_.insert(filePath, lines);
    }
    const QStringList& lines = fileLines_.value(filePath);
    if (line1based < 1 || line1based > lines.size()) {
        return QString();
    }
    QString text = lines.at(line1based - 1).trimmed();
    if (text.size() > kMaxRowTextLength) {
        text = text.left(kMaxRowTextLength) + QStringLiteral(" …");
    }
    return text;
}

void LspLocationsDialog::onLocationActivated(QTreeWidgetItem* item, int)
{
    if (item == nullptr) {
        return;
    }
    const QString path = item->data(0, Qt::UserRole).toString();
    const int line = item->data(0, Qt::UserRole + 1).toInt();
    if (path.isEmpty() || line < 1) {
        return;
    }
    emit locationActivated(path, line);
}
