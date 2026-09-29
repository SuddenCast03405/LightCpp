#pragma once

#include <QWidget>

class QListWidget;

/// The start page: shown in place of the auto-opened "Untitled" editor tab
/// at launch, listing the recently opened files and projects plus the four
/// ways to begin work.
///
/// It owns no logic — every action is a signal that MainWindow routes, so
/// the page stays a dumb view over whatever the settings recorded. It lives
/// inside the editor tab area (one removable tab), not in a dialog of its
/// own.
class StartupPage : public QWidget {
    Q_OBJECT
public:
    /// `recentFiles` is the persisted list, most recent first; entries that
    /// no longer exist on disk are filtered out of the view.
    explicit StartupPage(const QStringList& recentFiles,
                         QWidget* parent = nullptr);

signals:
    /// A recent entry was activated. The path is absolute; the receiver
    /// decides whether it is a plain file or a project descriptor.
    void recentActivated(const QString& path);
    void newFileRequested();
    void openFileRequested();
    void openProjectRequested();
    void openFolderRequested();

private:
    QListWidget* recentList_;
};
