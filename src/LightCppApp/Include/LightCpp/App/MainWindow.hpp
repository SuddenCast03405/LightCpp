#pragma once

#include <QJsonObject>
#include <QJsonValue>
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

#include "LightCpp/App/DebugViewsPanel.hpp"
#include "LightCpp/App/DebuggerController.hpp"
#include "LightCpp/App/ConsoleDecoder.hpp"
#include "LightCpp/App/LanguageServerClient.hpp"
#include "LightCpp/App/MsvcToolchain.hpp"
#include "LightCpp/App/OutputPanel.hpp"
#include "LightCpp/App/ProcessShutdown.hpp"
#include "LightCpp/App/ProcessTree.hpp"
#include "LightCpp/App/StartupPage.hpp"
#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/PluginManager.hpp"
#include "LightCpp/Plugin/PluginTypes.hpp"
#include "LightCpp/Project/BuildConfiguration.hpp"
#include "LightCpp/Project/CMakeProject.hpp"
#include "LightCpp/Project/GitStatus.hpp"
#include "LightCpp/Settings/SettingsStore.hpp"
#include "LightCpp/Text/Encoding.hpp"
#include "LightCpp/Text/TextFile.hpp"
#include "LightCpp/Workbench/EditorTools.hpp"
#include "LightCpp/Workbench/ExternalTools.hpp"
#include "LightCpp/Workbench/GitExtra.hpp"
#include "LightCpp/Workbench/PackageManager.hpp"
#include "LightCpp/Workbench/Viewers.hpp"
#include "LightCpp/Workbench/LspModel.hpp"
#include "LightCpp/Workbench/DebugSymbols.hpp"

#include <memory>
#include <optional>
#include <vector>

