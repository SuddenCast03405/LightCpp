#pragma once

#include <QMainWindow>
#include <QFont>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QByteArray>

#include "LightCpp/App/DebuggerController.hpp"
#include "LightCpp/App/ConsoleDecoder.hpp"
#include "LightCpp/App/LanguageServerClient.hpp"
#include "LightCpp/App/MsvcToolchain.hpp"
#include "LightCpp/App/OutputPanel.hpp"
#include "LightCpp/App/StartupPage.hpp"
#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/PluginManager.hpp"
#include "LightCpp/Plugin/PluginTypes.hpp"
#include "LightCpp/Project/BuildConfiguration.hpp"
#include "LightCpp/Project/CMakeProject.hpp"
#include "LightCpp/Project/GitStatus.hpp"
#include "LightCpp/Settings/SettingsStore.hpp"
#include "LightCpp/Text/Encoding.hpp"

#include <memory>
#include <optional>

class CodeEditor;
class FileExplorer;
class FindBar;
class QFileSystemWatcher;
class OutputPanel;
class QLabel;
class QListWidget;
class QTabWidget;
class QComboBox;
class QAction;
class QActionGroup;
class QProcess;
class QPushButton;
class QTimer;
namespace LightCpp::App {
class BatchFormatter;
class CodeFormatter;
class GitClient;
class LanguageServerClient;
class LibClangCompleter;
}

