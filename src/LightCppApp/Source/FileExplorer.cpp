// FileExplorer — file-tree panel.

#include "LightCpp/App/FileExplorer.hpp"

#include <QHeaderView>
#include <QTreeView>

FileExplorer::FileExplorer(QWidget* parent)
    : QDockWidget(tr("Explorer"), parent)
    , model_(new QFileSystemModel(this))
    , tree_(new QTreeView(this))
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

    setWidget(tree_);
    setObjectName("FileExplorer");

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
