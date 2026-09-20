#pragma once

#include <QDockWidget>
#include <QFileSystemModel>
#include <QTreeView>
#include <QString>

/// File-explorer panel: a filtered QTreeView backed by QFileSystemModel.
/// Emits fileActivated() when the user double-clicks a file.
class FileExplorer : public QDockWidget {
    Q_OBJECT
public:
    explicit FileExplorer(QWidget* parent = nullptr);

    void setRootPath(const QString& path);
    [[nodiscard]] QString rootPath() const noexcept { return rootPath_; }

signals:
    void fileActivated(const QString& path);

private:
    QFileSystemModel* model_;
    QTreeView* tree_;
    QString rootPath_;

private slots:
    void onDoubleClicked(const QModelIndex& index);
};