/// Main IDE window — menu bar, toolbar, tabbed editors and a status bar
/// showing encoding / cursor / language.
///
/// The explorer and output panels are plain widgets embedded in nested
/// QSplitters, so the whole shell behaves as one layout: no panel can be
/// torn off into a floating window.
///
/// The window also implements LightCpp::Plugin::IdeServices: it is the
/// object handed to every plugin at initialize time, so the plugin-visible
/// IDE surface is exactly what that interface declares.
class MainWindow : public QMainWindow, public LightCpp::Plugin::IdeServices {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // --- LightCpp::Plugin::IdeServices (the plugin-facing IDE surface) -----
    void log(const QString& message) override;
    [[nodiscard]] QString activeEditorPath() const override;
    [[nodiscard]] QString editorText() const override;
    void setEditorText(const QString& text) override;
    void insertAtCursor(const QString& text) override;
    void gotoLine(int line) override;
    [[nodiscard]] QStringList actionIds() const override;
    [[nodiscard]] QString actionText(const QString& id) const override;
    void setActionText(const QString& id, const QString& text) override;
    [[nodiscard]] QStringList menuIds() const override;
    [[nodiscard]] QString menuTitle(const QString& id) const override;
    void setMenuTitle(const QString& id, const QString& title) override;
    void registerCommand(const QString& pluginName, const QString& id,
                         const QString& label, const QString& shortcut,
                         std::function<void()> callback) override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onNewFile();
    void onOpenFile();
    void onSaveFile();
    void onSaveAll();
    /// Auto-save tick: every modified, already-named buffer goes back to
    /// disk without any prompting.
    void onAutoSaveTimeout();
    /// A watched file changed (or vanished) on disk. Unmodified buffers
    /// reload silently; buffers with unsaved changes only get a note.
    void onWatchedFileChanged(const QString& path);
    void onCloseTab(int index);
    void onFileActivated(const QString& path);
    void onCursorPositionChanged();
    void onEncodingChanged(int index);
    void onAbout();
    void onSyntaxCheckToggled(bool enabled);
    void onLintTimerTimeout();
    /// Re-read the configuration files from disk and re-apply everything
    /// that derives from them (Tools > Reload Settings).
    void onReloadSettings();
    /// Open the settings dialog (Edit > Settings...). Accepted changes
    /// trigger a full reload + re-apply, exactly like Reload Settings.
    void onSettings();
    /// Start-up check gated by preferences.general.checkCompilersAtStartup:
    /// when the default kit's compilers cannot be found on disk, offer to
    /// open the settings. Runs once, from the event loop after show().
    void checkStartupCompilers();
    /// A recent entry activated on the start page: project descriptors
    /// (CMakeLists.txt, .dev, .vcxproj, ...) go to openProjectFile(),
    /// everything else to onFileActivated().
    void openFromRecent(const QString& path);
    void onCompileCurrentFile();
    /// Compile the active file and, if the build succeeds, run the result.
    void onCompileAndRun();
    /// Run the binary produced by an earlier build. Prompts to build first
    /// when nothing has been compiled yet, or when the binary is older than
    /// its source.
    void onRunCurrentFile();
    /// Kill a program started by onRunCurrentFile() / onCompileAndRun().
    void onStopProgram();
    // --- debugging ---------------------------------------------------------
    /// F9 on the current editor line: plant or remove a breakpoint.
    void onToggleBreakpoint();
    /// Launch the configured debugger on the last built binary.
    void onStartDebugging();
    /// Kill the debug session.
    void onStopDebugging();
    /// The target hit a breakpoint or signal: jump to the reported position.
    void onDebugPaused(const QString& file, int line);
    /// The debugger process ended: restore the action states.
    void onDebuggerStopped();
    /// Repaint every open editor's gutter from breakpointKeys_.
    void syncBreakpointMarkers();
    /// Debugger for the active kit (kit.debugger / the toolchain directory /
    /// the Windows SDK cdb for MSVC kits), or empty when none is found.
    [[nodiscard]] QString resolveDebuggerPath() const;
    /// Jump to the source position of a Problems-list entry.
    /// `line` / `column` are 0-based.
    void onProblemActivated(const QString& filePath, int line, int column);
    /// Reindent the active file through clang-format. The answer is async, so
    /// the request id is remembered and a late reply for a superseded buffer
    /// is dropped.
    void onFormatDocument();
    void onFormatted(int requestId, const QString& text);
    void onFormatFailed(int requestId, const QString& message);
    /// Ask for a folder and clang-format every C/C++ file under it. Above
    /// BatchFormatter::kWarnThreshold files a confirmation is asked first.
    void onFormatFolder();
    /// clang-format every C/C++ file under the open project's root.
    void onFormatProject();
    /// Shared body: collect files, apply the threshold, run the batch.
    /// Connected to the Explorer's context-menu request too, so the right-
    /// click path and the menu path land in the same place.
    void formatFolder(const QString& dir);
    void onBatchProgress(int done, int total, const QString& file,
                         bool success);
    void onBatchFinished(int requestId, int formatted, int failed,
                         const QStringList& failures);
    /// Ask for a project file (CMakeLists.txt or a Dev-C++ .dev) and open it.
    void onOpenProject();
    /// Ask for a folder. What happens next depends on what is in it: a folder
    /// holding a project descriptor is opened as that project, and anything
    /// else simply fills the Explorer. See openFolder().
    void onOpenFolder();
    /// Prepare the build tree. CMake needs this once per configuration; a
    /// .dev project is configured by the file itself, so it only reports.
    void onConfigureProject();
    void onBuildProject();
    void onRunProject();
    void onCloseProject();
    /// Load a project from an explicit path — the body of
    /// File > Open Project..., separated out so it takes a path rather than
    /// going through a dialog every time.
    ///
    /// `showInExplorer` points the Explorer at the project's own root
    /// directory. Opening a *file* leaves the user's chosen folder on screen
    /// (there is no folder to replace it with), but opening a *folder* that
    /// turns out to hold a project deeper down should show that project's
    /// root, which is what the user was really looking for.
    void openProjectFile(const QString& path, bool showInExplorer = false);
    /// Open a folder: show it in the Explorer, and if a project descriptor
    /// sits directly inside it, open that project too.
    void openFolder(const QString& dir);
    /// Edit > Search in Project: scan the Explorer root (or the active
    /// file's folder, or a folder the user picks) in a background thread
    /// and show the hits in a modeless dialog; activating a hit opens the
    /// file at that line.
    void onSearchInProject();
    /// Edit > Switch Header/Source: open the header (or source) that pairs
    /// with the active file — same basename, the matching extension family,
    /// looked up next to the file first.
    void onSwitchHeaderSource();
    /// Shared request builder for the three navigation menu actions:
    /// 0 = go to declaration, 1 = go to implementation, 2 = find references.
    void requestNavigation(int kind);
    // --- git ---------------------------------------------------------------
    void onGitRefresh();
    void onGitStageAll();
    void onGitUnstageAll();
    void onGitCommit();
    void onGitLog();
    void onGitInit();
    /// A status refresh answered: update the status bar and the Git pane.
    void onGitStatusReady(const LightCpp::Project::GitStatusSummary& summary);
    /// A git command answered: print its transcript.
    void onGitCommandFinished(const QString& title, const QString& text, int exitCode);

private:
    void createActions();
    /// Grey the clipboard actions out when no editor tab is current.
    void updateEditActions();
    /// Sync the line-ending combo with `editor`: the per-tab override when
    /// one is set, else what the buffer actually contains (LF vs CRLF), else
    /// the global save default for an empty buffer.
    void updateEolCombo(CodeEditor* editor);
    /// The line-ending combo changed: record the override on the active tab.
    void onEolChanged(int index);
    void createMenuBar();
    void createToolBar();
    void createStatusBar();
    /// The Kit drop-down at the right of the toolbar. Kits are the settings'
    /// build environments; the selected one decides the compiler, the flags
    /// and which headers clangd resolves against.
    QWidget* createKitWidget();
    /// The Debug/Release picker that sits at the far right of the toolbar.
    /// Owns configCombo_, so it has to be built before buildConfiguration()
    /// returns anything but the default.
    QWidget* createBuildConfigurationWidget();
    /// Rebuild the Kit combo from the loaded kits, keeping the selection
    /// when it still exists (default: the settings' defaultKitId).
    void syncKitUi();
    /// Rebuild the configuration combo from the active kit's
    /// buildConfigurations (Debug/Release when the kit defines none).
    void syncConfigCombo();
    /// The configuration currently selected in the toolbar, as the name the
    /// kit's buildConfigurations are keyed by. Also decides the flags of a
    /// single-file compile, so "Debug" means the same thing whether the file
    /// is built on its own or as part of a project.
    [[nodiscard]] QString currentBuildConfigurationName() const;
    /// The configuration currently selected, parsed back into the enum the
    /// project models (CMake/Dev-C++) understand.
    [[nodiscard]] LightCpp::Project::BuildConfiguration buildConfiguration() const;
    /// The kit currently selected in the toolbar, or nullopt when none is.
    [[nodiscard]] std::optional<LightCpp::Settings::Kit> activeKit() const;
    /// true when the active kit's C++ compiler is cl.exe (or the combo has
    /// no selection and the legacy default applies — it does not).
    [[nodiscard]] bool isMsvcKit(const LightCpp::Settings::Kit& kit) const;
    [[nodiscard]] bool activeKitIsMsvc() const;

    /// Load the configuration: write the starter files on first run, then
    /// read base + overlays. Runs before anything else reads the store.
    void loadSettings();
    /// Push the loaded preferences into the UI: fonts, encoding/line-ending
    /// defaults, large-file limits, language-service gates, tool paths,
    /// shortcuts. Safe to call again whenever the settings change.
    void applyPreferences();
    /// The clang-format fallback style implied by the settings and the
    /// active kit ("file" keeps the built-in default).
    [[nodiscard]] QString resolvedClangFormatFallbackStyle() const;
    /// git: defaults.gitPath, else the active kit's git.path, else the
    /// compiled-in location — and PATH when none of those exist.
    [[nodiscard]] QString resolvedGitPath() const;
    /// Point the settings at the current workspace's config overlay
    /// (project root /config) and re-read + re-apply. Called when a folder
    /// or project is opened or closed.
    void syncSettingsOverlays();
    /// The workspace's own config overlay directory — the open project's
    /// root /config — or empty when there is nowhere to overlay. Shared by
    /// syncSettingsOverlays() and the settings dialog.
    [[nodiscard]] QString overlayConfigDirectory() const;
    /// true when `path` is above `limitMB` (0 = no limit).
    [[nodiscard]] bool fileExceeds(const QString& path, int limitMB) const;
    /// Log a disabled feature for an oversized file, once per file+feature.
    void noteLargeFile(const QString& path, const QString& feature);

