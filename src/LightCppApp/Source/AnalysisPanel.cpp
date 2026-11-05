// AnalysisPanel — implementation. See the header for the category decision.

#include "LightCpp/App/AnalysisPanel.hpp"

#include "LightCpp/App/ToolchainPaths.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace Workbench = LightCpp::Workbench;

namespace {

QString severityText(Workbench::Severity severity)
{
    return Workbench::severityName(severity);
}

}  // namespace

AnalysisPanel::AnalysisPanel(QWidget* parent) : QWidget(parent)
{
    buildUi();
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyRead, this, [this] {
        runningOutput_ += process_->readAll();
    });
    connect(process_, &QProcess::finished, this, [this] { finishTool(); });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            log_->appendPlainText(tr("%1 is not installed; skipped.").arg(runningTool_));
            finishTool();
        }
    });
}

void AnalysisPanel::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* toolRow = new QHBoxLayout();
    cppcheckBox_ = new QCheckBox(tr("Cppcheck"), this);
    cppcheckBox_->setChecked(true);
    tidyBox_ = new QCheckBox(tr("clang-tidy"), this);
    complexityBox_ = new QCheckBox(tr("Complexity (local)"), this);
    complexityBox_->setChecked(true);
    runButton_ = new QPushButton(tr("Analyse"), this);
    stopButton_ = new QPushButton(tr("Stop"), this);
    categoryBox_ = new QComboBox(this);
    categoryBox_->addItem(tr("All findings"), static_cast<int>(Workbench::FindingCategory::Other));
    for (Workbench::FindingCategory category :
         {Workbench::FindingCategory::Style, Workbench::FindingCategory::Unused,
          Workbench::FindingCategory::NullPointer, Workbench::FindingCategory::Bounds,
          Workbench::FindingCategory::Leak, Workbench::FindingCategory::Security,
          Workbench::FindingCategory::CodingStandard,
          Workbench::FindingCategory::Complexity,
          Workbench::FindingCategory::CompilerWarning,
          Workbench::FindingCategory::Performance}) {
        categoryBox_->addItem(Workbench::categoryName(category),
                              static_cast<int>(category));
    }
    toolRow->addWidget(cppcheckBox_);
    toolRow->addWidget(tidyBox_);
    toolRow->addWidget(complexityBox_);
    toolRow->addWidget(runButton_);
    toolRow->addWidget(stopButton_);
    toolRow->addWidget(categoryBox_, 1);
    layout->addLayout(toolRow);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({tr("Message"), tr("Category"), tr("Tool"), tr("Location")});
    tree_->setRootIsDecorated(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr) {
            return;
        }
        const QString file = item->data(0, Qt::UserRole).toString();
        const int line = item->data(0, Qt::UserRole + 1).toInt();
        if (!file.isEmpty()) {
            emit findingActivated(file, line > 0 ? line - 1 : 0);
        }
    });
    layout->addWidget(tree_, 3);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(3000);
    layout->addWidget(log_, 1);

    summary_ = new QLabel(tr("No analysis yet."), this);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    connect(runButton_, &QPushButton::clicked, this, &AnalysisPanel::runAnalysis);
    connect(stopButton_, &QPushButton::clicked, this, [this] {
        if (process_->state() != QProcess::NotRunning) {
            process_->kill();
        }
    });
    connect(categoryBox_, &QComboBox::currentIndexChanged,
            this, [this](int) { rebuildTree(); });
}

void AnalysisPanel::setProject(const QString& projectRoot, const QString& buildDirectory,
                               const QStringList& sourceFiles)
{
    projectRoot_ = projectRoot;
    buildDirectory_ = buildDirectory;
    sourceFiles_ = sourceFiles;
    summary_->setText(tr("%1 source file(s) in %2")
                          .arg(sourceFiles_.size())
                          .arg(projectRoot_.isEmpty() ? tr("(no project)") : projectRoot_));
    clearResults();
}

void AnalysisPanel::clearResults()
{
    report_ = Workbench::AnalysisReport{};
    tree_->clear();
}

void AnalysisPanel::runAnalysis()
{
    if (process_->state() != QProcess::NotRunning) {
        return;
    }
    clearResults();
    pendingTools_.clear();
    if (cppcheckBox_->isChecked()) {
        pendingTools_ << QStringLiteral("cppcheck");
    }
    if (tidyBox_->isChecked()) {
        pendingTools_ << QStringLiteral("clang-tidy");
    }
    if (complexityBox_->isChecked()) {
        pendingTools_ << QStringLiteral("complexity");
    }
    runNextTool();
}

