// ProjectSearchDialog — "Search in Project".
//
// The scan runs in ProjectSearchThread (a QThread subclass) so file reads
// never block the GUI thread. Hits stream back per file as queued signals;
// the dialog groups them under their file and hands the main window a
// (path, line) pair when the user activates a hit.
#include "LightCpp/App/ProjectSearchDialog.hpp"

#include "LightCpp/Text/TextFile.hpp"

#include <QCheckBox>
#include <QDirIterator>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

// The App-layer classes live in the global namespace (see the header note);
// only the file-local helpers below are kept in an anonymous namespace.

namespace {

/// Files bigger than this are treated as unscannable (binary blobs, dumps).
constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024;

/// The scan stops once this many hits are collected — a runaway regex on a
/// huge tree must not fill the RAM or the list forever.
constexpr int kMaxTotalHits = 2000;

/// Long source lines are cut before they reach the results list.
constexpr int kMaxHitTextLength = 240;

}  // namespace

// ---------------------------------------------------------------------------
// ProjectSearchThread
// ---------------------------------------------------------------------------
QRegularExpression ProjectSearchThread::buildPattern(const QString& pattern,
                                                     bool caseSensitive,
                                                     bool wholeWord,
                                                     bool useRegex)
{
    QString effective = useRegex ? pattern
                                 : QRegularExpression::escape(pattern);
    if (wholeWord) {
        effective = QStringLiteral("\\b(?:%1)\\b").arg(effective);
    }
    QRegularExpression::PatternOptions options =
        QRegularExpression::NoPatternOption;
    if (!caseSensitive) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    return QRegularExpression(effective, options);
}

ProjectSearchThread::ProjectSearchThread(const QString& root,
                                         const QString& pattern,
                                         bool caseSensitive, bool wholeWord,
                                         bool useRegex, QObject* parent)
    : QThread(parent),
      root_(root),
      regex_(buildPattern(pattern, caseSensitive, wholeWord, useRegex))
{
}

bool ProjectSearchThread::isSearchableFile(const QString& fileName)
{
    static const QSet<QString> kExtensions = {
        QStringLiteral("c"),    QStringLiteral("cc"),  QStringLiteral("cpp"),
        QStringLiteral("cxx"),  QStringLiteral("c++"), QStringLiteral("h"),
        QStringLiteral("hh"),   QStringLiteral("hpp"), QStringLiteral("hxx"),
        QStringLiteral("ipp"),  QStringLiteral("inl"), QStringLiteral("ino"),
        QStringLiteral("txt"),  QStringLiteral("md"),  QStringLiteral("cmake"),
        QStringLiteral("json"), QStringLiteral("xml"), QStringLiteral("lua"),
        QStringLiteral("py"),   QStringLiteral("rc"),  QStringLiteral("ui"),
        QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("dox"),
    };
    static const QSet<QString> kNames = {
        QStringLiteral("CMakeLists.txt"), QStringLiteral("Makefile"),
        QStringLiteral("README"), QStringLiteral("LICENSE"),
    };
    const QFileInfo info(fileName);
    return kExtensions.contains(info.suffix().toLower())
           || kNames.contains(info.fileName());
}

std::optional<QString> ProjectSearchThread::readTextFile(
    const QString& path) const
{
    // LightCppText owns the "is this a file we may scan" policy (binary sniff,
    // size limit, no lossy decoding) and is unit-tested there. Doing it here
    // by hand is how the scan lost the first 4 KiB of every file: the NUL sniff
    // read a prefix, and the readAll() that followed returned only the rest.
    return LightCpp::Text::readSearchableTextFile(path, kMaxFileBytes);
}