    // --- start page / recent files ------------------------------------------
    /// How many entries the recent list keeps. Older ones fall off.
    static constexpr int kMaxRecentFiles = 10;
    /// Open the start page as the first tab — it replaces the auto-opened
    /// "Untitled" editor when preferences.general.showStartupPage is on.
    void openStartupTab();
    /// Remove the start page tab (if it is still up) and delete it. Called
    /// once the user picked something to do from it.
    void closeStartupTab();
    // --- session restore ----------------------------------------------------
    /// Record the open file tabs (untitled buffers excepted) plus the active
    /// one into config/session.json. Called from closeEvent; restore happens
    /// in the constructor when preferences.general.restoreSession is on.
    void saveSessionState() const;
    /// Read back what saveSessionState() wrote: the file paths in tab order
    /// and the index of the tab that was active (-1 when none). Empty list
    /// when there is nothing usable (first run, preference off, unreadable).
    [[nodiscard]] QPair<QStringList, int> loadSessionState() const;
    /// The environment check plan for the start page: one row per toolchain
    /// (its compiler exists) and one per enabled kit (its toolchain
    /// reference resolves and its compiler exists).
    [[nodiscard]] QList<StartupPage::StartupCheckItem>
    buildStartupChecks() const;
    /// The start page tab while it is up; auto-nulls when the user closes
    /// the tab by hand.
    QPointer<StartupPage> startupTab_;
    /// Record a successfully opened file/project at the front of the recent
    /// list (deduplicated, capped) and persist it into preferences.json.
    void noteRecentFile(const QString& path);
    /// Write recentFiles / showStartupPage into the base layer's
    /// preferences.json ("general" key), leaving every other field
    /// untouched, then reload the store so the in-memory snapshot matches.
    bool persistGeneralPrefs(const QStringList& recentFiles,
                             bool showStartupPage);
    /// true when `path` names something the IDE opens as a project:
    /// CMakeLists.txt or a backend format (.dev/.vcxproj/.vcproj/.dsw/.dsp).
    /// Kept in step with FolderContents' descriptor list.
    [[nodiscard]] static bool isProjectDescriptorPath(const QString& path);

    CodeEditor* currentEditor() const;
    /// The tab title as shown, without the modification dot — for messages
    /// that name the file the user is looking at.
    [[nodiscard]] QString tabTitleFor(const CodeEditor* editor) const;
    CodeEditor* openFileInTab(const QString& path);
    CodeEditor* createEditorTab(const QString& title);
    void updateEncodingCombo(CodeEditor* editor);
    void setEncodingInfo(CodeEditor* editor, const QString& encodingName);
    /// Re-open the current editor's file with a forced encoding.
    void reloadWithEncoding(LightCpp::Text::TextEncoding encoding);
    /// Encode the editor's buffer for saving in the tab's own encoding
    /// (the encoding drop-down selection), including the CRLF translation
    /// and the UTF-16/32 byte-order mark. nullopt (after showing an error
    /// message) when the encoding cannot represent the text — the caller
    /// must not write a half-converted file in that case.
    [[nodiscard]] std::optional<QByteArray> encodedSavePayload(CodeEditor* editor);
    /// Disk-write half of saving, shared by Ctrl+S and the auto-save timer.
    [[nodiscard]] bool saveEditorToDisk(CodeEditor* editor);
    /// Start/stop watching `ed`'s file for external changes.
    void watchEditorFile(CodeEditor* ed, bool watch);
    /// Re-read `ed`'s file from disk (detected encoding), keeping folds.
    void reloadEditorFromDisk(CodeEditor* ed);
    /// Push the editor's current text to the language server (debounced).
    void scheduleLint(CodeEditor* editor);
    /// Show `filePath` at (line, column): focus the tab that already holds it
    /// (possibly an unsaved buffer under its virtual lint path) and only load
    /// from disk when no tab matches. Shared by the Problems list and by
    /// go-to-definition, so both land the caret identically.
    void jumpToLocation(const QString& filePath, int line, int column);
    /// Rebuild the Output panel's Problems list from diagnosticsByFile_.
    void rebuildProblems();
    // --- compiler problems (parsed from the compiler / build log) ---------
    /// Drop every compiler diagnostic and strip its editor markers
    /// (LSP squiggles are restored from diagnosticsByFile_). Called when a
    /// new build starts.
    void clearCompilerProblems();
    /// Split decoded compiler output into whole log lines and parse each;
    /// `tail` holds the unfinished last line between chunks. Shared by the
    /// single-file compiler and the project-build process.
    void consumeCompilerLines(const QString& text, QString& tail);
    /// Parse one compiler log line into the Problems (Compiler) list and
    /// refresh the editors' merged markers. Non-diagnostic lines are ignored.
    void processCompilerLogLine(const QString& line);
    /// Push LSP+compiler diagnostics into every open editor: the LSP list is
    /// the base layer, compiler entries are added only on lines the LSP does
    /// not already cover. Runs under applyingCompilerDiagnostics_ so the
    /// editors' diagnosticsChanged echo does not write back into the
    /// LSP-only store.
    void applyCompilerDiagnosticsToEditors();
    /// true when the file extension is handled by the language server.
    [[nodiscard]] static bool isCppLikeFile(const QString& path);
    /// LSP languageId for a path ("cpp", "c", ...).
    [[nodiscard]] static QString languageIdForFile(const QString& path);
    /// C/C++ source files under `dir`, excluding build trees and vendored
    /// dependencies. Sorted for stable log output.
    [[nodiscard]] static QStringList collectFormatFiles(const QString& dir);
    /// Re-read open, unmodified tabs whose files were just rewritten on
    /// disk, so the editor stays in step with the formatting that happened
    /// behind it. Modified buffers are left alone — the user's edits win.
    void reloadFormattedOpenTabs(const QStringList& paths);
    /// Paths of files open in a tab with unsaved changes — skipped on a
    /// disk-format batch so a later save does not throw the formatting away.
    [[nodiscard]] QSet<QString> openModifiedFilePaths() const;

