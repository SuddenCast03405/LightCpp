// GitBranchDialog — implementation. See the header for the sync/async split.

#include "LightCpp/App/GitBranchDialog.hpp"

#include "LightCpp/App/GitClient.hpp"
#include "LightCpp/App/ToolchainPaths.hpp"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace Git = LightCpp::Workbench::Git;

namespace {

QString gitPath()
{
    return LightCpp::App::ToolchainPaths::gitPath();
}

QTreeWidget* makeTree(const QStringList& headers, QWidget* parent)
{
    auto* tree = new QTreeWidget(parent);
    tree->setColumnCount(static_cast<int>(headers.size()));
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->header()->setStretchLastSection(true);
    return tree;
}

QTreeWidgetItem* addRow(QTreeWidget* tree, const QStringList& columns)
{
    auto* item = new QTreeWidgetItem(tree);
    for (int i = 0; i < columns.size() && i < tree->columnCount(); ++i) {
        item->setText(i, columns.at(i));
    }
    return item;
}

}  // namespace

GitBranchDialog::GitBranchDialog(const QString& workingDirectory, QWidget* parent)
    : QDialog(parent), workingDirectory_(workingDirectory)
{
    setWindowTitle(tr("Git Branches"));
    resize(880, 620);
    git_ = new LightCpp::App::GitClient(this);
    git_->setWorkingDirectory(workingDirectory_);
    git_->setToolPath(gitPath());
    connect(git_, &LightCpp::App::GitClient::commandFinished,
            this, [this](const QString& title, const QString& text, int exitCode) {
                if (!text.trimmed().isEmpty()) {
                    emit message(QStringLiteral("%1 (%2): %3")
                                     .arg(title)
                                     .arg(exitCode)
                                     .arg(text.trimmed().left(500)));
                }
                refreshAll();
            });
    buildUi();
    refreshAll();
}

void GitBranchDialog::setWorkingDirectory(const QString& directory)
{
    workingDirectory_ = directory;
    git_->setWorkingDirectory(directory);
    refreshAll();
}

void GitBranchDialog::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    stateLabel_ = new QLabel(this);
    conflictLabel_ = new QLabel(this);
    conflictLabel_->setWordWrap(true);
    layout->addWidget(stateLabel_);
    layout->addWidget(conflictLabel_);

    auto* stateRow = new QHBoxLayout();
    auto* abortButton = new QPushButton(tr("Abort"), this);
    auto* continueButton = new QPushButton(tr("Continue"), this);
    auto* refreshButton = new QPushButton(tr("Refresh"), this);
    stateRow->addWidget(abortButton);
    stateRow->addWidget(continueButton);
    stateRow->addStretch(1);
    stateRow->addWidget(refreshButton);
    layout->addLayout(stateRow);
    connect(abortButton, &QPushButton::clicked, this, &GitBranchDialog::onAbort);
    connect(continueButton, &QPushButton::clicked, this, &GitBranchDialog::onContinue);
    connect(refreshButton, &QPushButton::clicked, this, &GitBranchDialog::refreshAll);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildBranchesTab(), tr("Branches"));
    tabs_->addTab(buildTagsTab(), tr("Tags"));
    tabs_->addTab(buildRemotesTab(), tr("Remotes"));
    tabs_->addTab(buildStashTab(), tr("Stash"));
    layout->addWidget(tabs_, 1);

    auto* closeRow = new QHBoxLayout();
    auto* closeButton = new QPushButton(tr("Close"), this);
    closeRow->addStretch(1);
    closeRow->addWidget(closeButton);
    layout->addLayout(closeRow);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

