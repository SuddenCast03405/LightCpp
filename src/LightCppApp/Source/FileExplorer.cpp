// FileExplorer — file-tree panel (in-layout widget, not a dock).

#include "LightCpp/App/FileExplorer.hpp"

#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QTreeView>
#include <QVBoxLayout>

FileExplorer::FileExplorer(QWidget* parent)
    : QWidget(parent)
    , model_(new QFileSystemModel(this))
    , tree_(new QTreeView(this))
    , header_(new QLabel(tr("EXPLORER"), this))
{
    model_->setRootPath(QString());
    model_->setFilter(QDir::AllDirs | QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);

    tree_->setModel(model_);
    tree_->setRootIndex(model_->index(QString()));
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setUniformRowHeights(true);
    tree_->setHeaderHidden(false);
    tree_->setAnimated(false);
    tree_->hideColumn(1);  // size
    tree_->hideColumn(2);  // type
    tree_->hideColumn(3);  // date

    // Narrow the name column.
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    // A dock used to provide the caption bar; a plain widget needs its own
    // section header so the panel still reads as a titled area.
    QFont headerFont = header_->font();
    headerFont.setBold(true);
    headerFont.setPointSizeF(headerFont.pointSizeF() * 0.9);
    header_->setFont(headerFont);
    header_->setContentsMargins(6, 4, 6, 4);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header_);
    layout->addWidget(tree_, 1);

    setObjectName(QStringLiteral("FileExplorer"));

    connect(tree_, &QTreeView::doubleClicked,
            this, &FileExplorer::onDoubleClicked);
}

void FileExplorer::setRootPath(const QString& path)
{
    rootPath_ = path;
    model_->setRootPath(path);
    tree_->setRootIndex(model_->index(path));
}

void FileExplorer::onDoubleClicked(const QModelIndex& index)
{
    const QString path = model_->filePath(index);
    if (QFileInfo(path).isFile()) {
        emit fileActivated(path);
    }
}