void ProjectSearchThread::run()
{
    int filesScanned = 0;
    int totalHits = 0;

    QDirIterator it(root_, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (aborted_.load()) {
            break;
        }
        const QString path = it.next();
        const QString relative = QDir::fromNativeSeparators(
            QDir(root_).relativeFilePath(path));

        // Skip generated and VCS-internal directories at *any* depth: a search
        // that only recognised "build/" at the root still walked src/build and
        // .git, and those files could spend the whole hit cap between them.
        if (LightCpp::Text::isUnderSkippedDirectory(relative)
            || !isSearchableFile(path)) {
            continue;
        }

        const std::optional<QString> text = readTextFile(path);
        if (!text.has_value()) {
            continue;
        }
        ++filesScanned;

        QList<QPair<int, QString>> hits;
        const QStringList lines =
            text->split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        for (int i = 0; i < lines.size(); ++i) {
            const QStringView line(lines.at(i));
            if (regex_.matchView(line).hasMatch()) {
                QString trimmed = line.toString().trimmed();
                if (trimmed.size() > kMaxHitTextLength) {
                    trimmed = trimmed.left(kMaxHitTextLength)
                              + QStringLiteral(" …");
                }
                hits.append({i + 1, trimmed});
                ++totalHits;
                if (totalHits >= kMaxTotalHits) {
                    break;
                }
            }
        }
        if (!hits.isEmpty()) {
            emit fileHits(path, hits);
        }
        if (totalHits >= kMaxTotalHits) {
            break;
        }
    }
    const bool abortedByUser = aborted_.load();
    emit scanFinished(filesScanned, totalHits, abortedByUser);
}

// ---------------------------------------------------------------------------
// ProjectSearchDialog
// ---------------------------------------------------------------------------
ProjectSearchDialog::ProjectSearchDialog(const QString& root,
                                         QWidget* parent)
    : QDialog(parent), root_(root)
{
    setWindowTitle(tr("Search in Project — %1")
                       .arg(QDir::toNativeSeparators(root)));
    resize(760, 520);

    auto* topRow = new QHBoxLayout;
    pattern_ = new QLineEdit(this);
    pattern_->setPlaceholderText(
        tr("Search text (regular expressions when enabled)"));
    topRow->addWidget(pattern_, 1);

    caseSensitive_ = new QCheckBox(tr("&Case sensitive"), this);
    wholeWord_ = new QCheckBox(tr("&Whole word"), this);
    useRegex_ = new QCheckBox(tr("Re&gex"), this);
    topRow->addWidget(caseSensitive_);
    topRow->addWidget(wholeWord_);
    topRow->addWidget(useRegex_);

    searchButton_ = new QPushButton(tr("&Search"), this);
    searchButton_->setDefault(true);
    topRow->addWidget(searchButton_);

    results_ = new QTreeWidget(this);
    results_->setColumnCount(2);
    results_->setHeaderLabels({tr("Line"), tr("Match")});
    results_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    results_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    results_->setRootIsDecorated(true);
    results_->setUniformRowHeights(true);
    results_->setAlternatingRowColors(true);

    status_ = new QLabel(this);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(topRow);
    layout->addWidget(results_, 1);
    layout->addWidget(status_);

    connect(pattern_, &QLineEdit::returnPressed,
            this, &ProjectSearchDialog::startSearch);
    connect(searchButton_, &QPushButton::clicked,
            this, &ProjectSearchDialog::startSearch);
    connect(results_, &QTreeWidget::itemActivated,
            this, &ProjectSearchDialog::onResultActivated);

    pattern_->setFocus();
    startSearch();
}

ProjectSearchDialog::~ProjectSearchDialog()
{
    // The dialog can close (Esc, parent destroyed) while the scan runs. Hand
    // the worker over before the QObject tree goes down, so it is never
    // destroyed mid-flight.
    stopThread();
}

void ProjectSearchDialog::stopThread()
{
    ProjectSearchThread* worker = thread_;
    thread_ = nullptr;
    if (worker == nullptr) {
        return;
    }
    // Whatever the old query still has queued belongs to a stale pattern.
    worker->disconnect(this);
    if (!worker->isRunning()) {
        worker->deleteLater();
        return;
    }
    worker->abort();
    if (worker->wait(10000)) {
        worker->deleteLater();
        return;
    }
    // A read is wedged (a network drive, a blocking FIFO…). Deleting a running
    // QThread calls qFatal — "QThread: Destroyed while thread is still
    // running" takes the whole IDE down — so let the worker go instead: it is
    // no longer a child of this dialog, and it will delete itself on finish.
    worker->setParent(nullptr);
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    if (worker->isFinished()) {
        worker->deleteLater();  // it slipped through between wait() and here
    }
}