QWidget* GitBranchDialog::buildBranchesTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    branchTree_ = makeTree({tr("Branch"), tr("Upstream"), tr("Ahead"), tr("Behind"), tr("Subject")}, page);
    layout->addWidget(branchTree_, 1);

    auto* row = new QHBoxLayout();
    auto* checkoutButton = new QPushButton(tr("Checkout"), page);
    newBranchName_ = new QLineEdit(page);
    newBranchName_->setPlaceholderText(tr("New branch name"));
    auto* createButton = new QPushButton(tr("Create"), page);
    forceDelete_ = new QCheckBox(tr("Force delete"), page);
    auto* deleteButton = new QPushButton(tr("Delete"), page);
    auto* mergeButton = new QPushButton(tr("Merge into current"), page);
    auto* rebaseButton = new QPushButton(tr("Rebase current onto"), page);
    row->addWidget(checkoutButton);
    row->addWidget(newBranchName_, 1);
    row->addWidget(createButton);
    row->addWidget(forceDelete_);
    row->addWidget(deleteButton);
    row->addWidget(mergeButton);
    row->addWidget(rebaseButton);
    layout->addLayout(row);

    connect(checkoutButton, &QPushButton::clicked, this, &GitBranchDialog::onCheckout);
    connect(createButton, &QPushButton::clicked, this, &GitBranchDialog::onCreateBranch);
    connect(deleteButton, &QPushButton::clicked, this, &GitBranchDialog::onDeleteBranch);
    connect(mergeButton, &QPushButton::clicked, this, &GitBranchDialog::onMerge);
    connect(rebaseButton, &QPushButton::clicked, this, &GitBranchDialog::onRebase);
    connect(branchTree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) {
        onCheckout();
    });
    return page;
}

QWidget* GitBranchDialog::buildTagsTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    tagTree_ = makeTree({tr("Tag"), tr("Commit"), tr("Subject")}, page);
    layout->addWidget(tagTree_, 1);

    auto* row = new QHBoxLayout();
    newTagName_ = new QLineEdit(page);
    newTagName_->setPlaceholderText(tr("New tag name"));
    auto* createButton = new QPushButton(tr("Create"), page);
    auto* deleteButton = new QPushButton(tr("Delete"), page);
    row->addWidget(newTagName_, 1);
    row->addWidget(createButton);
    row->addWidget(deleteButton);
    layout->addLayout(row);

    connect(createButton, &QPushButton::clicked, this, &GitBranchDialog::onTagCreate);
    connect(deleteButton, &QPushButton::clicked, this, &GitBranchDialog::onTagDelete);
    return page;
}

QWidget* GitBranchDialog::buildRemotesTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    remoteTree_ = makeTree({tr("Remote"), tr("Fetch"), tr("Push")}, page);
    layout->addWidget(remoteTree_, 1);

    auto* row = new QHBoxLayout();
    remoteName_ = new QLineEdit(page);
    remoteName_->setPlaceholderText(tr("name"));
    remoteUrl_ = new QLineEdit(page);
    remoteUrl_->setPlaceholderText(tr("url"));
    auto* addButton = new QPushButton(tr("Add"), page);
    auto* fetchButton = new QPushButton(tr("Fetch"), page);
    auto* pullButton = new QPushButton(tr("Pull"), page);
    auto* pushButton = new QPushButton(tr("Push"), page);
    row->addWidget(remoteName_, 1);
    row->addWidget(remoteUrl_, 2);
    row->addWidget(addButton);
    row->addWidget(fetchButton);
    row->addWidget(pullButton);
    row->addWidget(pushButton);
    layout->addLayout(row);

    connect(addButton, &QPushButton::clicked, this, &GitBranchDialog::onRemoteAdd);
    connect(fetchButton, &QPushButton::clicked, this, [this] {
        mutate(Git::Commands::fetch(QString(), true), QStringLiteral("git fetch"));
    });
    connect(pullButton, &QPushButton::clicked, this, [this] {
        mutate(Git::Commands::pull(QString(), QString(), false), QStringLiteral("git pull"));
    });
    connect(pushButton, &QPushButton::clicked, this, [this] {
        mutate(Git::Commands::push(QString(), QString(), false, false),
               QStringLiteral("git push"));
    });
    return page;
}

QWidget* GitBranchDialog::buildStashTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    stashTree_ = makeTree({tr("#"), tr("Message"), tr("Branch")}, page);
    layout->addWidget(stashTree_, 1);

    auto* row = new QHBoxLayout();
    auto* pushButton = new QPushButton(tr("Stash current changes"), page);
    auto* popButton = new QPushButton(tr("Pop"), page);
    auto* dropButton = new QPushButton(tr("Drop"), page);
    row->addWidget(pushButton);
    row->addWidget(popButton);
    row->addWidget(dropButton);
    row->addStretch(1);
    layout->addLayout(row);

    connect(pushButton, &QPushButton::clicked, this, &GitBranchDialog::onStashPush);
    connect(popButton, &QPushButton::clicked, this, &GitBranchDialog::onStashPop);
    connect(dropButton, &QPushButton::clicked, this, &GitBranchDialog::onStashDrop);
    return page;
}

