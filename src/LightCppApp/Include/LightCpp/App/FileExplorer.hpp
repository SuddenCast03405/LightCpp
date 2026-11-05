#pragma once

#include <QFileSystemModel>
#include <QTreeView>
#include <QWidget>
#include <QString>

class QLabel;
class QPoint;

/// File-explorer panel: a filtered QTreeView backed by QFileSystemModel.
///
/// Deliberately a plain QWidget (not a QDockWidget): the panel lives inside
/// the main window's splitter layout and can never be undocked into a
/// floating window of its own.
///
/// Emits fileActivated() when the user double-clicks a file.
class FileExplorer : public QWidget {
    Q_OBJECT
public:
    explicit FileExplorer(QWidget* parent = nullptr);

    void setRootPath(const QString& path);
    [[nodiscard]] QString rootPath() const noexcept { return rootPath_; }

signals:
    void fileActivated(const QString& path);
    /// The user asked to format every C/C++ file under `path` (a folder
    /// picked from the context menu).
    void formatFolderRequested(const QString& path);

private:
    QFileSystemModel* model_;
    QTreeView* tree_;
    QLabel* header_;
    QString rootPath_;

private slots:
    void onDoubleClicked(const QModelIndex& index);
    /// Right-click menu: offers "Format This Folder" on whatever the click
    /// landed on (the folder, a file's parent, or the Explorer root).
    void onContextMenu(const QPoint& pos);
};
