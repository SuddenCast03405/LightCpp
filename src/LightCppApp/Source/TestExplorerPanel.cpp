// TestExplorerPanel — implementation. See the header for the split.

#include "LightCpp/App/TestExplorerPanel.hpp"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace Testing = LightCpp::Testing;

TestExplorerPanel::TestExplorerPanel(QWidget* parent) : QWidget(parent)
{
    buildUi();
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyRead, this, &TestExplorerPanel::onProcessOutput);
    connect(process_, &QProcess::finished, this, &TestExplorerPanel::onProcessFinished);
}

void TestExplorerPanel::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* projectRow = new QHBoxLayout();
    projectPath_ = new QLineEdit(this);
    projectPath_->setPlaceholderText(tr("Path to CMakeLists.txt"));
    buildPath_ = new QLineEdit(this);
    buildPath_->setPlaceholderText(tr("Build directory"));
    frameworkBox_ = new QComboBox(this);
    for (Testing::TestFramework framework :
         {Testing::TestFramework::GoogleTest, Testing::TestFramework::Catch2,
          Testing::TestFramework::BoostTest, Testing::TestFramework::CTest}) {
        frameworkBox_->addItem(Testing::frameworkName(framework),
                               static_cast<int>(framework));
    }
    projectRow->addWidget(new QLabel(tr("Project"), this));
    projectRow->addWidget(projectPath_, 2);
    projectRow->addWidget(new QLabel(tr("Build"), this));
    projectRow->addWidget(buildPath_, 2);
    projectRow->addWidget(frameworkBox_);
    layout->addLayout(projectRow);

    auto* buttonRow = new QHBoxLayout();
    discoverButton_ = new QPushButton(tr("Discover"), this);
    runAllButton_ = new QPushButton(tr("Run All"), this);
    runOneButton_ = new QPushButton(tr("Run Selected"), this);
    rerunButton_ = new QPushButton(tr("Rerun Failed"), this);
    stopButton_ = new QPushButton(tr("Stop"), this);
    coverageButton_ = new QPushButton(tr("Coverage…"), this);
    timeoutSpin_ = new QSpinBox(this);
    timeoutSpin_->setRange(1, 120);
    timeoutSpin_->setValue(5);
    timeoutSpin_->setSuffix(tr(" min"));
    buttonRow->addWidget(discoverButton_);
    buttonRow->addWidget(runAllButton_);
    buttonRow->addWidget(runOneButton_);
    buttonRow->addWidget(rerunButton_);
    buttonRow->addWidget(stopButton_);
    buttonRow->addWidget(coverageButton_);
    buttonRow->addWidget(new QLabel(tr("Timeout"), this));
    buttonRow->addWidget(timeoutSpin_);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({tr("Test"), tr("Outcome"), tr("Time"), tr("Message")});
    tree_->setRootIsDecorated(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr) {
            return;
        }
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || index >= lastRun_.cases.size()) {
            return;
        }
        const Testing::TestCaseResult& result = lastRun_.cases.at(index);
        if (result.outcome == Testing::TestOutcome::Failed
            || result.outcome == Testing::TestOutcome::TimedOut) {
            emit failureActivated(result.file, result.line > 0 ? result.line - 1 : 0);
        }
    });
    layout->addWidget(tree_, 3);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(4000);
    layout->addWidget(log_, 2);

    summary_ = new QLabel(tr("No project loaded."), this);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    connect(discoverButton_, &QPushButton::clicked, this, &TestExplorerPanel::discover);
    connect(runAllButton_, &QPushButton::clicked, this, &TestExplorerPanel::runAll);
    connect(runOneButton_, &QPushButton::clicked, this, &TestExplorerPanel::runSelected);
    connect(rerunButton_, &QPushButton::clicked, this, &TestExplorerPanel::rerunFailed);
    connect(stopButton_, &QPushButton::clicked, this, &TestExplorerPanel::stopRun);
    connect(coverageButton_, &QPushButton::clicked, this, &TestExplorerPanel::showCoverage);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item != nullptr && item->parent() == nullptr) {
            runSelected();
        }
    });
}

