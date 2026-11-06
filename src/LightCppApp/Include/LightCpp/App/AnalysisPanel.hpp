// AnalysisPanel — lightcpp.1.md §七 inside the IDE.
//
// Many tools, one list. The panel runs cppcheck / clang-tidy / scan-build /
// PVS-Studio / Coverity, parses with LightCppWorkbench and shows the findings
// grouped by the plan's own categories (style / unused / null pointer /
// bounds / leak / security / coding standard / complexity), because that is
// the question a user asks —
// "show me the null-pointer risks", not "show me rule V522".
//
// Complexity is computed locally: it needs no tool and no build, so it always
// runs, which makes the "complexity analysis" row of the plan true even on a
// machine with no analyser installed.

#pragma once

#include <QWidget>

#include "LightCpp/Workbench/AnalysisReport.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class AnalysisPanel : public QWidget {
    Q_OBJECT
public:
    explicit AnalysisPanel(QWidget* parent = nullptr);

    /// The project to analyse: its root (for cppcheck) and its build
    /// directory (for clang-tidy's compile_commands.json).
    void setProject(const QString& projectRoot, const QString& buildDirectory,
                    const QStringList& sourceFiles);

signals:
    void findingActivated(const QString& file, int line);
    void message(const QString& text);

private:
    void buildUi();
    void runAnalysis();
    void runNextTool();
    void finishTool();
    void appendFindings(const QList<LightCpp::Workbench::Finding>& findings);
    void rebuildTree();
    void clearResults();

    QCheckBox* cppcheckBox_ = nullptr;
    QCheckBox* tidyBox_ = nullptr;
    QCheckBox* scanBuildBox_ = nullptr;
    QCheckBox* pvsBox_ = nullptr;
    QCheckBox* coverityBox_ = nullptr;
    QCheckBox* complexityBox_ = nullptr;
    QComboBox* categoryBox_ = nullptr;
    QPushButton* runButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    /// The build command scan-build should wrap. Empty means the panel falls
    /// back to the last successful build command, and then to "make".
    QLineEdit* buildCommandEdit_ = nullptr;
    QLabel* summary_ = nullptr;

    QProcess* process_ = nullptr;
    QString projectRoot_;
    QString buildDirectory_;
    QStringList sourceFiles_;
    LightCpp::Workbench::AnalysisReport report_;
    /// The tools still to run, in order; each one's answer is appended.
    QStringList pendingTools_;
    QString runningTool_;
    QByteArray runningOutput_;
};