    // --- open project -----------------------------------------------------
    /// One external command in a project build. A build is a short queue of
    /// these (configure, then build) run one after another, because an early
    /// step failing makes the later ones meaningless.
    struct ProjectStep {
        QString program;
        QStringList arguments;
        QString workingDirectory;
        QString label;  // echoed into the Build Log before the step runs
    };

    /// Which kind of project is open, if any.
    enum class ProjectKind { None, CMake, DevCpp };

    /// Convert plugin build steps into the internal queue's step type.
    [[nodiscard]] QList<ProjectStep>
    toProjectSteps(const QList<LightCpp::Plugin::BuildStep>& steps) const;
    /// Resolve the tool paths and configuration a project backend needs.
    [[nodiscard]] LightCpp::Plugin::BuildRequest makeBuildRequest() const;

    /// The steps that make up "build the open project" for the selected
    /// configuration, or an empty list with a reason in the Build Log.
    [[nodiscard]] QList<ProjectStep> projectBuildSteps();
    /// Start the queue produced by projectBuildSteps().
    void startProjectBuildSteps();
    /// Run the next queued step, or finish when the queue is empty.
    void runNextProjectStep();
    /// Tell the user the build ended and refresh what can be run.
    void finishProjectBuild(bool succeeded, int exitCode);
    /// Enable/disable the project actions for the current state.
    void updateProjectActions();
    /// Name for the window title and log lines.
    [[nodiscard]] QString projectDisplayName() const;
    /// The directory the project lives in — what the Explorer shows while a
    /// project is open. Falls back to the descriptor's own directory.
    [[nodiscard]] QString projectRootDirectory() const;
    /// Forget the open project and put the window back to having none. The
    /// Explorer is deliberately left alone: it is showing a directory the user
    /// chose, which outlives whichever project happened to be inside it.
    void clearProjectState();

    // --- git ---------------------------------------------------------------
    /// Directory git should run in: the open project, else the active file's
    /// directory, else the process's own. Git finds the repository itself.
    [[nodiscard]] QString gitWorkingDirectory() const;
    /// Point the git client at wherever the user is working now, and refresh
    /// the status if that is somewhere new.
    void syncGitWorkingDirectory();
    /// Print a status into the Git pane: one line by default, or the whole
    /// file list when the user asked for a refresh.
    void showGitStatus(const LightCpp::Project::GitStatusSummary& summary,
                       bool withFileList);
    /// `git diff` for the active file, or the whole tree when none is open.
    void showGitDiff(bool staged);
    /// Grey out what the current repository state cannot do.
    void setGitActionsEnabled();

    /// Source the compiler reads and binary it produces for one editor.
    /// Build and run share this, so "run" always looks exactly where the
    /// last build wrote (unsaved buffers are built from a temp copy).
    struct BuildTarget {
        QString srcPath;          // what the compiler is invoked on
        QString exePath;          // what the linker writes
        bool tempSource = false;  // true when srcPath lives in %TEMP%
    };
    [[nodiscard]] BuildTarget resolveBuildTarget(CodeEditor* editor) const;
    /// Launch `exePath`, capturing its output into the Program Output channel.
    void startProgram(const QString& exePath, const QString& workDir);
    /// Create the embedded program runner (a merged-channel QProcess whose
    /// output goes through the run decoder) exactly once. Used both by the
    /// captured Program Output mode and by embedded terminal modes.
    QProcess* ensureProgramRunner();
    /// The embedded terminal session's shell process (cmd / PowerShell …),
    /// lazily created and separate from runner_: its raw bytes land in the
    /// dedicated terminal channel, not in Program Output.
    QProcess* ensureTerminalRunner();
    /// Begin watching the user program launched inside the embedded
    /// terminal: a Toolhelp32 poll pairs the shell's PID with the program's
    /// image name, so "can run again" follows the program's lifetime, not
    /// the shell's (a `cmd /k` shell outlives the program by design).
    void startTerminalProgramWatch(const QString& exePath);
    /// One poll tick: update the embedded-terminal program-alive state.
    void pollTerminalProgram();
    /// True when a live process whose parent is `shellPid` carries
    /// `imageLower` (case-insensitive, e.g. "main.exe").
    [[nodiscard]] static bool shellHasChildImage(unsigned long shellPid,
                                                 const QString& imageLower);
    /// Launch `exePath` through ConsolePauser.exe in its own console window
    /// (preferences run.output = "consolePauser"). Nothing is captured; the
    /// console owns the program's input and output.
    void startWithConsolePauser(const QString& exePath, const QString& workDir);
    /// Launch `exePath` inside a user-defined terminal from the editable
    /// list (preferences run.output = a terminal id). Embedded terminals are
    /// captured by the Program Output channel; non-embedded ones open their
    /// own console window. Either way the IDE keeps no input field.
    void startInTerminal(const QString& exePath, const QString& workDir);
    /// The system's default console code page as an iconv name ("GBK" on
    /// zh-CN Windows). Console-backed run modes compile the program's
    /// narrow literals for it, since their output lands in a real console
    /// window that starts at that code page.
    [[nodiscard]] static QString systemConsoleCharset();
    /// The execution charset the current run mode's output needs: empty for
    /// captured Program Output (the panel probes each line, so any encoding
    /// displays), the system console charset for ConsolePauser and terminal
    /// modes.
    [[nodiscard]] QString requiredExecCharset() const;
    /// The ConsolePauser.exe to run a given program with: one next to the
    /// program, else one next to the IDE, else the compiled-in fallback
    /// location. When found elsewhere it is copied next to the program, so
    /// the pairing travels with the binary. Empty when none was found.
    [[nodiscard]] QString resolveConsolePauser(const QString& exePath) const;
    /// Detached start through `cmd /c start`: `start` always allocates a
    /// brand-new console window, so the child never inherits the console of
    /// whatever terminal the IDE itself was launched from (an MSYS2
    /// mintty/bash session included — plain startDetached would print the
    /// program's output into that terminal).
    bool startDetachedInNewConsole(const QString& program,
                                   const QStringList& args,
                                   const QString& workDir,
                                   const QProcessEnvironment& env);
    /// Enable/disable the build and run actions for the current states.
    void updateRunActionStates();
    /// Tell the language server whose headers to resolve includes against,
    /// following the selected toolchain, and restart it when that changes —
    /// clangd reads its include paths once, at start-up.
    void applyToolchainHeaders();