void TestExplorerPanel::setProject(const QString& projectFile, const QString& buildDirectory)
{
    projectPath_->setText(projectFile);
    buildPath_->setText(buildDirectory);
    project_ = Testing::detectFramework(projectFile);
    if (project_.framework != Testing::TestFramework::Unknown) {
        const int index = frameworkBox_->findData(static_cast<int>(project_.framework));
        if (index >= 0) {
            frameworkBox_->setCurrentIndex(index);
        }
    }
    project_.buildDirectory = buildDirectory;
    summary_->setText(project_.framework == Testing::TestFramework::Unknown
                          ? tr("No test framework recognised in this project.")
                          : tr("%1 detected. %2 executable(s).")
                                .arg(Testing::frameworkName(project_.framework))
                                .arg(project_.executables.size()));
    tests_.clear();
    tree_->clear();
}

bool TestExplorerPanel::hasProject() const
{
    return project_.framework != Testing::TestFramework::Unknown;
}

void TestExplorerPanel::discover()
{
    const auto framework =
        static_cast<Testing::TestFramework>(frameworkBox_->currentData().toInt());
    if (framework == Testing::TestFramework::Unknown) {
        return;
    }
    runningFramework_ = framework;
    discovering_ = true;

    if (Testing::runsThroughCtest(framework)) {
        Testing::TestRunPlan plan;
        plan.program = QStringLiteral("ctest");
        plan.arguments << QStringLiteral("--test-dir") << buildPath_->text()
                       << QStringLiteral("-N");
        plan.workingDirectory = buildPath_->text();
        plan.timeoutMs = 60000;
        log_->appendPlainText(tr("$ %1").arg(plan.commandLine()));
        pendingOutput_.clear();
        process_->setWorkingDirectory(plan.workingDirectory);
        process_->start(plan.program, plan.arguments);
        return;
    }

    QString executable;
    if (!project_.executables.isEmpty()) {
        executable = project_.executables.first();
        if (!QFileInfo(executable).isAbsolute()) {
            // A bare target name resolves against the build directory: that
            // is where CMake puts it, and it is what the user typed.
            executable = QDir(buildPath_->text()).filePath(executable);
        }
    }
    if (executable.isEmpty() || !QFileInfo::exists(executable)) {
        log_->appendPlainText(tr("No test executable found. Build the project first."));
        discovering_ = false;
        return;
    }
    Testing::TestRunPlan plan;
    plan.program = executable;
    plan.arguments = Testing::discoveryArguments(framework);
    plan.workingDirectory = QFileInfo(executable).absolutePath();
    plan.timeoutMs = 60000;
    log_->appendPlainText(tr("$ %1").arg(plan.commandLine()));
    pendingOutput_.clear();
    process_->setWorkingDirectory(plan.workingDirectory);
    process_->start(plan.program, plan.arguments);
}

void TestExplorerPanel::runPlan(const Testing::TestRunPlan& plan, const QString& title)
{
    if (process_->state() != QProcess::NotRunning) {
        log_->appendPlainText(tr("A run is already in progress."));
        return;
    }
    log_->appendPlainText(QStringLiteral("--- %1 ---").arg(title));
    log_->appendPlainText(tr("$ %1").arg(plan.commandLine()));
    emit logLine(tr("%1: %2").arg(title, plan.commandLine()));
    pendingOutput_.clear();
    discovering_ = false;
    if (!plan.workingDirectory.isEmpty()) {
        process_->setWorkingDirectory(plan.workingDirectory);
    }
    process_->start(plan.program, plan.arguments);
    const int budget = timeoutSpin_->value() * 60 * 1000;
    QTimer::singleShot(budget, this, [this, title] {
        if (process_->state() == QProcess::NotRunning) {
            return;
        }
        log_->appendPlainText(tr("--- %1 timed out after %2 min ---")
                                  .arg(title)
                                  .arg(timeoutSpin_->value()));
        // A killed run is recorded as a failure, not as "no result": the
        // user asked a question and the answer is "it did not finish".
        lastRun_ = Testing::parseRunOutput(runningFramework_,
                                           pendingOutput_.join(QLatin1Char('\n')), -1);
        for (Testing::TestCaseResult& result : lastRun_.cases) {
            if (result.outcome == Testing::TestOutcome::NotRun) {
                result.outcome = Testing::TestOutcome::TimedOut;
            }
        }
        lastRun_.failed = qMax(1, lastRun_.failed);
        process_->kill();
        applyResults();
    });
}