QString GitBranchDialog::read(const QStringList& arguments) const
{
    const QString executable = gitPath();
    if (executable.isEmpty()) {
        return {};
    }
    QProcess process;
    process.setWorkingDirectory(workingDirectory_);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(executable, arguments);
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

void GitBranchDialog::mutate(const QStringList& arguments, const QString& title)
{
    git_->run(arguments, title);
}

void GitBranchDialog::refreshAll()
{
    refreshBranches();
    refreshTags();
    refreshRemotes();
    refreshStashes();
    refreshState();
}

void GitBranchDialog::refreshBranches()
{
    branchTree_->clear();
    for (const Git::Branch& branch : Git::parseBranches(
             read({QStringLiteral("branch"), QStringLiteral("-vv"),
                   QStringLiteral("--no-color")}))) {
        auto* item = addRow(branchTree_,
                            {branch.isCurrent ? QStringLiteral("* %1").arg(branch.name)
                                              : branch.name,
                             branch.upstream,
                             branch.ahead > 0 ? QString::number(branch.ahead) : QString(),
                             branch.behind > 0 ? QString::number(branch.behind) : QString(),
                             branch.subject});
        item->setData(0, Qt::UserRole, branch.name);
    }
}

void GitBranchDialog::refreshTags()
{
    tagTree_->clear();
    for (const Git::Tag& tag : Git::parseTags(read(Git::Commands::listTags()))) {
        auto* item = addRow(tagTree_, {tag.name, tag.sha, tag.subject});
        item->setData(0, Qt::UserRole, tag.name);
    }
}

void GitBranchDialog::refreshRemotes()
{
    remoteTree_->clear();
    for (const Git::Remote& remote : Git::parseRemotes(read(Git::Commands::listRemotes()))) {
        auto* item = addRow(remoteTree_, {remote.name, remote.fetchUrl,
                                          remote.pushUrl.isEmpty() ? remote.fetchUrl
                                                                   : remote.pushUrl});
        item->setData(0, Qt::UserRole, remote.name);
    }
}

void GitBranchDialog::refreshStashes()
{
    stashTree_->clear();
    for (const Git::StashEntry& entry : Git::parseStashes(read(Git::Commands::stashList()))) {
        auto* item = addRow(stashTree_, {QString::number(entry.index), entry.message, entry.branch});
        item->setData(0, Qt::UserRole, entry.index);
    }
}

void GitBranchDialog::refreshState()
{
    // The .git directory may be a file (a worktree or a submodule); the
    // state files live in the directory it points at.
    QString gitDir = QDir(workingDirectory_).filePath(QStringLiteral(".git"));
    if (QFileInfo(gitDir).isFile()) {
        QFile pointer(gitDir);
        if (pointer.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString text = QString::fromUtf8(pointer.readAll()).trimmed();
            if (text.startsWith(QLatin1String("gitdir:"))) {
                gitDir = text.mid(7).trimmed();
            }
        }
    }
    QStringList entries = QDir(gitDir).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                 QDir::Name);
    state_ = Git::detectOperationState(entries);
    if (state_ == Git::OperationState::Clean) {
        stateLabel_->setText(tr("No operation in progress."));
    } else {
        stateLabel_->setText(tr("%1 in progress.").arg(Git::operationName(state_)));
    }

    const QList<Git::ConflictFile> conflicts = Git::conflictsFromStatus(
        read({QStringLiteral("status"), QStringLiteral("--porcelain=v1")}));
    if (conflicts.isEmpty()) {
        conflictLabel_->setText(tr("No conflicts."));
        conflictLabel_->setStyleSheet(QString());
    } else {
        QStringList names;
        for (const Git::ConflictFile& file : conflicts) {
            names << file.path;
        }
        conflictLabel_->setText(tr("%1 conflicted file(s): %2")
                                    .arg(conflicts.size())
                                    .arg(names.join(QStringLiteral(", "))));
        conflictLabel_->setStyleSheet(QStringLiteral("color: #c02b2b;"));
    }
}

void GitBranchDialog::onCheckout()
{
    QTreeWidgetItem* item = branchTree_->currentItem();
    if (item == nullptr) {
        return;
    }
    mutate(Git::Commands::checkout(item->data(0, Qt::UserRole).toString()),
           QStringLiteral("git switch"));
}