    // --- code-completion backends -----------------------------------------
    /// Route a completion request to the configured backend (clangd/ccls as
    /// language servers, libclang in-process). Shared by every editor tab.
    void requestCompletionFor(CodeEditor* editor, int line, int column,
                              const QString& triggerChar);
    /// Run the last completion request against libclang and show the popup.
    /// The parse itself runs on the completer's worker thread.
    void deliverLibClangCompletion();
    /// The libclang answer, back from the worker thread. Answers whose token
    /// is not the newest request describe text the caret has left behind and
    /// are dropped.
    void onLibClangCompleted(
        quint64 token, const QList<LightCpp::App::LspCompletionItem>& items,
        const QString& error);
    /// Schedule one re-issue of the most recent completion request. Returns
    /// false when the retry budget (preferences.completion.retryCount) is
    /// spent or nothing is pending.
    bool scheduleCompletionRetry();
    /// The LSP server executable for `backend` ("clangd" | "ccls"): the
    /// configured clangd path, else the same directory, else PATH. A bare
    /// file name comes back when nothing was found, so the failure surfaces
    /// in the server log with the path that was tried.
    [[nodiscard]] QString resolveCompletionServerPath(const QString& backend) const;
    /// Where libclang.dll lives (toolchain dir, exe dir), or empty.
    [[nodiscard]] QString resolveLibClangPath() const;

    /// A completion request kept around so a failed one can be re-issued.
    struct PendingCompletion {
        QPointer<CodeEditor> editor;
        QString lintPath;
        int line = 0;
        int column = 0;
        QString triggerChar;
    };
    /// preferences.completion.*: which engine answers completions, and how
    /// many times a failed request is retried.
    QString completionBackend_ = QStringLiteral("clangd");
    int completionRetryCount_ = 2;
    int completionAttempts_ = 0;
    PendingCompletion lastCompletion_;
    /// In-process completion engine (libclang.dll), used when the
    /// preferences name it as the backend. Created in the constructor.
    LightCpp::App::LibClangCompleter* libClang_ = nullptr;
    /// Token of the libclang request whose answer is still wanted; bumped for
    /// every new request so a slow parse cannot overwrite a newer popup.
    quint64 libclangToken_ = 0;
    quint64 libclangTokenCounter_ = 0;

    // --- widgets ---
    std::unique_ptr<LightCpp::Settings::SettingsStore> settings_;
    QTabWidget* tabWidget_;
    /// The vertical activity bar on the far left, VSCode style: the
    /// Explorer (the former sidebar), a Git placeholder and the plugin
    /// management panel.
    QTabWidget* sideTabs_ = nullptr;
    /// Outline page: the active document's symbol tree (documentSymbol).
    QTreeWidget* outlineTree_ = nullptr;
    QListWidget* pluginList_ = nullptr;
    /// The "this change needs a restart" banner above the plugin list plus
    /// its Restart IDE button. Shown only while a C++ plugin is disabled
    /// but still loaded — Lua plugins apply immediately.
    QWidget* restartBanner_ = nullptr;
    QPushButton* restartButton_ = nullptr;
    FileExplorer* fileExplorer_;
    OutputPanel* outputPanel_;
    /// Find/replace bar docked under the editor tabs; hidden until Ctrl+F.
    FindBar* findBar_ = nullptr;
    QComboBox* encodingCombo_;
    /// The line-ending drop-down (LF / CRLF) to the left of the encoding
    /// combo. Reflects the active buffer's line endings; picking one sets a
    /// per-tab override that encodedSavePayload() honours on the next save.
    QComboBox* eolCombo_ = nullptr;
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
    QAction* actReloadSettings_ = nullptr;  // re-read config/*.json
    QAction* actSettings_ = nullptr;        // edit config/*.json in a dialog
    QAction* actOpenConfigDir_ = nullptr;   // open the config folder in Explorer
    QAction* actToggleExplorer_ = nullptr;  // show/hide the explorer panel
    QAction* actToggleOutput_ = nullptr;    // show/hide the output panel
    QAction* actSyntaxCheck_;
    QAction* actCompile_ = nullptr;        // single-file compile (F7)
    QAction* actCompileRun_ = nullptr;     // compile, then run (F5)
    QAction* actRun_ = nullptr;            // run the last build (Ctrl+F5)
    QAction* actStopRun_ = nullptr;        // kill the running program (Shift+F5)
    QAction* actDebugStart_ = nullptr;     // start a debug session (F4)
    QAction* actDebugStop_ = nullptr;      // kill the debug session
    QAction* actDebugContinue_ = nullptr;  // resume after a breakpoint
    QAction* actDebugStepOver_ = nullptr;  // F10
    QAction* actDebugStepInto_ = nullptr;  // F11
    QAction* actDebugStepOut_ = nullptr;   // Shift+F11
    QAction* actToggleBreakpoint_ = nullptr;  // F9
    QAction* actFold_ = nullptr;           // collapse the block at the caret
    QAction* actUnfold_ = nullptr;         // expand the block around the caret
    QAction* actFoldAll_ = nullptr;
    QAction* actUnfoldAll_ = nullptr;
    QAction* actFind_ = nullptr;           // open the find bar (Ctrl+F)
    QAction* actFindNext_ = nullptr;       // F3
    QAction* actFindPrev_ = nullptr;       // Shift+F3
    QAction* actReplace_ = nullptr;        // open the bar in replace mode (Ctrl+H)
    QAction* actUndo_ = nullptr;           // Edit > Undo (Ctrl+Z)
    QAction* actRedo_ = nullptr;           // Edit > Redo (Ctrl+Y)
    QAction* actCut_ = nullptr;            // Edit > Cut (Ctrl+X)
    QAction* actCopy_ = nullptr;           // Edit > Copy (Ctrl+C)
    QAction* actPaste_ = nullptr;          // Edit > Paste (Ctrl+V)
    QAction* actSearchInProject_ = nullptr; // Edit > Search in Project (Ctrl+Shift+F)
    QAction* actSwitchHeaderSource_ = nullptr; // Edit > Switch Header/Source (Alt+O)
    QAction* actGotoDeclaration_ = nullptr;    // Edit > Go to Declaration (Ctrl+F12)
    QAction* actGotoImplementation_ = nullptr; // Edit > Go to Implementation (Ctrl+F11)
    QAction* actFindReferences_ = nullptr;     // Edit > Find References (Shift+F12)
    QAction* actGoToSymbol_ = nullptr;         // Edit > Go to Symbol in File (Ctrl+Shift+O)
    QAction* actToggleBookmark_ = nullptr;     // Edit > Toggle Bookmark (Ctrl+F2)
    QAction* actNextBookmark_ = nullptr;       // Edit > Next Bookmark (F2)
    QAction* actPrevBookmark_ = nullptr;       // Edit > Previous Bookmark (Shift+F2)
    QAction* actFormat_ = nullptr;         // clang-format the whole file
    QAction* actFormatFolder_ = nullptr;   // clang-format a folder
    QAction* actFormatProject_ = nullptr;  // clang-format the open project
    QAction* actOpenProject_ = nullptr;    // File > Open Project...
    QAction* actConfigureProject_ = nullptr;
    QAction* actBuildProject_ = nullptr;
    QAction* actRunProject_ = nullptr;
    QAction* actCloseProject_ = nullptr;
    QAction* actGitRefresh_ = nullptr;
    QAction* actGitStageAll_ = nullptr;
    QAction* actGitUnstageAll_ = nullptr;
    QAction* actGitCommit_ = nullptr;
    QAction* actGitLog_ = nullptr;
    QAction* actGitDiff_ = nullptr;
    QAction* actGitDiffStaged_ = nullptr;
    QAction* actGitInit_ = nullptr;