void TestExplorerPanel::runAll()
{
    const auto framework =
        static_cast<Testing::TestFramework>(frameworkBox_->currentData().toInt());
    runningFramework_ = framework;
    QString executable;
    if (!project_.executables.isEmpty()) {
        executable = project_.executables.first();
        if (!QFileInfo(executable).isAbsolute()) {
            executable = QDir(buildPath_->text()).filePath(executable);
        }
    }
    runPlan(Testing::planRunAll(framework, executable, buildPath_->text()),
            tr("Run all tests"));
}

Testing::TestCase TestExplorerPanel::selectedTest() const
{
    QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) {
        return {};
    }
    if (item->parent() != nullptr) {
        item = item->parent();
    }
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= tests_.size()) {
        return {};
    }
    return tests_.at(index);
}

void TestExplorerPanel::runSelected()
{
    const Testing::TestCase test = selectedTest();
    if (test.name.isEmpty() && test.suite.isEmpty()) {
        log_->appendPlainText(tr("Select a test first."));
        return;
    }
    const auto framework =
        static_cast<Testing::TestFramework>(frameworkBox_->currentData().toInt());
    runningFramework_ = framework;
    QString executable;
    if (!project_.executables.isEmpty()) {
        executable = project_.executables.first();
        if (!QFileInfo(executable).isAbsolute()) {
            executable = QDir(buildPath_->text()).filePath(executable);
        }
    }
    runPlan(Testing::planRun(framework, executable, buildPath_->text(), test),
            tr("Run %1").arg(test.displayName()));
}

void TestExplorerPanel::rerunFailed()
{
    if (lastRun_.failed == 0) {
        log_->appendPlainText(tr("No failed test to rerun."));
        return;
    }
    const QStringList arguments =
        Testing::rerunFailedArguments(runningFramework_, lastRun_);
    if (arguments.isEmpty()) {
        return;
    }
    Testing::TestRunPlan plan;
    if (Testing::runsThroughCtest(runningFramework_)) {
        plan.program = QStringLiteral("ctest");
        plan.arguments << QStringLiteral("--test-dir") << buildPath_->text() << arguments;
        plan.workingDirectory = buildPath_->text();
    } else {
        QString executable;
        if (!project_.executables.isEmpty()) {
            executable = project_.executables.first();
            if (!QFileInfo(executable).isAbsolute()) {
                executable = QDir(buildPath_->text()).filePath(executable);
            }
        }
        plan.program = executable;
        plan.arguments = arguments;
        plan.workingDirectory = QFileInfo(executable).absolutePath();
    }
    plan.timeoutMs = Testing::defaultTimeoutMs(runningFramework_);
    runPlan(plan, tr("Rerun failed"));
}

void TestExplorerPanel::stopRun()
{
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        log_->appendPlainText(tr("--- killed ---"));
    }
}

void TestExplorerPanel::onProcessOutput()
{
    const QString text = QString::fromLocal8Bit(process_->readAll());
    pendingOutput_ << text;
    log_->moveCursor(QTextCursor::End);
    log_->insertPlainText(text);
    log_->moveCursor(QTextCursor::End);
}

void TestExplorerPanel::onProcessFinished()
{
    const QString text = pendingOutput_.join(QLatin1Char('\n'));
    const int exitCode = process_->exitCode();

    if (discovering_) {
        discovering_ = false;
        tests_ = Testing::parseList(runningFramework_, text);
        log_->appendPlainText(tr("%1 test(s) discovered.").arg(tests_.size()));
        rebuildTree();
        return;
    }
    lastRun_ = Testing::parseRunOutput(runningFramework_, text, exitCode);
    lastRun_.durationMs = 0;
    applyResults();
}