class CodeEditor;
class FileExplorer;
class FindBar;
class AnalysisPanel;
class GitBranchDialog;
class TestExplorerPanel;
class QFileSystemWatcher;
class QHBoxLayout;
class OutputPanel;
class QLabel;
class QListWidget;
class QDockWidget;
class QLineEdit;
class QMenu;
class QTreeWidget;
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
    void onProblemActivated(const QString& filePath, int line, int column,
                            bool compilerColumn);
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
    /// View > Preview Markdown / RST (Ctrl+Shift+M): render the active file
    /// through LightCppWorkbench and show it in a read-only dialog.
    void onMarkdownPreview();
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
    /// The directory holding the active kit's compiler, empty when no kit is
    /// selected or it names no C compiler. Put first on the build steps' PATH
    /// so CMake's compiler detection finds the kit's compiler instead of
    /// whichever one the IDE's own tool directory happens to hold.
    [[nodiscard]] QString kitToolchainDirectory() const;
    /// Wait for the background MSVC detection when it is still running and
    /// apply its result. Every path that needs msvcToolchain_ calls this
    /// first, so F7 and Build Project cannot disagree about the toolset.
    void ensureMsvcToolchainDetected();
    /// Drop the remembered project/single-file binaries. The files on disk
    /// were produced for the previous configuration (or kit), so running them
    /// would silently start the wrong build.
    void invalidateBuiltExecutables();

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
    /// Record whether this tab's encoding was detected or guessed, and say so
    /// in the output channel (R2-37).
    ///
    /// A guessed read and a verified one look identical on screen, yet only
    /// the guess is written back over the original bytes on the next save: a
    /// legacy code page re-encodes the whole file, and U+FFFD in place of the
    /// bytes that did not decode. The flag lives on the tab, the tooltip and
    /// the log carry it, and saveEditorToDisk() asks once before persisting it.
    void noteEncodingGuess(
        CodeEditor* editor,
        const LightCpp::Text::TextFileContent& content);
    /// Re-open the current editor's file with a forced encoding.
    void reloadWithEncoding(LightCpp::Text::TextEncoding encoding);
    /// Encode the editor's buffer for saving in the tab's own encoding
    /// (the encoding drop-down selection), including the CRLF translation
    /// and the UTF-16/32 byte-order mark. nullopt (after showing an error
    /// message) when the encoding cannot represent the text — the caller
    /// must not write a half-converted file in that case.
    [[nodiscard]] std::optional<QByteArray> encodedSavePayload(CodeEditor* editor);
    /// The line-ending style a save of this editor writes with: the
    /// per-tab status-bar override, else the style the file was loaded
    /// with, else the global files.lineEnding default. Both
    /// encodedSavePayload() and the post-save bookkeeping use it so the
    /// priority cannot drift apart.
    [[nodiscard]] QString effectiveLineEnding(CodeEditor* editor) const;
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
    void jumpToLocation(const QString& filePath, int line, int column,
                        bool compilerColumn = false);
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
    /// What Run Project starts for the open project: a backend's runProgram
    /// when it names one (an interpreter), else the built output. Empty when
    /// no project is open or the backend produced nothing runnable yet.
    [[nodiscard]] QString projectRunTarget() const;
    /// The directory the project lives in — what the Explorer shows while a
    /// project is open. Falls back to the descriptor's own directory.
    [[nodiscard]] QString projectRootDirectory() const;
    /// The active build tree, for tools that need to find compile_commands.json
    /// or write output next to the build. Empty when there is no CMake project.
    [[nodiscard]] QString buildDirectoryPath() const;
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
    /// `arguments` reach the program in every output mode (captured, console
    /// pauser, embedded/external terminal).
    void startProgram(const QString& exePath, const QString& workDir,
                      const QStringList& arguments = {});
    /// Create the embedded program runner (a merged-channel QProcess whose
    /// output goes through the run decoder) exactly once. Used both by the
    /// captured Program Output mode and by embedded terminal modes.
    QProcess* ensureProgramRunner();
    /// The embedded terminal session's shell process (cmd / PowerShell …),
    /// lazily created and separate from runner_: its raw bytes land in the
    /// dedicated terminal channel, not in Program Output.
    QProcess* ensureTerminalRunner();
    /// Begin watching the user program launched inside the embedded
    /// terminal: a Toolhelp32 poll finds the program among the shell's
    /// descendants and pins the pid it found, so "can run again" follows the
    /// program's lifetime, not the shell's (a `cmd /k` shell outlives the
    /// program by design — and the program outlives the shell once it is
    /// stopped, which is the half that used to go unnoticed).
    void startTerminalProgramWatch(const QString& exePath);
    /// One poll tick: update the embedded-terminal program-alive state.
    void pollTerminalProgram();
    /// Stop the four child processes the window itself starts — the
    /// single-file compiler, the embedded program runner, the project build
    /// and the terminal shell — in one bounded sweep (see
    /// App::stopProcesses for the policy).
    ///
    /// The debugger's process is deliberately not in here: it belongs to
    /// DebuggerController, which has its own stop() and its own teardown
    /// rules (see R2-45).
    ///
    /// Called from closeEvent once the close is certain. Left to the
    /// destructors instead, the project build and the terminal shell would
    /// only be killed during the QObject teardown: after the save prompts and
    /// after the language server's own shutdown, i.e. with the window already
    /// unresponsive and the build tools still holding their file locks.
    LightCpp::App::ProcessShutdownReport stopChildProcesses();
    /// Stop the embedded terminal's program before the shell that started
    /// it: the tree kill plus, for anything that refused to go, a line in
    /// the terminal saying which image stayed alive.
    void stopTerminalProgram();
    /// Launch `exePath` through ConsolePauser.exe in its own console window
    /// (preferences run.output = "consolePauser"). Nothing is captured; the
    /// console owns the program's input and output.
    void startWithConsolePauser(const QString& exePath, const QString& workDir,
                                const QStringList& arguments);
    /// Launch `exePath` inside a user-defined terminal from the editable
    /// list (preferences run.output = a terminal id). Embedded terminals are
    /// captured by the Program Output channel; non-embedded ones open their
    /// own console window. Either way the IDE keeps no input field.
    void startInTerminal(const QString& exePath, const QString& workDir,
                         const QStringList& arguments);
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
    /// The background MSVC detection finished: store it, report it in the
    /// build channel, and hand clangd the MSVC headers it implies.
    void onMsvcDetected(const LightCpp::App::Msvc::Toolchain& toolchain);

    // --- code-completion backends -----------------------------------------
    /// Route a completion request to the configured backend (clangd/ccls as
    /// language servers, libclang in-process). Shared by every editor tab.
    void requestCompletionFor(CodeEditor* editor, int line, int column,
                              const QString& triggerChar);
    /// Run the last completion request against libclang and show the popup.
    /// The parse itself runs on the completer's worker thread.
    void deliverLibClangCompletion();
    /// Push the buffer and ask the language server for a completion at the
    /// position remembered in `lastCompletion_`. The server must already be
    /// initialized; callers that have to wait for the handshake go through
    /// LanguageServerClient::whenReady() first (R2-08). Shared by the first
    /// attempt and the retry so both take the same protocol path
    /// (changeDocument before requestCompletion).
    void requestLspCompletion(CodeEditor* editor);
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
    /// Candidate libclang.dll locations, best first: the toolchain the IDE
    /// itself runs on (its libstdc++/libwinpthread are already in the
    /// process), the configured clangd directory, then the executable's
    /// directory. Empty when none exists.
    [[nodiscard]] QStringList resolveLibClangPaths() const;

    /// A completion request kept around so a failed one can be re-issued.
    struct PendingCompletion {
        QPointer<CodeEditor> editor;
        QString lintPath;
        int line = 0;
        int column = 0;
        QString triggerChar;
        /// Which user-initiated request this is. A retry keeps the generation it
        /// was scheduled under, so a queued (ready-edge) callback from an older
        /// request can tell that the user has typed on and dropped it — without
        /// this it would ask at a position nobody is looking at and spend the
        /// newer request's retry budget.
        quint64 generation = 0;
    };
    /// preferences.completion.*: which engine answers completions, and how
    /// many times a failed request is retried.
    QString completionBackend_ = QStringLiteral("clangd");
    int completionRetryCount_ = 2;
    int completionAttempts_ = 0;
    PendingCompletion lastCompletion_;
    /// Bumped for every request the user starts; see PendingCompletion.
    quint64 completionGeneration_ = 0;
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
    /// Class browser dock: types on the left, the selected type's members on
    /// the right. Kept separate from the outline on purpose — the outline
    /// answers "what is in this file", the class browser answers "what is in
    /// this type", and one list cannot be both without becoming noise.
    QDockWidget* classBrowserDock_ = nullptr;
    QLineEdit* classFilter_ = nullptr;
    QListWidget* classList_ = nullptr;
    QTreeWidget* classMembers_ = nullptr;
    QLabel* classDetail_ = nullptr;
    /// The full class list of the current document, filter applied at display
    /// time so typing does not re-ask the server.
    QList<LightCpp::Workbench::Lsp::ClassType> classTypes_;
    /// The documentSymbol request the class browser is currently waiting for.
    int classBrowserRequestId_ = 0;
    /// The document that request was about, so a stale answer for another
    /// file cannot populate the list.
    QString classBrowserFilePath_;
    /// Breadcrumb bar: the enclosing-symbol chain of the caret, one flat
    /// button per scope level.
    QWidget* breadcrumbBar_ = nullptr;
    QHBoxLayout* breadcrumbLayout_ = nullptr;
    /// Names of the last breadcrumb chain — rebuilding is skipped while the
    /// chain is unchanged (cursor moves fire per keystroke).
    QString breadcrumbKey_;
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
    QAction* actRenameSymbol_ = nullptr;       // Edit > Rename Symbol (Ctrl+Shift+R)
    QAction* actQuickFix_ = nullptr;           // Edit > Quick Fix (Ctrl+.)
    QAction* actSortIncludes_ = nullptr;       // Edit > Sort Includes
    QAction* actCallHierarchy_ = nullptr;      // Edit > Call Hierarchy
    QAction* actTypeHierarchy_ = nullptr;      // Edit > Type Hierarchy
    QAction* actPackageManager_ = nullptr;     // Tools > Package Manager
    /// Tools ▸ External Tools: generated from Preferences::externalTools.
    QMenu* externalToolsMenu_ = nullptr;
    QAction* actManageExternalTools_ = nullptr;
    QAction* actBlameFile_ = nullptr;          // Git > Blame File
    QAction* actDebugSymbols_ = nullptr;       // Debug > Symbol Settings...
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
    QAction* actGitBranches_ = nullptr;      // Git > Branches/Tags/Remotes... dialog
    QAction* actTestExplorer_ = nullptr;     // View > Test Explorer (Ctrl+Shift+T)
    QAction* actAnalyze_ = nullptr;          // Tools > Static Analysis (Ctrl+Shift+A)
    QAction* actCommandPalette_ = nullptr;   // Tools > Command Palette (Ctrl+Shift+P)
    QAction* actMarkdownPreview_ = nullptr;  // View > Preview Markdown (Ctrl+Shift+M)
    QAction* actClassBrowser_ = nullptr;     // View > Class Browser (Ctrl+Shift+B)
    QAction* actCallGraph_ = nullptr;        // View > Call Graph (Ctrl+Alt+G)

    // --- git state ---
    LightCpp::App::GitClient* git_ = nullptr;
    /// Branch / drift / dirty marker, at the right of the status bar.
    QLabel* statusGit_ = nullptr;
    /// Latest status, kept so commands can report drift without re-querying.
    LightCpp::Project::GitStatusSummary gitStatus_;
    /// The Git panel's widgets; nullptr until buildGitPanel() runs.
    QTreeWidget* gitPanelStaged_ = nullptr;
    QTreeWidget* gitPanelChanges_ = nullptr;
    QLabel* gitPanelBranch_ = nullptr;
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

    /// MSVC located at start-up, in the background. cl.exe cannot be launched
    /// bare — it needs INCLUDE/LIB pointing at both its own toolset and the
    /// Windows SDK, which normally vcvarsall.bat would set up. See
    /// MsvcToolchain.hpp; the detector must outlive the window because its
    /// worker thread emits through itself.
    LightCpp::App::Msvc::ToolchainDetector* msvcDetector_ = nullptr;
    LightCpp::App::Msvc::Toolchain msvcToolchain_;
    /// The detection result has been stored (and reported) once already.
    bool msvcDetectionApplied_ = false;
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
    /// Hover requests in flight: the editor that asked and where the tooltip
    /// belongs, keyed by request id.
    struct PendingHover {
        QPointer<CodeEditor> editor;
        QPoint position;
    };
    QMap<int, PendingHover> pendingHovers_;
    /// Render a decoded hover answer as the tooltip's rich text: the
    /// signature first, then the documentation, with the type hint called
    /// out when one can be derived.
    [[nodiscard]] QString formatHoverTooltip(
        const LightCpp::Workbench::Lsp::HoverInfo& hover) const;
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
    /// The newest in-flight semanticTokens request id. Semantic answers are
    /// per-file and arrive out of order while typing; only the newest one may
    /// repaint.
    int latestSemanticTokensRequestId_ = 0;
    /// Which file the in-flight semantic-token request described, so a late
    /// answer for the previous tab does not colour the current one.
    QString semanticTokensFilePath_;
    /// One flattened outline entry — the tree walked into a list so the
    /// Ctrl+Shift+O dialog can filter it without touching the tree widget.
    struct OutlineEntry {
        QString name;
        QString detail;
        int kind = 0;
        int line = 0;
        int endLine = 0;  // last line of the symbol's body (breadcrumb scope)
        int depth = 0;
    };
    /// Flattened symbols of the outline answer currently shown.
    QList<OutlineEntry> outlineEntries_;
    /// The file the outlineEntries_ describe. The breadcrumb must not show
    /// another tab's scope chain after a tab switch.
    QString outlineFilePath_;
    /// The newest in-flight rename / codeAction request ids.
    int latestRenameRequestId_ = 0;
    int latestCodeActionRequestId_ = 0;
    /// Quick fixes returned for the current position, in server order; the
    /// popup menu's index maps into this list.
    QList<LightCpp::App::LspCodeAction> pendingCodeActions_;
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
    /// Ask for the active editor's semantic tokens and push the decoded
    /// spans into it. Shares the outline's trigger (a settle after typing)
    /// because both need the same thing: the buffer already known to the
    /// server. A no-op when the server does not advertise the capability.
    void requestSemanticTokens();
    /// Decode one semanticTokens/full answer and hand it to the editor that
    /// asked for it. Drops the answer when the tab has moved on.
    void applySemanticTokens(const QString& filePath, const QJsonValue& result);

    // --- include sorting ----------------------------------------------------
    /// Rewrite the active buffer's top-level `#include` runs into the chosen
    /// order, as one undo step. Sorts each contiguous run and never crosses a
    /// blank line or a conditional, because those runs are usually deliberate
    /// ("system headers last within this block"). Reports what changed.
    void sortIncludesInEditor();
    /// The include order used by the command. Read from the environment's
    /// LIGHTCpp configuration when it names one, otherwise the C++ convention
    /// of "own header first, then project, then system".
    [[nodiscard]] LightCpp::Workbench::Editor::IncludeOrder includeOrderSetting() const;

    // --- call / type hierarchy ---------------------------------------------
    /// Ask the server for the call hierarchy of the symbol at the caret and
    /// show it in a dialog; picking an entry jumps to it.
    void showCallHierarchy();
    /// The same for the type hierarchy (base types and derived types).
    void showTypeHierarchy();

    // --- call graph ---------------------------------------------------------
    /// Walk the call hierarchy outwards from the symbol at the caret and show
    /// the result as a graph: a QTreeWidget of the calls as text, plus DOT
    /// written next to the source for anyone who wants a picture. The graph
    /// is a *neighbourhood*, not a whole-program call graph — the server only
    /// answers one call site at a time, and depth 2 is already hundreds of
    /// round trips.
    void showCallGraph();

    // --- class browser ------------------------------------------------------
    /// Show the classes of the active document with their members, in a
    /// dock that stays in sync with the active editor. Unlike the outline
    /// (one flat list of every symbol), this keeps *membership*: what belongs
    /// to `Foo` and not to the namespace that happens to precede it.
    void showClassBrowser();
    /// Ask the server for `filePath`'s documentSymbol and feed the class
    /// browser when the answer is the newest one for that document.
    void requestClassBrowser(const QString& filePath);
    /// Rebuild the class browser tree and repopulate the class list.
    void applyClassBrowser(const QString& filePath, const QJsonValue& result);
    /// Jump to a class or member row's position in the active editor.
    void jumpToClassBrowserItem(int line, int column);
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

    // --- workspace edits (rename / quick fix) -------------------------------
    /// Apply a flattened workspace-edit list to the open editors whose lint
    /// path matches (edits are grouped per document and applied bottom-up so
    /// earlier insertions don't shift later positions). Returns the number
    /// of applied edits; `skippedFiles` counts distinct files with edits but
    /// no open tab.
    int applyWorkspaceEdits(const QList<LightCpp::App::LspTextEdit>& edits,
                            int* skippedFiles = nullptr);
    /// Edit > Rename Symbol: prompt for the new name, ask clangd, apply.
    void renameSymbolAtCursor();
    /// Edit > Quick Fix: request codeActions for the diagnostics around the
    /// cursor; the answer pops a menu of fixes at the caret.
    void requestQuickFix();

    // --- breadcrumb ----------------------------------------------------------
    /// Rebuild the clickable scope chain (enclosing symbols of the caret)
    /// under the editor tabs. Hidden when there is no outline data. Skipped
    /// entirely when the chain did not change since the last rebuild.
    void updateBreadcrumb();

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
    /// the shell, the pid it was pinned to, the current aliveness, and the
    /// launch-time grace timer source. There is no "seen it once" flag any
    /// more: remembering the pid says the same thing and answers more.
    QString terminalProgramImage_;
    /// The pid the watch pinned the program to once it saw it, or 0 while it
    /// has not. Kept because the shell's lifetime is not the program's: once
    /// the shell is gone this is the only thing that can answer "is it still
    /// running?" — and it answers with the image too, so a reused pid reads
    /// as gone instead of as alive.
    unsigned long terminalProgramPid_ = 0;
    bool terminalProgramRunning_ = false;
    QElapsedTimer terminalProgramLaunch_;
    QTimer* terminalProgramTimer_ = nullptr;
    /// The terminal channel's stack/combo index once it has been opened.
    int terminalChannel_ = -1;
    /// The debug session (gdb / lldb / cdb). Created in the constructor.
    LightCpp::App::DebuggerController* debugger_ = nullptr;
    /// The inspection views (locals, stack, threads, registers, memory,
    /// disassembly) and the breakpoint model that feeds them.
    LightCpp::App::Debug::DebugViewsModel* debugViews_ = nullptr;
    LightCpp::App::Debug::BreakpointModel breakpoints_;
    DebugViewsPanel* debugPanel_ = nullptr;
    /// §十: the test explorer and §七: the static-analysis panel. Both are
    /// side-panel tabs; they stay alive for the whole session because their
    /// QProcess members survive a project switch.
    TestExplorerPanel* testPanel_ = nullptr;
    AnalysisPanel* analysisPanel_ = nullptr;
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
    /// The embedded terminal gets the streaming variant: its lines do not end
    /// (a prompt has no newline for minutes), so the line decoder's "wait for
    /// the terminator" would make the terminal look dead — but decoding each
    /// chunk raw would cut multi-byte characters at the seam (R2-41).
    LightCpp::App::ConsoleStreamDecoder terminalStreamDecoder_;
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
    /// `languageService.clangd.hover` — the same switch clangd itself uses.
    bool clangdHoverEnabled_ = true;
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
    /// The application font as it was before any preference touched it —
    /// what "(built-in default)" restores when the user un-picks a custom
    /// toolbar font. Captured before the first applyPreferences() runs.
    QFont defaultAppFont_;
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
    /// The resolved plugins directory, cached by loadPlugins() so every
    /// consumer — the panel list, the enable/disable/uninstall actions —
    /// uses exactly the directory the plugins were loaded from (R2-17).
    QString pluginDir_;
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
    /// The plugins directory in use: what loadPlugins() resolved, or — when
    /// that has not run yet — a fresh LightCpp::Plugin::resolvePluginsDirectory
    /// lookup. The panel list and the enable/disable/uninstall actions all
    /// go through here; each of them rebuilding `<exeDir>/plugins` by hand
    /// was R2-17, and running the raw build-tree exe then listed nothing.
    [[nodiscard]] QString pluginDirectory() const;

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
    void removePluginCommands(const QString& pluginName) override;
    /// The plugins-directory-relative paths of the disabled plugins plus
    /// the files still waiting to be deleted, as read by settings_.
    [[nodiscard]] LightCpp::Settings::Preferences pluginPrefsSnapshot() const;

    // --- side bar (the vertical Explorer / Git / Plugins activity bar) ------
    /// Git tab placeholder — the panel itself is not built yet.
    QWidget* createGitPlaceholder();

    // --- the Git side panel ------------------------------------------------
    /// Replace the "not built yet" placeholder with the real panel: branch
    /// header, staged/unstaged change lists, and the blame/blame-hover for
    /// the active file. The menu and status bar stay as they are; this is the
    /// view of the same data.
    void buildGitPanel(QWidget* panel);
    /// Push one status answer into the panel's lists.
    void refreshGitPanel(const LightCpp::Project::GitStatusSummary& summary);
    /// The blame view for the active buffer, opened in the output area's
    /// own channel: one row per block, double-click jumps to the line.
    void showBlameForActiveFile();
    /// Open the three-way conflict editor for `path` when git left markers in
    /// it; a no-op (with a status message) when the file has none.
    void openConflictResolution(const QString& path);

    // --- debugger symbol layer ----------------------------------------------
    /// The user's symbol settings, converted to the Workbench model. Read
    /// fresh each time so a settings change applies to the next session
    /// without a restart.
    [[nodiscard]] LightCpp::Workbench::Debug::SymbolSettings symbolSettings() const;
    /// Write one sub-object of the preferences document back to disk, leaving
    /// every other key untouched — the same surgical update
    /// persistGeneralPrefs() does, generalised so the debugger block does not
    /// need a second copy of the array-or-object dance.
    /// `section` may be an object or an array; the settings document is
    /// shallow-merged at the top level, so both shapes work the same way.
    bool persistPreferenceSection(const QString& key, const QJsonValue& section);
    /// Tools > Debugger Symbol Settings...: the form for the switches above.
    void showDebugSymbolSettings();
    /// Push the symbol-layer commands for the current backend into the live
    /// session. Called right after a session starts — after the inferior
    /// exists, before the first frame is read, which is the only window where
    /// pretty-printing takes effect on what the user is about to see.
    void applySymbolSettings();

    // --- package managers ---------------------------------------------------
    /// Tools > Package Manager...: pick a manager, search/install/remove a
    /// package, and copy the CMake flag that makes the build find it.
    void showPackageManager();

    // --- external tools (§九 tool.external) --------------------------------
    /// Rebuild the Tools ▸ External Tools submenu from the preferences. The
    /// list is user-editable, so the menu is generated rather than declared.
    void rebuildExternalToolMenu();
    /// Run one configured tool against the active file and selection.
    /// Asks first when the tool says so, and reports an unresolvable
    /// placeholder instead of running a command with a hole in it.
    void runExternalTool(int index);
    /// The dialog that adds, edits, reorders and removes tools.
    void showExternalToolsDialog();

    // --- hex / log / image viewers -----------------------------------------
    /// Open `path` in a read-only viewer tab when its content is better read
    /// that way than edited: a binary (hex dump), a log (level-coloured), an
    /// image (shown as an image). Returns nullptr when the file should go to
    /// the ordinary text editor instead, which is the caller's signal to fall
    /// through.
    ///
    /// The decision is made from the file's bytes, not its extension: a
    /// `.txt` full of NULs is a binary, and a `.log` written by an embedded
    /// target is still a log.
    QWidget* openInViewer(const QString& path);
    /// The Plugins tab: a list of every loaded plugin, one row per plugin
    /// with publisher / version / kind (C++ / Lua) tag chips.
    QWidget* createPluginPanel();
    /// Rebuild the plugin list from pluginManager_->loaded().
    void refreshPluginPanel();
};