    // --- git state ---
    LightCpp::App::GitClient* git_ = nullptr;
    /// Branch / drift / dirty marker, at the right of the status bar.
    QLabel* statusGit_ = nullptr;
    /// Latest status, kept so commands can report drift without re-querying.
    LightCpp::Project::GitStatusSummary gitStatus_;
    bool inRepository_ = false;
    /// Set by an explicit refresh, so the next status answer prints the whole
    /// file list while an automatic one stays to a single line.
    bool gitStatusWantsFileList_ = false;
    /// Whether statusGit_ is currently part of the status bar's item list.
    /// removeWidget() does not reparent, so parentWidget() cannot tell —
    /// only this flag can, and re-inserting without it would double-insert.
    bool gitSlotInserted_ = false;

    // --- open project state ---
    ProjectKind projectKind_ = ProjectKind::None;
    std::optional<LightCpp::Project::CMakeProject> cmakeProject_;
    /// Summary of the plugin-imported (Dev-C++) project. The full model —
    /// units, flags, makefile choices — stays inside the project backend
    /// plugin; the IDE only ever sees this summary and the build steps the
    /// backend answers with.
    LightCpp::Plugin::ProjectSummary devSummary_;
    bool hasDevProject_ = false;
    /// The file the user opened; kept for the window title even when the
    /// project model itself only needs the directory.
    QString projectPath_;
    /// Process running the current project build step; lazily created.
    QProcess* projectProcess_ = nullptr;
    /// Steps still to run. The one at the front is in flight.
    QList<ProjectStep> projectSteps_;
    /// Binary the last project build produced, i.e. what "Run Project" starts.
    QString lastProjectExe_;

    /// MSVC located once at start-up. cl.exe cannot be launched bare — it needs
    /// INCLUDE/LIB pointing at both its own toolset and the Windows SDK, which
    /// normally vcvarsall.bat would set up. See MsvcToolchain.hpp.
    LightCpp::App::Msvc::Toolchain msvcToolchain_;
    /// Compiler launched by the most recent build, so the failure message can
    /// name the path that was actually missing.
    QString lastCompilerPath_;

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

    /// Same routing for textDocument/definition: request id -> editor.
    QMap<int, QPointer<CodeEditor>> pendingDefinitions_;
    /// Id of the most recently issued definition request. A slow answer for
    /// an earlier Ctrl+Click must not yank the view away from wherever the
    /// user has navigated since.
    int latestDefinitionId_ = 0;

    /// Outstanding declaration/implementation/references requests:
    /// request id -> kind (0 = declaration, 1 = implementation,
    /// 2 = references), plus the newest request id for staleness guarding.
    QMap<int, int> navigationKinds_;
    int latestLocationRequestId_ = 0;
    /// The newest in-flight signature-help request id; older answers are
    /// stale and dropped (same guard as latestCompletionId_).
    int latestSignatureHelpId_ = 0;
    /// The newest in-flight documentSymbol request id (staleness guard).
    int latestOutlineRequestId_ = 0;
    /// One flattened outline entry — the tree walked into a list so the
    /// Ctrl+Shift+O dialog can filter it without touching the tree widget.
    struct OutlineEntry {
        QString name;
        QString detail;
        int kind = 0;
        int line = 0;
        int depth = 0;
    };
    /// Flattened symbols of the outline answer currently shown.
    QList<OutlineEntry> outlineEntries_;
    /// Format one signature-help answer into rich text for the tooltip:
    /// the label HTML-escaped with the active parameter bolded. (A plain
    /// private method, not a slot — MOC would demand a metatype for the
    /// parameter here.)
    void showSignatureHelp(const LightCpp::App::LspSignatureHelp& help);