void ProjectSearchDialog::startSearch()
{
    stopThread();

    results_->clear();
    filesScanned_ = 0;
    totalHits_ = 0;

    const QString pattern = pattern_->text();
    if (pattern.isEmpty()) {
        searchButton_->setEnabled(true);
        status_->setText(tr("Type something to search for."));
        return;
    }

    // Validate before starting a scan: a pattern the engine cannot compile
    // matches nothing at all, and "No matches" for a broken expression reads
    // as "the text is not in the project".
    const QRegularExpression engine = ProjectSearchThread::buildPattern(
        pattern, caseSensitive_->isChecked(), wholeWord_->isChecked(),
        useRegex_->isChecked());
    if (!engine.isValid()) {
        searchButton_->setEnabled(true);
        status_->setText(
            tr("Invalid regular expression: %1").arg(engine.errorString()));
        return;
    }

    auto* worker = new ProjectSearchThread(
        root_, pattern, caseSensitive_->isChecked(),
        wholeWord_->isChecked(), useRegex_->isChecked(), this);
    thread_ = worker;
    connect(worker, &ProjectSearchThread::fileHits,
            this, &ProjectSearchDialog::onFileHits);
    connect(worker, &ProjectSearchThread::scanFinished,
            this, &ProjectSearchDialog::onScanFinished);
    searchButton_->setEnabled(false);
    worker->start();
    updateStatus();
}

void ProjectSearchDialog::onFileHits(
    const QString& filePath, const QList<QPair<int, QString>>& hits)
{
    ++filesScanned_;
    totalHits_ += hits.size();

    // Group under the file path. The file row itself shows the hit count.
    auto* fileItem = new QTreeWidgetItem(results_);
    fileItem->setText(0, QString::number(hits.size()));
    fileItem->setText(1, QDir::toNativeSeparators(filePath));
    fileItem->setFirstColumnSpanned(false);
    QFont fileFont = fileItem->font(1);
    fileFont.setBold(true);
    fileItem->setFont(1, fileFont);
    fileItem->setData(0, Qt::UserRole, filePath);
    for (const auto& [line, text] : hits) {
        auto* hitItem = new QTreeWidgetItem(fileItem);
        hitItem->setText(0, QString::number(line));
        hitItem->setText(1, text);
        // The jump needs both the path and the 1-based line number.
        hitItem->setData(0, Qt::UserRole, filePath);
        hitItem->setData(0, Qt::UserRole + 1, line);
    }
    updateStatus();
}

void ProjectSearchDialog::onScanFinished(int filesScanned, int totalHits,
                                         bool abortedByUser)
{
    filesScanned_ = filesScanned;
    totalHits_ = totalHits;
    searchButton_->setEnabled(true);
    if (abortedByUser) {
        status_->setText(tr("Search stopped."));
    } else if (totalHits == 0) {
        status_->setText(
            tr("No matches. %1 files scanned.").arg(filesScanned));
    } else {
        status_->setText(tr("%1 matches in %2 files.")
                             .arg(totalHits)
                             .arg(results_->topLevelItemCount()));
    }
}

void ProjectSearchDialog::onResultActivated(QTreeWidgetItem* item, int)
{
    if (item == nullptr) {
        return;
    }
    const QString path = item->data(0, Qt::UserRole).toString();
    const QVariant lineValue = item->data(0, Qt::UserRole + 1);
    if (path.isEmpty() || !lineValue.isValid()) {
        return;  // a file row: activating it does nothing
    }
    emit resultActivated(path, lineValue.toInt());
}

void ProjectSearchDialog::updateStatus()
{
    if (thread_ != nullptr && thread_->isRunning()) {
        status_->setText(tr("Searching… %1 matches so far.").arg(totalHits_));
    }
}