void GitBranchDialog::onCreateBranch()
{
    const QString name = newBranchName_->text().trimmed();
    if (name.isEmpty()) {
        return;
    }
    QTreeWidgetItem* item = branchTree_->currentItem();
    const QString from = item != nullptr ? item->data(0, Qt::UserRole).toString() : QString();
    mutate(Git::Commands::createAndCheckout(name, from), QStringLiteral("git switch -c"));
    newBranchName_->clear();
}

void GitBranchDialog::onDeleteBranch()
{
    QTreeWidgetItem* item = branchTree_->currentItem();
    if (item == nullptr) {
        return;
    }
    const QString name = item->data(0, Qt::UserRole).toString();
    if (QMessageBox::question(this, tr("Delete branch"),
                              tr("Delete branch \"%1\"?").arg(name))
        != QMessageBox::Yes) {
        return;
    }
    mutate(Git::Commands::deleteBranch(name, forceDelete_->isChecked()),
           QStringLiteral("git branch -d"));
}

void GitBranchDialog::onMerge()
{
    QTreeWidgetItem* item = branchTree_->currentItem();
    if (item == nullptr) {
        return;
    }
    mutate(Git::Commands::merge(item->data(0, Qt::UserRole).toString(), false, false),
           QStringLiteral("git merge"));
}

void GitBranchDialog::onRebase()
{
    QTreeWidgetItem* item = branchTree_->currentItem();
    if (item == nullptr) {
        return;
    }
    mutate(Git::Commands::rebase(item->data(0, Qt::UserRole).toString(), false),
           QStringLiteral("git rebase"));
}

void GitBranchDialog::onTagCreate()
{
    const QString name = newTagName_->text().trimmed();
    if (name.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString message = QInputDialog::getText(
        this, tr("Create tag"), tr("Annotation (empty = lightweight tag):"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok) {
        return;
    }
    mutate(Git::Commands::createTag(name, message, QString()), QStringLiteral("git tag"));
    newTagName_->clear();
}

void GitBranchDialog::onTagDelete()
{
    QTreeWidgetItem* item = tagTree_->currentItem();
    if (item == nullptr) {
        return;
    }
    mutate(Git::Commands::deleteTag(item->data(0, Qt::UserRole).toString(), false),
           QStringLiteral("git tag -d"));
}

void GitBranchDialog::onStashPush()
{
    bool ok = false;
    const QString message = QInputDialog::getText(
        this, tr("Stash"), tr("Message:"), QLineEdit::Normal, QString(), &ok);
    if (!ok) {
        return;
    }
    mutate(Git::Commands::stashPush(message, true), QStringLiteral("git stash push"));
}

void GitBranchDialog::onStashPop()
{
    QTreeWidgetItem* item = stashTree_->currentItem();
    if (item == nullptr) {
        return;
    }
    mutate(Git::Commands::stashPop(item->data(0, Qt::UserRole).toInt()),
           QStringLiteral("git stash pop"));
}

void GitBranchDialog::onStashDrop()
{
    QTreeWidgetItem* item = stashTree_->currentItem();
    if (item == nullptr) {
        return;
    }
    mutate(Git::Commands::stashDrop(item->data(0, Qt::UserRole).toInt()),
           QStringLiteral("git stash drop"));
}

void GitBranchDialog::onRemoteAdd()
{
    const QString name = remoteName_->text().trimmed();
    const QString url = remoteUrl_->text().trimmed();
    if (name.isEmpty() || url.isEmpty()) {
        return;
    }
    mutate(Git::Commands::addRemote(name, url), QStringLiteral("git remote add"));
    remoteName_->clear();
    remoteUrl_->clear();
}

void GitBranchDialog::onAbort()
{
    if (state_ == Git::OperationState::Clean) {
        return;
    }
    mutate(Git::Commands::abortOperation(state_),
           QStringLiteral("git %1 --abort").arg(Git::operationName(state_)));
}

void GitBranchDialog::onContinue()
{
    if (state_ == Git::OperationState::Clean) {
        return;
    }
    mutate(Git::Commands::continueOperation(state_),
           QStringLiteral("git %1 --continue").arg(Git::operationName(state_)));
}