void TestExplorerPanel::rebuildTree()
{
    tree_->clear();
    QString currentSuite;
    QTreeWidgetItem* suiteItem = nullptr;
    for (int i = 0; i < tests_.size(); ++i) {
        const Testing::TestCase& test = tests_.at(i);
        QTreeWidgetItem* parent = nullptr;
        if (!test.suite.isEmpty()) {
            if (suiteItem == nullptr || currentSuite != test.suite) {
                suiteItem = new QTreeWidgetItem(tree_, {test.suite});
                currentSuite = test.suite;
            }
            parent = suiteItem;
        }
        auto* item = parent != nullptr ? new QTreeWidgetItem(parent)
                                       : new QTreeWidgetItem(tree_);
        item->setText(0, test.name);
        item->setData(0, Qt::UserRole, i);
        if (!test.tags.isEmpty()) {
            item->setToolTip(0, test.tags.join(QLatin1Char(' ')));
        }
    }
    tree_->expandAll();
}

void TestExplorerPanel::applyResults()
{
    summary_->setText(QStringLiteral("%1 passed, %2 failed, %3 skipped — %4")
                          .arg(lastRun_.passed)
                          .arg(lastRun_.failed)
                          .arg(lastRun_.skipped)
                          .arg(lastRun_.allPassed() ? tr("all passed") : tr("not green")));
    // Paint the results back onto the discovered list, in discovery order;
    // the runner's order is its own and may not match.
    for (const Testing::TestCaseResult& result : lastRun_.cases) {
        for (qsizetype i = 0; i < tests_.size(); ++i) {
            if (tests_.at(i).name != result.name) {
                continue;
            }
            if (!result.suite.isEmpty() && tests_.at(i).suite != result.suite) {
                continue;
            }
            // Walk to the item: suites are top-level, cases are children.
            for (int top = 0; top < tree_->topLevelItemCount(); ++top) {
                QTreeWidgetItem* topItem = tree_->topLevelItem(top);
                QTreeWidgetItem* target = nullptr;
                if (topItem->data(0, Qt::UserRole).toInt() == i) {
                    target = topItem;
                } else {
                    for (int child = 0; child < topItem->childCount(); ++child) {
                        if (topItem->child(child)->data(0, Qt::UserRole).toInt() == i) {
                            target = topItem->child(child);
                            break;
                        }
                    }
                }
                if (target == nullptr) {
                    continue;
                }
                target->setText(1, Testing::outcomeName(result.outcome));
                if (result.durationMs > 0) {
                    target->setText(2, QStringLiteral("%1 ms").arg(result.durationMs));
                }
                if (!result.message.isEmpty()) {
                    target->setText(3, result.message.split(QLatin1Char('\n')).first());
                    target->setToolTip(3, result.message);
                }
                break;
            }
            break;
        }
    }
    emit logLine(summary_->text());
}

void TestExplorerPanel::showCoverage()
{
    const QString build = buildPath_->text();
    if (build.isEmpty()) {
        log_->appendPlainText(tr("Set the build directory first."));
        return;
    }
    // Read what is already there: a .info from lcov, or the .gcov files a
    // gcov run left. Running the tool is the build system's job; the panel
    // shows the answer.
    const QString info = QDir(build).filePath(QStringLiteral("coverage.info"));
    if (QFileInfo::exists(info)) {
        QFile file(info);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            coverage_ = Testing::parseLcov(QString::fromUtf8(file.readAll()));
        }
    } else {
        log_->appendPlainText(tr("No coverage.info in %1. Run: lcov --capture "
                                 "--directory <build> --output-file coverage.info")
                                  .arg(build));
        const QStringList gcovFiles = QDir(build).entryList({QStringLiteral("*.gcov")},
                                                           QDir::Files);
        if (!gcovFiles.isEmpty()) {
            QFile file(QDir(build).filePath(gcovFiles.first()));
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                coverage_ = Testing::parseGcov(QString::fromUtf8(file.readAll()),
                                               gcovFiles.first());
            }
        }
    }
    if (coverage_.isEmpty()) {
        summary_->setText(tr("No coverage data."));
        return;
    }
    summary_->setText(Testing::formatCoverageSummary(coverage_));
    log_->appendPlainText(summary_->text());
    for (const Testing::FileCoverage& file : coverage_.belowThreshold(80.0)) {
        log_->appendPlainText(tr("%1 — %2% (%3/%4)")
                                  .arg(file.path)
                                  .arg(file.linePercent(), 0, 'f', 1)
                                  .arg(file.linesHit)
                                  .arg(file.linesFound));
    }
}
