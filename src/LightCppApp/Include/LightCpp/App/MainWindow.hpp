#pragma once

#include <QMainWindow>
#include <QHash>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QString>

#include "LightCpp/App/LanguageServerClient.hpp"
#include "LightCpp/Text/Encoding.hpp"

class CodeEditor;
class FileExplorer;
class OutputPanel;
class QLabel;
class QTabWidget;
class QComboBox;
class QAction;
class QActionGroup;
class QProcess;
class QTimer;
namespace LightCpp::App {
class LanguageServerClient;
}

/// Main IDE window — menu bar, toolbar, tabbed editors and a status bar
/// showing encoding / cursor / language.
///
/// The explorer and output panels are plain widgets embedded in nested
/// QSplitters, so the whole shell behaves as one layout: no panel can be
/// torn off into a floating window.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onNewFile();
    void onOpenFile();
    void onSaveFile();
    void onSaveAll();
    void onCloseTab(int index);
    void onFileActivated(const QString& path);
    void onCursorPositionChanged();
    void onEncodingChanged(int index);
    void onAbout();
    void onSyntaxCheckToggled(bool enabled);
    void onLintTimerTimeout();
    void onCompileCurrentFile();
    /// Compile the active file and, if the build succeeds, run the result.
    void onCompileAndRun();
    /// Run the binary produced by an earlier build. Asks whether to build
    /// first when nothing has been compiled yet.
    void onRunCurrentFile();
    /// Kill a program started by onRunCurrentFile() / onCompileAndRun().
    void onStopProgram();
    /// Jump to the source position of a Problems-list entry.
    /// `line` / `column` are 0-based.
    void onProblemActivated(const QString& filePath, int line, int column);

private:
    void createActions();
    void createMenuBar();
    void createToolBar();
    void createStatusBar();

    CodeEditor* currentEditor() const;
    CodeEditor* openFileInTab(const QString& path);
    CodeEditor* createEditorTab(const QString& title);
    void updateEncodingCombo(CodeEditor* editor);
    void setEncodingInfo(CodeEditor* editor, const QString& encodingName);
    /// Re-open the current editor's file with a forced encoding.
    void reloadWithEncoding(LightCpp::Text::TextEncoding encoding);
    /// Push the editor's current text to the language server (debounced).
    void scheduleLint(CodeEditor* editor);
    /// Rebuild the Output panel's Problems list from diagnosticsByFile_.
    void rebuildProblems();
    /// true when the file extension is handled by the language server.
    [[nodiscard]] static bool isCppLikeFile(const QString& path);
    /// LSP languageId for a path ("cpp", "c", ...).
    [[nodiscard]] static QString languageIdForFile(const QString& path);

    /// Source the compiler reads and binary it produces for one editor.
    /// Build and run share this, so "run" always looks exactly where the
    /// last build wrote (unsaved buffers are built from a temp copy).
    struct BuildTarget {
        QString srcPath;          // what the compiler is invoked on
        QString exePath;          // what the linker writes
        bool tempSource = false;  // true when srcPath lives in %TEMP%
    };
    [[nodiscard]] BuildTarget resolveBuildTarget(CodeEditor* editor) const;
    /// Launch `exePath` with its output captured into the Build Log.
    void startProgram(const QString& exePath, const QString& workDir);
    /// Enable/disable the build and run actions for the current states.
    void updateRunActionStates();

    // --- widgets ---
    QTabWidget* tabWidget_;
    FileExplorer* fileExplorer_;
    OutputPanel* outputPanel_;
    QComboBox* encodingCombo_;
    QLabel* statusCursor_;
    QLabel* statusLanguage_;
    QLabel* statusFile_;

    // --- actions ---
    QAction* actNew_;
    QAction* actOpen_;
    QAction* actSave_;
    QAction* actSaveAll_;
    QAction* actExit_;
    QAction* actAbout_;
    QAction* actOpenFolder_;
    QAction* actToggleExplorer_ = nullptr;  // show/hide the explorer panel
    QAction* actToggleOutput_ = nullptr;    // show/hide the output panel
    QAction* actSyntaxCheck_;
    QAction* actCompile_ = nullptr;        // single-file compile (F7)
    QAction* actCompileRun_ = nullptr;     // compile, then run (F5)
    QAction* actRun_ = nullptr;            // run the last build (Ctrl+F5)
    QAction* actStopRun_ = nullptr;        // kill the running program (Shift+F5)
    QAction* actToolchainGcc_ = nullptr;   // mutually exclusive with Clang
    QAction* actToolchainClang_ = nullptr;

    // --- syntax check (clangd over LSP) ---
    LightCpp::App::LanguageServerClient* lsp_;
    QTimer* lintDebounceTimer_;      // batches didChange notifications
    CodeEditor* pendingLintEditor_ = nullptr;
    /// Maps an LSP completion request id to the editor that issued it,
    /// so the async response can be routed back to the right popup.
    /// QPointer auto-nulls when the editor (tab) is destroyed.
    QMap<int, QPointer<CodeEditor>> pendingCompletions_;
    /// Id of the most recently issued completion request. Responses for
    /// older requests are stale (the prefix has moved on) and must be
    /// ignored — otherwise an outdated empty result dismisses the popup
    /// that a newer response just filled.
    int latestCompletionId_ = 0;

    /// Latest diagnostics per document, keyed by the editor's lint path
    /// (a virtual temp path for unsaved buffers). Feeds the Problems list.
    QHash<QString, QList<LightCpp::App::LspDiagnostic>> diagnosticsByFile_;

    // --- single-file compiler / runner ---
    QProcess* compiler_ = nullptr;   // lazily created; reused across builds
    QProcess* runner_ = nullptr;     // lazily created; reused across runs
    /// Set by onCompileAndRun(): run the produced binary once the build
    /// finishes successfully.
    bool runAfterBuild_ = false;
    /// Binary produced by the most recent build, i.e. what "Run" launches.
    QString lastBuildExe_;

    // new-file counter
    int untitledCounter_ = 0;
};