void AnalysisPanel::runNextTool()
{
    if (pendingTools_.isEmpty()) {
        report_.sort();
        rebuildTree();
        summary_->setText(report_.summary());
        emit message(report_.summary());
        return;
    }
    runningTool_ = pendingTools_.takeFirst();

    if (runningTool_ == QLatin1String("complexity")) {
        // Local, synchronous, no process: every source file's functions.
        int files = 0;
        for (const QString& file : sourceFiles_) {
            QFile source(file);
            if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
                continue;
            }
            ++files;
            const QList<Workbench::Finding> findings = Workbench::complexityFindings(
                file, Workbench::computeComplexity(QString::fromUtf8(source.readAll())), 15);
            appendFindings(findings);
        }
        report_.filesAnalysed = qMax(report_.filesAnalysed, files);
        report_.toolsRun << QStringLiteral("complexity");
        runNextTool();
        return;
    }

    runningOutput_.clear();
    if (runningTool_ == QLatin1String("cppcheck")) {
        const QString path = LightCpp::App::ToolchainPaths::cppcheckPath();
        if (path.isEmpty()) {
            log_->appendPlainText(tr("cppcheck not found on PATH; skipped."));
            runNextTool();
            return;
        }
        const QStringList arguments = Workbench::cppcheckArguments(
            {projectRoot_}, buildDirectory_);
        log_->appendPlainText(tr("$ cppcheck %1").arg(arguments.join(QLatin1Char(' '))));
        process_->setWorkingDirectory(projectRoot_);
        process_->start(path, arguments);
        return;
    }
    if (runningTool_ == QLatin1String("clang-tidy")) {
        if (sourceFiles_.isEmpty()) {
            runNextTool();
            return;
        }
        const QString path = LightCpp::App::ToolchainPaths::clangTidyPath();
        if (path.isEmpty()) {
            log_->appendPlainText(tr("clang-tidy not found; skipped."));
            runNextTool();
            return;
        }
        // One invocation per file: clang-tidy takes a single translation
        // unit. The first file's answer is enough to prove the pipeline;
        // running all of them here would serialise a build-sized workload
        // behind one button.
        const QStringList arguments =
            Workbench::clangTidyArguments(sourceFiles_.first(), buildDirectory_);
        log_->appendPlainText(tr("$ clang-tidy %1").arg(arguments.join(QLatin1Char(' '))));
        process_->setWorkingDirectory(projectRoot_);
        process_->start(path, arguments);
        return;
    }
    runNextTool();
}

void AnalysisPanel::finishTool()
{
    const QString text = QString::fromUtf8(runningOutput_);
    runningOutput_.clear();
    if (runningTool_ == QLatin1String("cppcheck")) {
        appendFindings(Workbench::parseCppcheckXml(text));
        report_.toolsRun << QStringLiteral("cppcheck");
    } else if (runningTool_ == QLatin1String("clang-tidy")) {
        appendFindings(Workbench::parseClangTidyText(text));
        report_.toolsRun << QStringLiteral("clang-tidy");
    }
    if (!text.trimmed().isEmpty()) {
        log_->appendPlainText(text.trimmed().left(2000));
    }
    runNextTool();
}

void AnalysisPanel::appendFindings(const QList<Workbench::Finding>& findings)
{
    report_.findings += findings;
}

void AnalysisPanel::rebuildTree()
{
    tree_->clear();
    const auto wanted = static_cast<Workbench::FindingCategory>(
        categoryBox_->currentData().toInt());
    const bool all = categoryBox_->currentIndex() == 0;
    QHash<QString, QTreeWidgetItem*> groups;
    for (const Workbench::Finding& finding : report_.findings) {
        if (!all && finding.category != wanted) {
            continue;
        }
        const QString groupName = Workbench::categoryName(finding.category);
        QTreeWidgetItem* group = groups.value(groupName, nullptr);
        if (group == nullptr) {
            group = new QTreeWidgetItem(tree_, {groupName});
            group->setFirstColumnSpanned(true);
            QFont font = group->font(0);
            font.setBold(true);
            group->setFont(0, font);
            groups.insert(groupName, group);
        }
        auto* item = new QTreeWidgetItem(group);
        item->setText(0, finding.label());
        item->setText(1, Workbench::categoryName(finding.category));
        item->setText(2, Workbench::toolName(finding.tool));
        item->setText(3, QStringLiteral("%1:%2").arg(finding.file).arg(finding.line));
        item->setData(0, Qt::UserRole, finding.file);
        item->setData(0, Qt::UserRole + 1, finding.line);
        if (finding.severity == Workbench::Severity::Error) {
            item->setForeground(0, QBrush(QColor(0xc0, 0x2b, 0x2b)));
        }
        group->addChild(item);
        Q_UNUSED(severityText(finding.severity));
    }
    tree_->expandAll();
}
