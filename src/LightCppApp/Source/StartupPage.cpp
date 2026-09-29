// StartupPage — the start page shown instead of an auto-opened "Untitled"
// editor tab (recent files + quick actions).

#include "LightCpp/App/StartupPage.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
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

    // Keep the block from stretching over the whole page height.
    layout->addStretch(0);
}