    // --- outline / document symbols -----------------------------------------
    /// Build the "Outline" page (a symbol tree for the active editor) and
    /// add it to the side bar. Called once from the constructor.
    QWidget* createOutlinePanel();
    /// Ask clangd for the active editor's symbol tree. No-op for
    /// non-C/C++-like files, unsaved-less paths or a stopped server.
    void requestOutline();
    /// Rebuild the outline tree and the flattened symbol cache from one
    /// documentSymbol answer — but only if it is the newest request AND
    /// still describes the active editor's document.
    void applyOutlineSymbols(const QString& filePath,
                             const QList<LightCpp::App::LspSymbol>& symbols);
    /// Move the active editor's cursor to a 0-based symbol line, scroll it
    /// into view and give it focus.
    void jumpToSymbolLine(int line);
    /// Ctrl+Shift+O: a small dialog filtering the flattened symbol list of
    /// the active document; picking an entry jumps to it.
    void showSymbolSearch();

    /// Latest diagnostics per document, keyed by the editor's lint path
    /// (a virtual temp path for unsaved buffers). Feeds the Problems list.
    QHash<QString, QList<LightCpp::App::LspDiagnostic>> diagnosticsByFile_;
    /// Compiler diagnostics parsed out of the last build's log — the
    /// Problems (Compiler) channel and the editors' second diagnostic layer.
    QList<ProblemEntry> compilerProblems_;
    /// Unfinished trailing log lines between output chunks (one per
    /// compiler process; a chunk can end mid-line).
    QString compilerLineTail_;
    QString projectLineTail_;
    /// Working directory of the build in flight — compiler log lines with
    /// relative paths are resolved against it.
    QString currentBuildWorkingDir_;
    /// Set while merged diagnostics are pushed INTO the editors, so the
    /// diagnosticsChanged echo is not stored back into diagnosticsByFile_.
    bool applyingCompilerDiagnostics_ = false;

    // --- single-file compiler / runner ---
    QProcess* compiler_ = nullptr;   // lazily created; reused across builds
    QProcess* runner_ = nullptr;     // lazily created; reused across runs
    /// The embedded terminal session's shell, separate from runner_: its
    /// raw output goes to the dedicated terminal channel.
    QProcess* terminalRunner_ = nullptr;
    /// Terminal-program lifetime watch: image name of the exe typed into
    /// the shell, whether it has been observed alive at least once, the
    /// current aliveness, and the launch-time grace timer source.
    QString terminalProgramImage_;
    bool terminalProgramSeen_ = false;
    bool terminalProgramRunning_ = false;
    QElapsedTimer terminalProgramLaunch_;
    QTimer* terminalProgramTimer_ = nullptr;
    /// The terminal channel's stack/combo index once it has been opened.
    int terminalChannel_ = -1;
    /// The debug session (gdb / lldb / cdb). Created in the constructor.
    LightCpp::App::DebuggerController* debugger_ = nullptr;
    /// Breakpoints as "normalizedPath:line" keys — survives tab switches and
    /// is replayed into every new debug session.
    QSet<QString> breakpointKeys_;
    /// Set by onCompileAndRun(): run the produced binary once the build
    /// finishes successfully.
    bool runAfterBuild_ = false;
    /// Binary produced by the most recent build, i.e. what "Run" launches.
    QString lastBuildExe_;
    /// Narrow-literal execution charset the last single-file build used
    /// ("" = the compiler's default). A console-backed run mode needs the
    /// system code page, so switching run modes invalidates the binary
    /// until it is rebuilt with the matching -fexec-charset.
    QString lastBuildExecCharset_;
    /// Console-output decoders (UTF-8 vs local code page probe): single-file
    /// compiler, project build steps, and the embedded program runner.
    LightCpp::App::ConsoleDecoder buildDecoder_;
    LightCpp::App::ConsoleDecoder projectBuildDecoder_;
    LightCpp::App::ConsoleDecoder runDecoder_;
    /// preferences.run.output — "program" (default), "consolePauser", or
    /// the id of a terminal from the editable list.
    QString programOutputMode_ = QStringLiteral("program");
    /// The selected terminal's fields, valid when programOutputMode_ refers
    /// to a terminal id (not "program"/"consolePauser").
    QString runTerminalName_;
    QString runTerminalCommand_;
    bool runTerminalEmbedded_ = true;
    /// Debug/Release, at the far right of the toolbar.
    QComboBox* configCombo_ = nullptr;
    /// The active kit, at the right of the toolbar next to it.
    QComboBox* kitCombo_ = nullptr;

    // --- applied settings (kept in sync by applyPreferences) --------------
    /// languageService.clangd.* gates.
    bool clangdEnabledByDefault_ = true;
    bool clangdCompletionEnabled_ = true;
    bool clangdDiagnosticsEnabled_ = true;
    bool clangdNavigationEnabled_ = true;
    /// languageService.clangFormat.* gates.
    bool clangFormatEnabled_ = true;
    bool formatOnSave_ = false;
    /// Raw style string ("file" = built-in default); resolved where used.
    QString clangFormatFallbackStyle_;
    /// performance.largeFile limits in MB (0 = no limit).
    int largeFileHighlightMB_ = 0;
    int largeFileIndexMB_ = 0;
    int largeFileParseMB_ = 0;
    /// files.longLineMode == "wrap": lines longer than the editor are broken
    /// at its edge instead of scrolling sideways.
    bool longLinesWrap_ = false;
    /// files.showWhitespaces / files.showIndentGuides, applied to every
    /// editor created later as well as to the open ones.
    bool editorsShowWhitespaces_ = false;
    bool editorsShowIndentGuides_ = false;
    /// general.autoSave — the tick writes every modified named buffer.
    QTimer* autoSaveTimer_ = nullptr;
    /// Watches every open editor's file for changes made outside the IDE.
    QFileSystemWatcher* fileWatcher_ = nullptr;
    /// When our own save fired a fileChanged, the event within this window
    /// (ms since the write) is ours and must not reload the buffer.
    QHash<QString, qint64> selfWriteMs_;
    /// files.encoding default for new buffers (canonical encoding name).
    QString defaultEncodingName_ = QStringLiteral("UTF-8");
    /// files.lineEnding resolved for this platform: "LF" or "CRLF".
    QString lineEndingForSaving_ = QStringLiteral("CRLF");
    /// fonts.editor, when the settings name one; applied to new tabs too.
    QFont editorFont_;
    /// One log line per oversized file+feature, not one per keystroke.
    QSet<QString> largeFileNoted_;

