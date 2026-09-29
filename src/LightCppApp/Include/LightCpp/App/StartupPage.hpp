#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
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
    /// One environment check. "requiresFile" rows are OK when `detail`
    /// names an existing file (a compiler path); the other kind is a plain
    /// failure whose `detail` is the reason ("toolchain 'x' not found").
    struct StartupCheckItem {
        QString label;
        QString detail;
        bool requiresFile = true;
    };

    /// `recentFiles` is the persisted list, most recent first; entries that
    /// no longer exist on disk are filtered out of the view.
    explicit StartupPage(const QStringList& recentFiles,
                         QWidget* parent = nullptr);

    /// Queue the environment checks (toolchains + kits). One item runs per
    /// event-loop turn, so the page keeps repainting and the progress line
    /// advances step by step. Emits checksFinished() when the last one is
    /// done. Not called — the "Toolchains & Kits" group simply stays on its
    /// "Not checked." line.
    void runChecks(const QList<StartupCheckItem>& items);

signals:
    /// A recent entry was activated. The path is absolute; the receiver
    /// decides whether it is a plain file or a project descriptor.
    void recentActivated(const QString& path);
    void newFileRequested();
    void openFileRequested();
    void openProjectRequested();
    void openFolderRequested();
    /// The queued checks are all done. `failed` counts the rows that
    /// reported a problem.
    void checksFinished(int total, int failed);

private slots:
    /// Verify the next queued item and schedule the one after it.
    void runNextCheck();

private:
    /// The progress line's text for the current queue position.
    void updateCheckProgress();

    QListWidget* recentList_;
    QLabel* checkStatus_;
    QListWidget* checkList_;
    QList<StartupCheckItem> pendingChecks_;
    int totalChecks_ = 0;
    int failedChecks_ = 0;
};
