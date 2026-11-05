// StartupPage — the start page shown instead of an auto-opened "Untitled"
// editor tab (recent files, quick actions, and the toolchain/kit checks).

#include "LightCpp/App/StartupPage.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

StartupPage::StartupPage(const QStringList& recentFiles, QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(48, 40, 48, 32);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("LightC++"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 10);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto* subtitle = new QLabel(tr("A lightweight C/C++ IDE"), this);
    layout->addWidget(subtitle);
    layout->addSpacing(16);

    // The four ways to begin, one click each. MainWindow closes the start
    // page before acting, so the thing the user asked for ends up in front.
    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    auto* newFileButton = new QPushButton(tr("New File"), this);
    auto* openFileButton = new QPushButton(tr("Open File..."), this);
    auto* openProjectButton = new QPushButton(tr("Open Project..."), this);
    auto* openFolderButton = new QPushButton(tr("Open Folder..."), this);
    actions->addWidget(newFileButton);
    actions->addWidget(openFileButton);
    actions->addWidget(openProjectButton);
    actions->addWidget(openFolderButton);
    actions->addStretch(1);
    layout->addLayout(actions);

    connect(newFileButton, &QPushButton::clicked, this,
            &StartupPage::newFileRequested);
    connect(openFileButton, &QPushButton::clicked, this,
            &StartupPage::openFileRequested);
    connect(openProjectButton, &QPushButton::clicked, this,
            &StartupPage::openProjectRequested);
    connect(openFolderButton, &QPushButton::clicked, this,
            &StartupPage::openFolderRequested);

    auto* recentHeader = new QLabel(tr("Recent"), this);
    QFont headerFont = recentHeader->font();
    headerFont.setBold(true);
    recentHeader->setFont(headerFont);
    layout->addWidget(recentHeader);

    recentList_ = new QListWidget(this);
    recentList_->setObjectName(QStringLiteral("startupRecentList"));
    recentList_->setSelectionMode(QAbstractItemView::SingleSelection);
    recentList_->setFrameShape(QFrame::StyledPanel);
    for (const QString& path : recentFiles) {
        const QFileInfo info(path);
        if (!info.isFile()) {
            continue;  // a stale entry, gone from disk since it was recorded
        }
        // Name first — project descriptors (CMakeLists.txt, ...) share a
        // file name across projects, so the directory disambiguates.
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  \u2014  %2")
                .arg(info.fileName(), info.absolutePath()),
            recentList_);
        item->setToolTip(QDir::toNativeSeparators(info.absoluteFilePath()));
        item->setData(Qt::UserRole, info.absoluteFilePath());
    }
    if (recentList_->count() == 0) {
        auto* empty = new QListWidgetItem(tr("No recent files yet"),
                                          recentList_);
        empty->setFlags(Qt::NoItemFlags);  // a placeholder, not an entry
    }
    layout->addWidget(recentList_, /*stretch*/ 1);

    connect(recentList_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) {
                if (item == nullptr) {
                    return;
                }
                const QString path = item->data(Qt::UserRole).toString();
                if (!path.isEmpty()) {
                    emit recentActivated(path);
                }
            });

    // --- toolchain / kit checks ------------------------------------------
    auto* checkHeader = new QLabel(tr("Toolchains & Kits"), this);
    checkHeader->setFont(headerFont);
    layout->addWidget(checkHeader);

    checkStatus_ = new QLabel(tr("Not checked."), this);
    checkStatus_->setObjectName(QStringLiteral("startupCheckStatus"));
    layout->addWidget(checkStatus_);

    checkList_ = new QListWidget(this);
    checkList_->setObjectName(QStringLiteral("startupCheckList"));
    checkList_->setSelectionMode(QAbstractItemView::NoSelection);
    checkList_->setFrameShape(QFrame::StyledPanel);
    layout->addWidget(checkList_, /*stretch*/ 1);
}

void StartupPage::runChecks(const QList<StartupCheckItem>& items)
{
    pendingChecks_ = items;
    totalChecks_ = items.size();
    failedChecks_ = 0;
    checkList_->clear();
    if (totalChecks_ == 0) {
        checkStatus_->setText(tr("Nothing to check — no toolchains or kits "
                                 "are configured."));
        emit checksFinished(0, 0);
        return;
    }
    updateCheckProgress();
    // One item per event-loop turn: the page keeps repainting between
    // checks, and a future per-item probe that runs a subprocess (-v) will
    // slot in without blocking the UI.
    QTimer::singleShot(0, this, &StartupPage::runNextCheck);
}

void StartupPage::updateCheckProgress()
{
    const int done = totalChecks_ - pendingChecks_.size();
    checkStatus_->setText(
        tr("Checking toolchains and kits... %1/%2").arg(done).arg(totalChecks_));
}

void StartupPage::runNextCheck()
{
    if (pendingChecks_.isEmpty()) {
        checkStatus_->setText(
            failedChecks_ == 0
                ? tr("All toolchains and kits are ready.")
                : tr("%1 of %2 have a problem — see the red rows.")
                      .arg(failedChecks_)
                      .arg(totalChecks_));
        emit checksFinished(totalChecks_, failedChecks_);
        return;
    }

    const StartupCheckItem item = pendingChecks_.takeFirst();
    bool ok = false;
    QString status;
    if (item.requiresFile) {
        ok = QFileInfo(item.detail).isFile();
        status = ok ? tr("OK")
                    : tr("missing: %1").arg(item.detail);
    } else {
        status = item.detail;  // the reason, already phrased by the caller
    }
    auto* row = new QListWidgetItem(
        QStringLiteral("%1  \u2014  %2").arg(item.label, status), checkList_);
    if (!ok) {
        row->setForeground(Qt::red);
        ++failedChecks_;
    }
    updateCheckProgress();
    QTimer::singleShot(0, this, &StartupPage::runNextCheck);
}