    // --- code formatting (clang-format) ---
    LightCpp::App::CodeFormatter* formatter_ = nullptr;
    /// Editor that asked for the format in flight, so the answer lands in the
    /// buffer it describes even if the user switched tabs meanwhile.
    QPointer<CodeEditor> formatTarget_;
    int latestFormatRequest_ = 0;

    // --- batch formatting (clang-format -i) ---
    LightCpp::App::BatchFormatter* batchFormatter_ = nullptr;
    /// Id of the batch in flight; a late `finished` for an earlier one is
    /// dropped so it cannot clobber the summary of the current run.
    int latestBatchRequestId_ = 0;
    /// Paths that were actually written on disk by the last batch — used to
    /// reload the open, clean tabs that match them.
    QStringList batchFormattedPaths_;

    // new-file counter
    int untitledCounter_ = 0;

    // --- plugins ------------------------------------------------------------
    /// Owns the loaded C++ plugins (QPluginLoader) and the Lua host's
    /// script adapters. Created in the constructor; loads once, no hot reload.
    LightCpp::Plugin::PluginManager* pluginManager_ = nullptr;
    /// The Plugins menu, created lazily by the first registered command.
    class QMenu* pluginsMenu_ = nullptr;
    /// Set by onRestartIde(): the accepted closeEvent spawns a fresh
    /// instance, so the unsaved-file prompts still run before the restart.
    bool restartPending_ = false;
    /// Set when a C++ plugin was enabled that is not running yet — it only
    /// loads at the next start, so the plugin panel offers the restart.
    bool cppReloadPending_ = false;
    /// Route a language-server / completion-backend log line to the Output
    /// channel of the backend it belongs to — Clangd Output, ccls Output or
    /// libclang Output — so the three never mix in one view.
    void appendLanguageServerLog(const QString& text);
    /// The Output channel matching the configured completion backend.
    [[nodiscard]] OutputPanel::Channel serverLogChannel() const;
    /// The Output channel of the language-server PROCESS (Clangd Output or
    /// ccls Output), decided by the executable rather than by the current
    /// backend setting — so a server's own messages, including its death, land
    /// in its own pane even after the user switches backends.
    [[nodiscard]] OutputPanel::Channel serverProcessLogChannel() const;
    void appendServerProcessLog(const QString& text);
    /// libclang's own chatter, always in the libclang Output pane.
    void appendLibClangLog(const QString& text);
    /// Look up an action by its objectName (the id plugins use).
    [[nodiscard]] QAction* findNamedAction(const QString& id) const;
    /// Discover and load plugins (shared libraries + Lua scripts) and log
    /// what came up. Called once, from the constructor.
    void loadPlugins();

    /// One plugin file in the plugins directory — loaded, disabled or
    /// simply unloadable. The plugin panel shows all three.
    struct PluginCandidate {
        QString relPath;      // relative to the plugins directory
        QString absolutePath;
        QString fileName;
        bool lua = false;
        bool loaded = false;
        bool disabled = false;
    };
    /// Scan the plugins directory for C++ dlls and Lua scripts and mark
    /// each one's state against the loaded set and the disable list.
    [[nodiscard]] QList<PluginCandidate> pluginCandidates() const;
    /// Write the disabled / pending-delete plugin lists into the base
    /// layer's preferences.json ("plugins" key), leaving every other
    /// document field untouched.
    bool persistPluginLists(const QStringList& disabled,
                            const QStringList& pendingDeletes);
    /// Disable / enable / uninstall one plugin by plugins-directory-relative
    /// path. Lua plugins apply immediately (hot unload / load); C++ plugins
    /// mark the change and need a restart, uninstalling a loaded dll
    /// defers the file deletion to the next start (the file is locked).
    void onPluginDisable(const QString& relPath);
    void onPluginEnable(const QString& relPath);
    void onPluginUninstall(const QString& relPath);
    /// Restart the IDE: spawn a fresh instance and close this one. The
    /// closeEvent save prompts still run — a cancelled close leaves the
    /// current instance alive and drops the restart as well.
    void onRestartIde();
    /// Drop the Plugins-menu commands that belong to one Lua plugin
    /// (object names "pluginCommand.<plugin>.<id>"). Hot unloading removes
    /// the script, so its commands must not linger until a restart.
    void removePluginCommands(const QString& pluginName);
    /// The plugins-directory-relative paths of the disabled plugins plus
    /// the files still waiting to be deleted, as read by settings_.
    [[nodiscard]] LightCpp::Settings::Preferences pluginPrefsSnapshot() const;

    // --- side bar (the vertical Explorer / Git / Plugins activity bar) ------
    /// Git tab placeholder — the panel itself is not built yet.
    QWidget* createGitPlaceholder();
    /// The Plugins tab: a list of every loaded plugin, one row per plugin
    /// with publisher / version / kind (C++ / Lua) tag chips.
    QWidget* createPluginPanel();
    /// Rebuild the plugin list from pluginManager_->loaded().
    void refreshPluginPanel();
};
