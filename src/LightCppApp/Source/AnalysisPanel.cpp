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
#include <QLineEdit>
#include <QMessageBox>
#include <QSaveFile>
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
    // The three analysers below need a build to observe, so they stay
    // unchecked: running them by default would make "Analyse" build the
    // project behind the user's back.
    scanBuildBox_ = new QCheckBox(tr("scan-build"), this);
    scanBuildBox_->setStatusTip(
        tr("Clang Static Analyzer, driven through the build (needs a build command)"));
    pvsBox_ = new QCheckBox(tr("PVS-Studio"), this);
    pvsBox_->setStatusTip(tr("Analyse a build log produced by pvs-studio-analyzer"));
    coverityBox_ = new QCheckBox(tr("Coverity"), this);
    coverityBox_->setStatusTip(
        tr("cov-build + cov-analyze + cov-format-errors --csv-output"));
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
    toolRow->addWidget(scanBuildBox_);
    toolRow->addWidget(pvsBox_);
    toolRow->addWidget(coverityBox_);
    toolRow->addWidget(complexityBox_);
    toolRow->addWidget(runButton_);
    toolRow->addWidget(stopButton_);
    toolRow->addWidget(categoryBox_, 1);
    layout->addLayout(toolRow);

    // clang-tidy's rule set. The first entry keeps the project's own
    // configuration — the historical behaviour, and what a team with a
    // committed .clang-tidy actually wants.
    auto* tidyRow = new QHBoxLayout();
    tidyRow->addWidget(new QLabel(tr("clang-tidy checks:"), this));
    checkSetBox_ = new QComboBox(this);
    checkSetBox_->setObjectName(QStringLiteral("analysisCheckSet"));
    for (Workbench::TidyCheckSet set :
         {Workbench::TidyCheckSet::Project, Workbench::TidyCheckSet::Style,
          Workbench::TidyCheckSet::Unused, Workbench::TidyCheckSet::NullPointer,
          Workbench::TidyCheckSet::Bounds, Workbench::TidyCheckSet::Leak,
          Workbench::TidyCheckSet::CodingStandard, Workbench::TidyCheckSet::Security}) {
        checkSetBox_->addItem(Workbench::tidyCheckSetName(set),
                              static_cast<int>(set));
    }
    tidyRow->addWidget(checkSetBox_, 1);
    writeConfigButton_ = new QPushButton(tr("Write .clang-tidy..."), this);
    writeConfigButton_->setObjectName(QStringLiteral("analysisWriteTidyConfig"));
    writeConfigButton_->setStatusTip(
        tr("Write the selected rule set into the project so every tool and "
           "editor agrees with this panel"));
    tidyRow->addWidget(writeConfigButton_);
    layout->addLayout(tidyRow);

    connect(writeConfigButton_, &QPushButton::clicked, this, [this] {
        if (projectRoot_.isEmpty()) {
            log_->appendPlainText(
                tr("Open a project first: the file belongs at its root."));
            return;
        }
        const auto set = static_cast<Workbench::TidyCheckSet>(
            checkSetBox_->currentData().toInt());
        const QString target = QDir(projectRoot_).filePath(
            QStringLiteral(".clang-tidy"));
        // Refuse to overwrite silently: a project's .clang-tidy is a
        // reviewed file, and losing it to a menu action is unrecoverable
        // without git.
        if (QFileInfo::exists(target)
            && QMessageBox::question(
                   this, tr("Overwrite .clang-tidy?"),
                   tr("%1 already exists. Replace it with the %2 rule set?")
                       .arg(target, Workbench::tidyCheckSetName(set)))
                   != QMessageBox::Yes) {
            return;
        }
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            log_->appendPlainText(tr("Cannot write %1").arg(target));
            return;
        }
        const QString checks =
            set == Workbench::TidyCheckSet::Project
                ? Workbench::tidyChecksAllNamed()
                : Workbench::tidyChecks(set);
        file.write(Workbench::clangTidyConfigFor(checks).toUtf8());
        if (file.commit()) {
            log_->appendPlainText(tr("Wrote %1").arg(target));
        }
    });

    // scan-build wraps a build, so it needs to know which one. The field is
    // always visible rather than appearing with the checkbox: a user who
    // ticks scan-build and finds no input would have to guess why nothing
    // happened.
    auto* buildRow = new QHBoxLayout();
    buildRow->addWidget(new QLabel(tr("Build command:"), this));
    buildCommandEdit_ = new QLineEdit(this);
    buildCommandEdit_->setObjectName(QStringLiteral("analysisBuildCommand"));
    buildCommandEdit_->setPlaceholderText(
        tr("cmake --build build   (used by scan-build)"));
    buildRow->addWidget(buildCommandEdit_, 1);
    layout->addLayout(buildRow);

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
    // Order matters: cppcheck and clang-tidy read source, the three below
    // build. Putting the build-driven ones last means their output (which
    // includes a full build log) does not bury the cheap answers.
    if (scanBuildBox_->isChecked()) {
        pendingTools_ << QStringLiteral("scan-build");
    }
    if (pvsBox_->isChecked()) {
        pendingTools_ << QStringLiteral("pvs-studio");
    }
    if (coverityBox_->isChecked()) {
        pendingTools_ << QStringLiteral("coverity");
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
        const auto set = static_cast<Workbench::TidyCheckSet>(
            checkSetBox_->currentData().toInt());
        const QStringList arguments = Workbench::clangTidyArguments(
            sourceFiles_.first(), buildDirectory_, Workbench::tidyChecks(set));
        log_->appendPlainText(tr("$ clang-tidy %1").arg(arguments.join(QLatin1Char(' '))));
        process_->setWorkingDirectory(projectRoot_);
        process_->start(path, arguments);
        return;
    }
    if (runningTool_ == QLatin1String("scan-build")) {
        const QString path = LightCpp::App::ToolchainPaths::scanBuildPath();
        if (path.isEmpty()) {
            log_->appendPlainText(tr("scan-build not found; skipped."));
            runNextTool();
            return;
        }
        const QString buildCommand = buildCommandEdit_->text().trimmed();
        if (buildCommand.isEmpty()) {
            // Refuse rather than guess: wrapping the wrong command produces
            // an empty report, which looks exactly like "no bugs found".
            log_->appendPlainText(
                tr("scan-build needs a build command; skipped."));
            runNextTool();
            return;
        }
        const QStringList arguments =
            Workbench::scanBuildArguments(buildCommand);
        log_->appendPlainText(
            tr("$ scan-build %1").arg(arguments.join(QLatin1Char(' '))));
        process_->setWorkingDirectory(projectRoot_);
        process_->start(path, arguments);
        return;
    }
    if (runningTool_ == QLatin1String("pvs-studio")) {
        // The analyzer's own two-step flow: log a build, then analyse the
        // log. Only the analysis step produces findings, so the log is
        // produced on a previous run (or by the user) and named here.
        const QString path = LightCpp::App::ToolchainPaths::pvsStudioPath();
        if (path.isEmpty()) {
            log_->appendPlainText(tr("PVS-Studio not found; skipped."));
            runNextTool();
            return;
        }
        const QString log = QDir(projectRoot_).filePath(QStringLiteral("pvs.log"));
        if (!QFileInfo::exists(log)) {
            log_->appendPlainText(
                tr("PVS-Studio: %1 does not exist. Create it with "
                   "`pvs-studio-analyzer trace -- <build command>` first.")
                    .arg(log));
            runNextTool();
            return;
        }
        const QStringList arguments{
            QStringLiteral("analyze"), QStringLiteral("--output-format"),
            QStringLiteral("text"), QStringLiteral("-f"), log};
        log_->appendPlainText(
            tr("$ pvs-studio-analyzer %1").arg(arguments.join(QLatin1Char(' '))));
        process_->setWorkingDirectory(projectRoot_);
        process_->start(path, arguments);
        return;
    }
    if (runningTool_ == QLatin1String("coverity")) {
        const QString covBuild = LightCpp::App::ToolchainPaths::coverityPath();
        if (covBuild.isEmpty()) {
            log_->appendPlainText(tr("Coverity not found; skipped."));
            runNextTool();
            return;
        }
        // cov-build, cov-analyze and cov-format-errors are three processes;
        // the panel runs them as one shell line so the queue stays a queue.
        const QString buildCommand = buildCommandEdit_->text().trimmed();
        if (buildCommand.isEmpty()) {
            log_->appendPlainText(
                tr("Coverity needs a build command; skipped."));
            runNextTool();
            return;
        }
        const QDir binDir(QFileInfo(covBuild).absolutePath());
        const QString intermediate =
            QDir(projectRoot_).filePath(QStringLiteral("cov-int"));
        const QString script =
            QStringLiteral("%1 --dir %2 -- %3 && %4 --dir %5 --all && %6 "
                           "--dir %5 --all --csv-output -o %7")
                .arg(covBuild, intermediate, buildCommand,
                     binDir.filePath(QStringLiteral("cov-analyze")), intermediate,
                     binDir.filePath(QStringLiteral("cov-format-errors")),
                     QDir(projectRoot_).filePath(QStringLiteral("coverity.csv")));
        log_->appendPlainText(tr("$ %1").arg(script));
        process_->setWorkingDirectory(projectRoot_);
#ifdef Q_OS_WIN
        process_->start(QStringLiteral("cmd.exe"),
                        {QStringLiteral("/c"), script});
#else
        process_->start(QStringLiteral("/bin/sh"),
                        {QStringLiteral("-c"), script});
#endif
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
    } else if (runningTool_ == QLatin1String("scan-build")) {
        // scan-build prints one block per bug; the count is in the last
        // lines ("scan-build: N bugs found"), which is also a finding that
        // nothing else reports, so an empty list there is not "no bugs".
        appendFindings(Workbench::parseScanBuildText(text));
        report_.toolsRun << QStringLiteral("scan-build");
    } else if (runningTool_ == QLatin1String("pvs-studio")) {
        appendFindings(Workbench::parsePvsText(text));
        report_.toolsRun << QStringLiteral("PVS-Studio");
    } else if (runningTool_ == QLatin1String("coverity")) {
        // Prefer the CSV: it is the only form that keeps the file, line and
        // checker for every defect. Falling back to the console text would
        // lose the locations.
        const QString csvPath =
            QDir(projectRoot_).filePath(QStringLiteral("coverity.csv"));
        QFile csv(csvPath);
        int parsed = 0;
        if (csv.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const auto findings =
                Workbench::parseCoverityCsv(QString::fromUtf8(csv.readAll()));
            appendFindings(findings);
            parsed = int(findings.size());
        }
        if (parsed == 0) {
            // The CSV is generated from the intermediate directory; when the
            // format step failed, the console output still names the defects.
            appendFindings(Workbench::parseCoverityCsv(text));
        }
        report_.toolsRun << QStringLiteral("Coverity");
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
