// TestExplorerPanel — lightcpp.1.md §十 inside the IDE.
//
// The panel is the application half of LightCppTesting: it finds the
// framework, lists the tests, runs one/all/failed, applies a timeout, keeps a
// transcript, and reads a coverage tool's answer. All the parsing lives in
// the library; this file owns the process, the buttons and the tree.
//
// Why the process is here and not in the library: a QProcess in a library
// that the tests link would need a running event loop and a real executable
// for every test — the parsers are what actually break, and they are tested
// without any of that. The panel is thin by design: build a plan, run it,
// hand the text to a parser, show the result.

#pragma once

#include <QWidget>

#include "LightCpp/Testing/CoverageReport.hpp"
#include "LightCpp/Testing/TestModel.hpp"
#include "LightCpp/Testing/TestRunner.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

class TestExplorerPanel : public QWidget {
    Q_OBJECT
public:
    explicit TestExplorerPanel(QWidget* parent = nullptr);

    /// Point the panel at a project: reads CMakeLists.txt, decides the
    /// framework and offers discovery.
    void setProject(const QString& projectFile, const QString& buildDirectory);
    /// True when a project with a recognised framework is loaded.
    [[nodiscard]] bool hasProject() const;

signals:
    /// A failed test's source position, for the editor to jump to.
    void failureActivated(const QString& file, int line);
    /// A line for the output panel.
    void logLine(const QString& text);

private:
    void buildUi();
    void discover();
    void runAll();
    void runSelected();
    void rerunFailed();
    void runPlan(const LightCpp::Testing::TestRunPlan& plan,
                 const QString& title);
    void onProcessFinished();
    void onProcessOutput();
    void applyResults();
    void showCoverage();
    void stopRun();

    void rebuildTree();
    [[nodiscard]] LightCpp::Testing::TestCase selectedTest() const;

    QLineEdit* projectPath_ = nullptr;
    QLineEdit* buildPath_ = nullptr;
    QComboBox* frameworkBox_ = nullptr;
    QSpinBox* timeoutSpin_ = nullptr;
    QPushButton* discoverButton_ = nullptr;
    QPushButton* runAllButton_ = nullptr;
    QPushButton* runOneButton_ = nullptr;
    QPushButton* rerunButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* coverageButton_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QLabel* summary_ = nullptr;

    QProcess* process_ = nullptr;
    QStringList pendingOutput_;
    LightCpp::Testing::TestProject project_;
    QList<LightCpp::Testing::TestCase> tests_;
    LightCpp::Testing::TestRun lastRun_;
    LightCpp::Testing::CoverageReport coverage_;
    /// The plan the running process implements, so its answer is parsed with
    /// the right dialect even if the project changes mid-run.
    LightCpp::Testing::TestFramework runningFramework_ =
        LightCpp::Testing::TestFramework::Unknown;
    bool discovering_ = false;
};
