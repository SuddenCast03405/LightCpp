// GitBranchDialog — the §八 branch/tag/remote/stash view.
//
// A modeless dialog with four tabs and a conflict banner. It runs git itself
// (synchronously, with a short timeout) rather than through GitClient,
// because every view here needs the *answer* parsed, and GitClient's contract
// is "queue a command, print the transcript". The mutating operations still
// go through GitClient so one process at a time owns the index — the dialog
// asks for a refresh once the command reports back.
//
// The synchronous reads are the ones git answers in milliseconds (branch -vv,
// tag, remote -v, stash list); the 5 s ceiling is a guard against a wedged
// repository, not a performance target.

#pragma once

#include <QDialog>

#include "LightCpp/Workbench/GitModel.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace LightCpp::App {
class GitClient;
}

class GitBranchDialog : public QDialog {
    Q_OBJECT
public:
    explicit GitBranchDialog(const QString& workingDirectory, QWidget* parent = nullptr);

    /// Point the dialog at a repository (the window's current project root).
    void setWorkingDirectory(const QString& directory);

signals:
    void message(const QString& text);

private:
    void buildUi();
    QWidget* buildBranchesTab();
    QWidget* buildTagsTab();
    QWidget* buildRemotesTab();
    QWidget* buildStashTab();

    /// Run a read-only git command and return its stdout, or empty on error.
    [[nodiscard]] QString read(const QStringList& arguments) const;
    /// Ask GitClient to run a mutating command; the dialog refreshes when the
    /// command finishes.
    void mutate(const QStringList& arguments, const QString& title);

    void refreshAll();
    void refreshBranches();
    void refreshTags();
    void refreshRemotes();
    void refreshStashes();
    void refreshState();

    void onCheckout();
    void onCreateBranch();
    void onDeleteBranch();
    void onMerge();
    void onRebase();
    void onTagCreate();
    void onTagDelete();
    void onStashPush();
    void onStashPop();
    void onStashDrop();
    void onRemoteAdd();
    void onAbort();
    void onContinue();

    QString workingDirectory_;
    LightCpp::App::GitClient* git_ = nullptr;

    QTabWidget* tabs_ = nullptr;
    QTreeWidget* branchTree_ = nullptr;
    QTreeWidget* tagTree_ = nullptr;
    QTreeWidget* remoteTree_ = nullptr;
    QTreeWidget* stashTree_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* conflictLabel_ = nullptr;
    QLineEdit* newBranchName_ = nullptr;
    QLineEdit* newTagName_ = nullptr;
    QLineEdit* remoteName_ = nullptr;
    QLineEdit* remoteUrl_ = nullptr;
    QCheckBox* forceDelete_ = nullptr;

    LightCpp::Workbench::Git::OperationState state_ =
        LightCpp::Workbench::Git::OperationState::Clean;
};
