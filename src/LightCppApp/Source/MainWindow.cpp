// MainWindow — IDE shell implementation.

#include "LightCpp/App/MainWindow.hpp"
#include "LightCpp/App/BatchFormatter.hpp"
#include "LightCpp/App/CodeEditor.hpp"
#include "LightCpp/App/CodeFormatter.hpp"
#include "LightCpp/App/ConsoleDecoder.hpp"
#include "LightCpp/App/DefaultSettings.hpp"
#include "LightCpp/App/FileExplorer.hpp"
#include "LightCpp/App/FindBar.hpp"
#include "LightCpp/App/GitClient.hpp"
#include "LightCpp/App/OutputPanel.hpp"
#include "LightCpp/App/LspLocationsDialog.hpp"
#include "LightCpp/App/ProjectSearchDialog.hpp"
#include "LightCpp/App/LanguageServerClient.hpp"
#include "LightCpp/App/LibClangCompleter.hpp"
#include "LightCpp/App/MsvcToolchain.hpp"
#include "LightCpp/App/SettingsDialog.hpp"
#include "LightCpp/App/StartupPage.hpp"
#include "LightCpp/App/ToolchainPaths.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "LightCpp/Project/CompilerDiagnostics.hpp"

#include "LightCpp/Text/Converter.hpp"
#include "LightCpp/Text/Encoding.hpp"
#include "LightCpp/Text/TextFile.hpp"

#include "LightCpp/Lua/LuaPluginHost.hpp"

#include "LightCpp/Project/FolderContents.hpp"

#include <algorithm>
#include <functional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QFileDialog>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QMenu>
#include <QUrl>
#include <QInputDialog>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMetaType>
#include <QMessageBox>
#include <QPushButton>
#include <QPair>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QSet>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

// Required so QVariant can carry a TextEncoding in QComboBox item data.
Q_DECLARE_METATYPE(LightCpp::Text::TextEncoding)

namespace {

// The tool paths live in ToolchainPaths.hpp so the compiler, the language
// server and the formatter cannot disagree about where the toolchain is.
using LightCpp::App::kClangdPath;
using LightCpp::App::kMingwBin;

/// Where ConsolePauser.exe lives today on this machine — the last resort
/// when neither the program's directory nor the IDE's own has a copy. When
/// found here it is copied next to the program being run.
constexpr char kConsolePauserFallback[] = "E:/Cpp/ConsolePauser.exe";

/// How long the editor stays idle before the new text is pushed to the
/// language server (ms). 300ms keeps up with fast typing without spamming
/// clangd on every keystroke.
constexpr int kLintDebounceMs = 300;

/// Determine a human-readable language label from the file extension.
QString languageFromSuffix(const QString& path)
{
    const QString suf = QFileInfo(path).suffix().toLower();
    static const QHash<QString, QString> map = {
        {"cpp", "C++"}, {"cc", "C++"}, {"cxx", "C++"}, {"hpp", "C++"},
        {"h", "C/C++ Header"}, {"c", "C"},   {"ui", "Qt UI"},
        {"pro", "QMake"},      {"txt", "Text"}, {"md", "Markdown"},
        {"cmake", "CMake"},    {"py", "Python"}, {"lua", "Lua"},
        {"json", "JSON"},      {"xml", "XML"},
    };
    return map.value(suf, suf.isEmpty() ? QStringLiteral("Plain") : suf.toUpper());
}

/// All encodings the drop-down offers, in the order they should appear.
/// The first two slots (UTF-8 and the local ANSI code page) are pinned to
/// the top; the rest are alphabetical by display name.
struct ComboEntry {
    QString display;
    LightCpp::Text::TextEncoding encoding;
};

QList<ComboEntry> buildEncodingList()
{
    using namespace LightCpp::Text;
    QList<ComboEntry> list;
    // Slot 0: UTF-8.
    list.append({QString::fromLatin1(toName(TextEncoding::Utf8)),
                 TextEncoding::Utf8});
    // Slot 1: local ANSI code page, displayed as "ANSI (<name>)".
    list.append({localAnsiDisplayName(), localAnsiEncoding()});

    // Remaining encodings, sorted alphabetically by canonical name.
    const QList<TextEncoding> rest = {
        TextEncoding::Utf16Le, TextEncoding::Utf16Be,
        TextEncoding::Utf32Le, TextEncoding::Utf32Be,
        TextEncoding::Gb18030, TextEncoding::Gbk, TextEncoding::Big5,
        TextEncoding::ShiftJis, TextEncoding::EucKr,
        TextEncoding::Windows1252, TextEncoding::Latin1,
    };
    QList<ComboEntry> tail;
    for (const TextEncoding e : rest) {
        // Skip the local ANSI encoding if it already occupies slot 1.
        if (e == list[1].encoding) continue;
        tail.append({QString::fromLatin1(toName(e)), e});
    }
    std::sort(tail.begin(), tail.end(),
              [](const ComboEntry& a, const ComboEntry& b) {
                  return a.display.compare(b.display, Qt::CaseInsensitive) < 0;
              });
    list.append(tail);
    return list;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , settings_(std::make_unique<LightCpp::Settings::SettingsStore>())
    , tabWidget_(new QTabWidget(this))
    , fileExplorer_(new FileExplorer(this))
    , outputPanel_(new OutputPanel(this))
    , encodingCombo_(new QComboBox(this))
    , statusCursor_(new QLabel(this))
    , statusLanguage_(new QLabel(this))
    , statusFile_(new QLabel(this))
    , statusGit_(new QLabel(this))
    , lsp_(new LightCpp::App::LanguageServerClient(
          kClangdPath, QDir(QDir::currentPath()).absolutePath(), this))
    , lintDebounceTimer_(new QTimer(this))
{
    setWindowTitle(QStringLiteral("LightC++"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("accessories-text-editor")));

    // --- central widget: nested splitters ---------------------------------
    // The explorer and the output panel used to be QDockWidgets, which could
    // be dragged out into separate floating windows. They are plain widgets
    // now, arranged so the shell is a single fixed layout:
    //   +--------------+------------+
    //   | Side | Expl. | Editors    |   <- horizontal splitter; the side
    //   | tabs |       |            |      bar is a vertical tab widget
    //   +--------------+------------+      (Explorer/Git/Plugins)
    //   | Output                    |   <- vertical splitter
    //   +---------------------------+
    tabWidget_->setTabsClosable(true);
    tabWidget_->setMovable(true);
    tabWidget_->setDocumentMode(true);

    // The left-most activity bar, VSCode style: vertical tabs on the west
    // edge, one page each for the Explorer, Git and plugin management.
    sideTabs_ = new QTabWidget(this);
    sideTabs_->setObjectName(QStringLiteral("sideTabs"));
    sideTabs_->setTabPosition(QTabWidget::West);
    sideTabs_->setDocumentMode(true);
    sideTabs_->addTab(fileExplorer_, tr("Explorer"));
    sideTabs_->addTab(createGitPlaceholder(), tr("Git"));
    sideTabs_->addTab(createOutlinePanel(), tr("Outline"));
    sideTabs_->addTab(createPluginPanel(), tr("Plugins"));

    auto* mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->setChildrenCollapsible(false);  // panels can't be dragged shut

    // The editor area wraps the tab pages plus the find/replace bar, so the
    // bar can slide in under the tabs without moving the splitters.
    auto* editorArea = new QWidget(this);
    auto* editorLayout = new QVBoxLayout(editorArea);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    editorLayout->addWidget(tabWidget_);
    // Breadcrumb bar (hidden until an outline answer provides scopes).
    breadcrumbBar_ = new QWidget(editorArea);
    breadcrumbBar_->setObjectName(QStringLiteral("breadcrumbBar"));
    breadcrumbLayout_ = new QHBoxLayout(breadcrumbBar_);
    breadcrumbLayout_->setContentsMargins(6, 1, 6, 1);
    breadcrumbLayout_->setSpacing(0);
    breadcrumbLayout_->addStretch(1);
    breadcrumbBar_->hide();
    editorLayout->addWidget(breadcrumbBar_);
    findBar_ = new FindBar(editorArea);
    editorLayout->addWidget(findBar_);

    mainSplitter->addWidget(sideTabs_);
    mainSplitter->addWidget(editorArea);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes({260, 1020});

    auto* outerSplitter = new QSplitter(Qt::Vertical, this);
    outerSplitter->setChildrenCollapsible(false);
    outerSplitter->addWidget(mainSplitter);
    outerSplitter->addWidget(outputPanel_);
    outerSplitter->setStretchFactor(0, 1);
    outerSplitter->setStretchFactor(1, 0);
    outerSplitter->setSizes({600, 190});

    fileExplorer_->setMinimumWidth(160);
    sideTabs_->setMinimumWidth(160);
    outputPanel_->setMinimumHeight(80);

    setCentralWidget(outerSplitter);

    createActions();
    createMenuBar();
    createToolBar();
    createStatusBar();

    // --- syntax check wiring ---
    lintDebounceTimer_->setSingleShot(true);
    lintDebounceTimer_->setInterval(kLintDebounceMs);
    connect(lintDebounceTimer_, &QTimer::timeout,
            this, &MainWindow::onLintTimerTimeout);
    // Auto-save ticks only when the setting turns it on (applyPreferences).
    autoSaveTimer_ = new QTimer(this);
    autoSaveTimer_->setInterval(30 * 1000);
    connect(autoSaveTimer_, &QTimer::timeout,
            this, &MainWindow::onAutoSaveTimeout);
    // External file changes: unmodified buffers reload, modified ones get a
    // note instead of losing the user's edits.
    fileWatcher_ = new QFileSystemWatcher(this);
    connect(fileWatcher_, &QFileSystemWatcher::fileChanged,
            this, &MainWindow::onWatchedFileChanged);
    // The server's own log (stderr + window/logMessage) goes to its own
    // channel, so it never drowns out compiler output. The channel follows
    // the configured backend: Clangd Output, ccls Output or libclang Output.
    connect(lsp_, &LightCpp::App::LanguageServerClient::serverLog, this,
            [this](const QString& text) { appendServerProcessLog(text); });
    connect(lsp_, &LightCpp::App::LanguageServerClient::serverStarted, this,
            [this] {
        appendServerProcessLog(tr("language server started"));
        // Open the documents that are already in tabs. Use lintPath so
        // unsaved (untitled) buffers get their virtual path pushed too.
        for (int i = 0; i < tabWidget_->count(); ++i) {
            if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
                const QString lintPath = ed->lintPath();
                if (isCppLikeFile(lintPath)) {
                    lsp_->openDocument(lintPath, languageIdForFile(lintPath),
                                       ed->toPlainText());
                }
            }
        }
        requestOutline();  // the active tab gets its outline right away
    });
    connect(lsp_, &LightCpp::App::LanguageServerClient::diagnosticsReady,
            this, [this](const QString& filePath,
                         const QList<LightCpp::App::LspDiagnostic>& diags) {
        // languageService.clangd.diagnostics: the server still pushes, the
        // IDE just does not show what it does not want to see.
        if (!clangdDiagnosticsEnabled_) {
            return;
        }
        int errors = 0, warnings = 0;
        for (const auto& d : diags) {
            if (d.severity == 1) ++errors;
            else if (d.severity == 2) ++warnings;
        }
        for (int i = 0; i < tabWidget_->count(); ++i) {
            if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
                if (QFileInfo(ed->lintPath()).absoluteFilePath()
                        == QFileInfo(filePath).absoluteFilePath()) {
                    // Record the fresh LSP list first (the store stays
                    // LSP-only), then re-apply the merge so compiler markers
                    // survive a clangd push instead of being wiped by it.
                    const QString key = ed->lintPath();
                    if (diags.isEmpty()) {
                        diagnosticsByFile_.remove(key);
                    } else {
                        diagnosticsByFile_.insert(key, diags);
                    }
                    ed->setDiagnostics(diags);
                    applyCompilerDiagnosticsToEditors();
                    break;
                }
            }
        }
        // Throttle output: print only when the error/warning counts change.
        // clangd pushes fresh diagnostics after every keystroke, so
        // printing on every push floods the panel and slows the UI loop.
        static QHash<QString, QPair<int, int>> lastCounts;
        const QString key = QFileInfo(filePath).absoluteFilePath();
        const auto prev = lastCounts.value(key, qMakePair(-1, -1));
        if (prev.first != errors || prev.second != warnings) {
            lastCounts.insert(key, qMakePair(errors, warnings));
            appendLanguageServerLog(
                tr("%1: %2 error(s), %3 warning(s)")
                    .arg(QFileInfo(filePath).fileName())
                    .arg(errors).arg(warnings));
        }
    });
    connect(lsp_, &LightCpp::App::LanguageServerClient::completionReady,
            this, [this](int requestId, const QString& filePath,
                         const QList<LightCpp::App::LspCompletionItem>& items) {
        Q_UNUSED(filePath)
        // Ignore outdated responses: only the most recent request reflects
        // the user's current prefix. An older response (which may be empty,
        // e.g. one fired right before a trigger char) must neither replace
        // nor dismiss the popup a newer response just filled.
        if (requestId != latestCompletionId_) {
            pendingCompletions_.remove(requestId);
            return;
        }
        const auto it = pendingCompletions_.find(requestId);
        if (it == pendingCompletions_.end()) {
            return;
        }
        // QPointer auto-nulls when the editor (tab) was destroyed while
        // the request was in flight — drop the response in that case.
        if (CodeEditor* ed = it.value()) {
            if (items.isEmpty() && scheduleCompletionRetry()) {
                // The backend answered "nothing", which after a crash or a
                // restart is more often "not ready yet" than "no symbols".
                // A retry was scheduled — do not dismiss a popup for it.
                pendingCompletions_.erase(it);
                return;
            }
            ed->presentPopup(items);
        }
        pendingCompletions_.erase(it);
    });
    connect(lsp_, &LightCpp::App::LanguageServerClient::definitionReady,
            this, [this](int requestId, const QString& filePath,
                         const LightCpp::App::LspLocation& location) {
        Q_UNUSED(filePath)
        // Only the newest request may move the caret (see latestDefinitionId_).
        if (requestId != latestDefinitionId_) {
            pendingDefinitions_.remove(requestId);
            return;
        }
        pendingDefinitions_.remove(requestId);
        if (!location.isValid()) {
            // Common and expected: the click landed on whitespace, a keyword
            // or a local. Say so instead of looking like nothing happened.
            appendLanguageServerLog(
                tr("No definition found at that position."));
            return;
        }
        jumpToLocation(location.filePath, location.line, location.column);
    });
    connect(lsp_, &LightCpp::App::LanguageServerClient::locationsReady,
            this, [this](int requestId, const QString& filePath,
                         const QList<LightCpp::App::LspLocation>& locations) {
        Q_UNUSED(filePath)
        if (!navigationKinds_.contains(requestId)) {
            return;
        }
        const int kind = navigationKinds_.take(requestId);
        // Only the newest navigation request may move anything (same rule
        // as latestDefinitionId_).
        if (requestId != latestLocationRequestId_) {
            return;
        }
        if (locations.isEmpty()) {
            appendLanguageServerLog(
                tr("Nothing found at that position."));
            return;
        }
        // A single declaration/implementation target jumps right away;
        // references (or several targets) get the result list.
        if (kind != 2 && locations.size() == 1) {
            jumpToLocation(locations.first().filePath,
                           locations.first().line,
                           locations.first().column);
            return;
        }
        const QString title =
            kind == 0 ? tr("Go to Declaration")
                      : kind == 1 ? tr("Go to Implementation")
                                  : tr("Find References");
        auto* dialog = new LspLocationsDialog(title, locations, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &LspLocationsDialog::locationActivated,
                this, [this](const QString& path, int line1based) {
            if (openFileInTab(path) != nullptr) {
                // Dialog lines are 1-based; gotoLine() is 0-based.
                gotoLine(line1based - 1);
                if (CodeEditor* ed = currentEditor()) {
                    ed->setFocus();
                }
            }
        });
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });
    connect(lsp_, &LightCpp::App::LanguageServerClient::signatureHelpReady,
            this, [this](int requestId,
                         const LightCpp::App::LspSignatureHelp& help) {
        if (requestId != latestSignatureHelpId_) {
            return;  // stale: the caret has moved on
        }
        showSignatureHelp(help);
    });
    connect(lsp_,
            &LightCpp::App::LanguageServerClient::documentSymbolsReady,
            this, [this](int requestId, const QString& filePath,
                         const QList<LightCpp::App::LspSymbol>& symbols) {
        if (requestId != latestOutlineRequestId_) {
            return;  // a newer request is already in flight
        }
        applyOutlineSymbols(filePath, symbols);
    });
    connect(lsp_, &LightCpp::App::LanguageServerClient::renameReady,
            this, [this](int requestId,
                         const QList<LightCpp::App::LspTextEdit>& edits) {
        if (requestId != latestRenameRequestId_) {
            return;
        }
        if (edits.isEmpty()) {
            // The server refused (or found nothing) — say so instead of
            // leaving the user wondering whether the keystroke registered.
            statusBar()->showMessage(
                tr("Rename: the server cannot rename this symbol here"),
                5000);
            return;
        }
        int skipped = 0;
        const int applied = applyWorkspaceEdits(edits, &skipped);
        statusBar()->showMessage(
            applied > 0
                ? tr("Renamed: %1 edit(s) applied").arg(applied)
                : tr("Rename: no matching open document to edit"),
            5000);
        if (skipped > 0) {
            outputPanel_->appendProgramOutput(tr(
                "Rename skipped %1 file(s) that are not open in a tab")
                .arg(skipped));
        }
    });
    connect(lsp_, &LightCpp::App::LanguageServerClient::codeActionsReady,
            this, [this](int requestId,
                         const QList<LightCpp::App::LspCodeAction>& actions) {
        if (requestId != latestCodeActionRequestId_ || actions.isEmpty()) {
            return;
        }
        pendingCodeActions_ = actions;
        CodeEditor* ed = currentEditor();
        if (ed == nullptr) {
            return;
        }
        // Command-only actions (clangd's applyFix) cannot run: this client
        // does not execute server commands. Showing them would produce menu
        // entries that do nothing, so they are filtered out — with a hint
        // when that leaves the menu empty.
        int commandOnly = 0;
        for (const auto& action : actions) {
            if (action.edits.isEmpty()) {
                ++commandOnly;
            }
        }
        if (commandOnly == actions.size()) {
            statusBar()->showMessage(
                tr("Quick fix: the server only offered command-based fixes, "
                   "which are not supported"),
                5000);
            return;
        }
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        for (int i = 0; i < actions.size(); ++i) {
            if (actions[i].edits.isEmpty()) {
                continue;  // command-only — not executable here
            }
            QAction* fixAction = menu->addAction(actions[i].title);
            connect(fixAction, &QAction::triggered, this, [this, i] {
                if (i >= pendingCodeActions_.size()) {
                    return;
                }
                int skipped = 0;
                const int applied = applyWorkspaceEdits(
                    pendingCodeActions_[i].edits, &skipped);
                statusBar()->showMessage(
                    applied > 0
                        ? tr("Quick fix: %1 edit(s) applied").arg(applied)
                        : tr("Quick fix: nothing to apply"),
                    5000);
                if (skipped > 0) {
                    outputPanel_->appendProgramOutput(tr(
                        "Quick fix skipped %1 file(s) that are not open")
                        .arg(skipped));
                }
            });
        }
        menu->popup(ed->viewport()->mapToGlobal(
            ed->cursorRect().bottomLeft()));
    });
    connect(lsp_, &LightCpp::App::LanguageServerClient::serverFailed,
            this, [this](const QString& message) {
        appendServerProcessLog(tr("language server error: %1").arg(message));
        // Surface the reason instead of leaving the user with a silent pane.
        outputPanel_->showChannel(serverProcessLogChannel());
        actSyntaxCheck_->setChecked(false);
        // A completion was on its way: burn one retry attempt on bringing
        // the server back up. Bounded by preferences.completion.retryCount.
        if (completionBackend_ != QStringLiteral("libclang")) {
            scheduleCompletionRetry();
        }
    });

    // --- libclang (in-process completion engine) ---
    libClang_ = new LightCpp::App::LibClangCompleter(this);
    connect(libClang_, &LightCpp::App::LibClangCompleter::logMessage, this,
            [this](const QString& message) {
                // libclang's own chatter always belongs in the libclang pane,
                // whatever backend happens to be selected.
                appendLibClangLog(message);
            });
    connect(libClang_, &LightCpp::App::LibClangCompleter::completed, this,
            &MainWindow::onLibClangCompleted);

    // --- debugger session ---
    debugger_ = new LightCpp::App::DebuggerController(this);
    connect(debugger_, &LightCpp::App::DebuggerController::logLine,
            outputPanel_, &OutputPanel::appendDebugOutput);
    connect(debugger_, &LightCpp::App::DebuggerController::targetOutput,
            outputPanel_, &OutputPanel::appendDebugOutput);
    connect(debugger_, &LightCpp::App::DebuggerController::started, this, [this] {
        outputPanel_->showChannel(OutputPanel::ChannelDebug);
        outputPanel_->setDebuggerInputEnabled(true);
        updateRunActionStates();
    });
    connect(debugger_, &LightCpp::App::DebuggerController::paused,
            this, &MainWindow::onDebugPaused);
    connect(debugger_, &LightCpp::App::DebuggerController::stopped, this, [this] {
        outputPanel_->setDebuggerInputEnabled(false);
        onDebuggerStopped();
    });

    // --- connections ---
    connect(tabWidget_, &QTabWidget::tabCloseRequested,
            this, &MainWindow::onCloseTab);
    connect(fileExplorer_, &FileExplorer::fileActivated,
            this, &MainWindow::onFileActivated);
    // Right-click "Format This Folder" in the Explorer.
    connect(fileExplorer_, &FileExplorer::formatFolderRequested,
            this, &MainWindow::formatFolder);
    // Clicking a row in the Problems list jumps to the source location.
    connect(outputPanel_, &OutputPanel::problemActivated,
            this, &MainWindow::onProblemActivated);
    // A line typed into the Program Output channel goes to the running
    // program's stdin. The IDE's pipes do not echo typed input, so the line
    // is echoed into the transcript first — it reads like a terminal.
    connect(outputPanel_, &OutputPanel::programInputSubmitted,
            this, [this](const QString& text) {
        if (runner_ == nullptr || runner_->state() == QProcess::NotRunning) {
            outputPanel_->appendProgramOutput(
                tr("(no program is running — input was discarded)"));
            return;
        }
        outputPanel_->appendProgramOutput(text);
        // Console programs read their stdin in the local code page, which is
        // what a real terminal would have handed them.
        runner_->write(text.toLocal8Bit() + '\n');
    });
    // A line typed into the embedded terminal channel goes to the shell's
    // stdin as if typed at the keyboard (the shell itself echoes commands,
    // so the transcript needs no manual echo).
    connect(outputPanel_, &OutputPanel::terminalInputSubmitted,
            this, [this](const QString& text) {
        if (terminalRunner_ == nullptr
            || terminalRunner_->state() == QProcess::NotRunning) {
            outputPanel_->appendTerminalOutput(
                tr("(no terminal session is running — input was discarded)\n"));
            return;
        }
        terminalRunner_->write(text.toLocal8Bit() + QByteArray("\r\n"));
    });
    // A line typed into the Debugger channel goes straight to the debugger
    // process (GDB/MI, LLDB or cdb dialect — whatever the session speaks).
    connect(outputPanel_, &OutputPanel::debuggerInputSubmitted,
            this, [this](const QString& text) {
        if (debugger_ == nullptr || !debugger_->isRunning()) {
            outputPanel_->appendDebugOutput(
                tr("(no debug session is running — input was discarded)"));
            return;
        }
        outputPanel_->appendDebugOutput(text);
        debugger_->writeCommand(text);
    });
    connect(tabWidget_, &QTabWidget::currentChanged, this, [this](int) {
        // Whatever made this tab current — opening a file, switching or
        // closing tabs — the git pane must follow the active file, or a
        // non-repository directory keeps showing the previous repo's branch.
        syncGitWorkingDirectory();
        if (findBar_ != nullptr) {
            findBar_->setEditor(currentEditor());
        }
        if (auto* ed = currentEditor()) {
            statusLanguage_->setText(languageFromSuffix(ed->filePath()));
            statusFile_->setText(ed->filePath().isEmpty()
                                 ? QStringLiteral("Untitled") : ed->filePath());
            updateEncodingCombo(ed);
            updateEolCombo(ed);
            onCursorPositionChanged();
            requestOutline();  // outline follows the active tab
        } else {
            statusLanguage_->clear();
            statusFile_->clear();
            statusCursor_->clear();
            updateEolCombo(nullptr);
        }
        updateEditActions();
    });

    resize(1280, 800);
    updateProjectActions();  // nothing is open yet, so most of Project is greyed out

    // Git runs on demand: nothing polls the working tree behind the user's
    // back, so a repository changing underneath the IDE costs nothing.
    git_ = new LightCpp::App::GitClient(this);
    connect(git_, &LightCpp::App::GitClient::statusReady, this,
            &MainWindow::onGitStatusReady);
    connect(git_, &LightCpp::App::GitClient::commandFinished, this,
            &MainWindow::onGitCommandFinished);
    syncGitWorkingDirectory();

    // Locate MSVC in the background. cl.exe needs INCLUDE/LIB built by hand,
    // so the toolset and the Windows SDK are walked up front — but that walk
    // shells out to vswhere, whose worst case is tens of seconds, which used
    // to freeze the window before it was ever shown. onMsvcDetected() stores
    // the result and re-applies the clangd headers when it arrives; a build
    // that asks first waits for it (see the MSVC kit branch in the build
    // path) instead of reporting "no toolchain".
    msvcDetector_ = new LightCpp::App::Msvc::ToolchainDetector(this);
    connect(msvcDetector_, &LightCpp::App::Msvc::ToolchainDetector::detected,
            this, &MainWindow::onMsvcDetected);
    msvcDetector_->start();

    // Load the config (writing the starter files on first run) *before*
    // anything consumes it: fonts, gates and the Kit combo below all read
    // the loaded state, and applyPreferences() also points the language
    // server at the configured clangd.
    defaultAppFont_ = QApplication::font();  // before any preference touches it
    loadSettings();

    // Apply the loaded preferences (fonts, gates, paths, shortcuts) and fill
    // the Kit combo — all before the language server starts.
    applyPreferences();
    syncKitUi();
    applyToolchainHeaders();

    // Report where the settings live and anything the loader complained
    // about, so a typo in a config file is visible instead of silent.
    outputPanel_->appendBuildOutput(
        tr("Settings: %1")
            .arg(QDir::toNativeSeparators(
                LightCpp::Settings::SettingsStore::baseConfigDirectory())));
    for (const QString& error : settings_->lastErrors()) {
        outputPanel_->appendBuildOutput(tr("Settings: %1").arg(error));
    }

    // Syntax checking follows languageService.clangd.enabledByDefault; the
    // Tools menu entry stays checkable so it can still be switched off.
    actSyntaxCheck_->setChecked(clangdEnabledByDefault_);

    // Load C++ plugins and Lua scripts once, before the window shows: they
    // may customize menus (the zh-cn language pack does) and they fill the
    // plugin management panel.
    loadPlugins();

    // The start page takes the place of the auto-opened "Untitled" editor
    // tab; the old behaviour stays available behind the preference. With
    // session restore on and files recorded at last exit, those tabs come
    // back instead and the start page is skipped — there is something to
    // continue with.
    bool sessionRestored = false;
    if (settings_->preferences().restoreSession) {
        const QPair<QStringList, int> session = loadSessionState();
        int opened = 0;
        for (const QString& path : session.first) {
            if (openFileInTab(path) != nullptr) {
                ++opened;
            }
        }
        if (opened > 0) {
            sessionRestored = true;
            const int active = session.second;
            if (active >= 0 && active < tabWidget_->count()) {
                tabWidget_->setCurrentIndex(active);
            }
        }
    }
    if (!sessionRestored) {
        if (settings_->preferences().showStartupPage) {
            openStartupTab();
        } else {
            onNewFile();  // start with an empty editor
        }
    }
    updateProjectActions();

    // The start-up compiler check runs once the event loop is up: asking
    // from inside the constructor would open a dialog before the window is
    // visible.
    QTimer::singleShot(0, this, &MainWindow::checkStartupCompilers);
}

void MainWindow::checkStartupCompilers()
{
    const LightCpp::Settings::Preferences prefs = settings_->preferences();
    if (!prefs.checkCompilersAtStartup) {
        return;
    }
    // No usable default kit means nothing is configured yet — a first run
    // without any toolchain must not nag on every start.
    const std::optional<LightCpp::Settings::Kit> kit = settings_->defaultKit();
    if (!kit.has_value()) {
        return;
    }
    QStringList missing;
    for (const QString& path : {kit->cCompiler, kit->cppCompiler}) {
        if (!path.isEmpty() && !QFileInfo::exists(path)) {
            missing.append(QDir::toNativeSeparators(path));
        }
    }
    if (missing.isEmpty()) {
        return;
    }
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Missing compilers"),
        tr("The default kit's compiler(s) cannot be found:\n%1\n\n"
           "Open the settings to change it?")
            .arg(missing.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        onSettings();
    }
}

MainWindow::~MainWindow()
{
    // Plugins may hold state tied to the window (Lua states with callbacks,
    // cached widget pointers): shut them down while every widget is alive,
    // not from inside the QObject teardown.
    if (pluginManager_ != nullptr) {
        pluginManager_->shutdownPlugins();
    }
}

// ---------------------------------------------------------------------------
// Plugins (LightCpp::Plugin::IdeServices)
// ---------------------------------------------------------------------------
LightCpp::Settings::Preferences MainWindow::pluginPrefsSnapshot() const
{
    return settings_ != nullptr ? settings_->preferences()
                                : LightCpp::Settings::Preferences{};
}

bool MainWindow::persistPluginLists(const QStringList& disabled,
                                    const QStringList& pendingDeletes)
{
    const QString path =
        QDir(LightCpp::Settings::SettingsStore::baseConfigDirectory())
            .filePath(QStringLiteral("preferences.json"));
    // Surgical update: read the document, replace only the "plugins" key of
    // the preferences document, leave everything else exactly as it is.
    // The file may be a single document or an array of documents.
    QByteArray raw;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        raw = file.readAll();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    const auto apply = [](const QJsonObject& obj, const QStringList& dis,
                          const QStringList& pend) {
        QJsonArray disabledArr;
        for (const QString& p : dis) {
            disabledArr.append(p);
        }
        QJsonArray pendingArr;
        for (const QString& p : pend) {
            pendingArr.append(p);
        }
        QJsonObject plugins;
        plugins.insert(QStringLiteral("disabled"), disabledArr);
        plugins.insert(QStringLiteral("pendingDeletes"), pendingArr);
        QJsonObject out = obj;
        out.insert(QStringLiteral("plugins"), plugins);
        return out;
    };
    // Write the file and reload the store right away: the panel refresh and
    // the next enable/disable read their snapshot from settings_, so the
    // in-memory state must see the new lists, not the start-up copy.
    const auto save = [this, &path](const QJsonDocument& document) {
        const bool ok =
            LightCpp::Settings::SettingsStore::writeDocument(path, document);
        if (ok && settings_ != nullptr) {
            settings_->reload();
        }
        return ok;
    };
    if (doc.isArray()) {
        QJsonArray arr = doc.array();
        for (int i = 0; i < arr.size(); ++i) {
            const QJsonObject obj = arr.at(i).toObject();
            if (obj.value(QStringLiteral("type")).toString()
                == QLatin1String("preferences")) {
                arr[i] = apply(obj, disabled, pendingDeletes);
            }
        }
        return save(QJsonDocument(arr));
    }
    if (doc.isObject()) {
        return save(QJsonDocument(apply(doc.object(), disabled,
                                        pendingDeletes)));
    }
    // No usable document yet: create a minimal preferences document.
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("preferences"));
    return save(QJsonDocument(apply(obj, disabled, pendingDeletes)));
}

// ---------------------------------------------------------------------------
// Start page / recent files
// ---------------------------------------------------------------------------
void MainWindow::openStartupTab()
{
    if (startupTab_ != nullptr
        && tabWidget_->indexOf(startupTab_) >= 0) {
        // Already up (e.g. a settings reload ran this again): focus it.
        tabWidget_->setCurrentIndex(tabWidget_->indexOf(startupTab_));
        return;
    }
    startupTab_ = nullptr;  // stale pointer to a page that is going away
    auto* page =
        new StartupPage(settings_->preferences().recentFiles, tabWidget_);
    startupTab_ = page;
    tabWidget_->setCurrentIndex(tabWidget_->addTab(page, tr("Start Page")));
    connect(page, &StartupPage::recentActivated, this,
            &MainWindow::openFromRecent);
    connect(page, &StartupPage::newFileRequested, this, [this] {
        closeStartupTab();
        onNewFile();
    });
    connect(page, &StartupPage::openFileRequested, this, [this] {
        closeStartupTab();
        onOpenFile();
    });
    connect(page, &StartupPage::openProjectRequested, this, [this] {
        closeStartupTab();
        onOpenProject();
    });
    connect(page, &StartupPage::openFolderRequested, this, [this] {
        closeStartupTab();
        onOpenFolder();
    });
    // The environment check (preferences.general.checkToolchainsAtStartup)
    // runs one row per event-loop turn on the page itself, so the progress
    // line advances visibly.
    if (settings_->preferences().checkToolchainsAtStartup) {
        page->runChecks(buildStartupChecks());
    }
}

void MainWindow::closeStartupTab()
{
    StartupPage* page = startupTab_;
    startupTab_ = nullptr;
    if (page == nullptr) {
        return;
    }
    const int index = tabWidget_->indexOf(page);
    if (index >= 0) {
        tabWidget_->removeTab(index);
    }
    // removeTab does not delete: drop the stateless page right away.
    page->deleteLater();
}

// ---------------------------------------------------------------------------
// Session restore
// ---------------------------------------------------------------------------
void MainWindow::saveSessionState() const
{
    // Only real, still-existing files are restorable: untitled buffers have
    // no path to reopen, and a file deleted after the IDE opened it would
    // just log an error on every start.
    QStringList files;
    int active = -1;
    for (int i = 0; i < tabWidget_->count(); ++i) {
        if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
            const QString path = ed->filePath();
            if (path.isEmpty() || !QFileInfo::exists(path)) {
                continue;
            }
            if (i == tabWidget_->currentIndex()) {
                active = files.size();
            }
            files.append(path);
        }
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("session"));
    QJsonArray filesArr;
    for (const QString& path : files) {
        filesArr.append(path);
    }
    obj.insert(QStringLiteral("files"), filesArr);
    obj.insert(QStringLiteral("active"), active);

    const QString sessionPath =
        LightCpp::Settings::SettingsStore::baseConfigDirectory()
        + QStringLiteral("/session.json");
    QDir().mkpath(
        LightCpp::Settings::SettingsStore::baseConfigDirectory());
    LightCpp::Settings::SettingsStore::writeDocument(
        sessionPath, QJsonDocument(obj));
}

QPair<QStringList, int> MainWindow::loadSessionState() const
{
    const QString sessionPath =
        LightCpp::Settings::SettingsStore::baseConfigDirectory()
        + QStringLiteral("/session.json");
    QFile file(sessionPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        return {};
    }
    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("type")).toString()
        != QLatin1String("session")) {
        return {};
    }
    QStringList files;
    const QJsonArray filesArr = obj.value(QStringLiteral("files")).toArray();
    for (const QJsonValue& value : filesArr) {
        const QString path = value.toString();
        if (!path.isEmpty()) {
            files.append(path);
        }
    }
    return {files, obj.value(QStringLiteral("active")).toInt(-1)};
}

QList<StartupPage::StartupCheckItem> MainWindow::buildStartupChecks() const
{
    using Item = StartupPage::StartupCheckItem;
    QList<Item> items;

    // One row per toolchain: the C++ compiler (g++/clang++/cl) is the one
    // that matters; any other compiler the toolchain carries names the
    // row when there is nothing else.
    for (const LightCpp::Settings::Toolchain& toolchain : settings_->toolchains()) {
        Item item;
        item.label = tr("Toolchain %1").arg(toolchain.name.isEmpty()
                                                ? toolchain.id
                                                : toolchain.name);
        QString detail = toolchain.compilers.value(QStringLiteral("g++"));
        if (detail.isEmpty()) {
            detail = toolchain.compilers.value(QStringLiteral("clang++"));
        }
        if (detail.isEmpty()) {
            detail = toolchain.compilers.value(QStringLiteral("cl"));
        }
        if (detail.isEmpty()) {
            detail = toolchain.compilers.value(QStringLiteral("gcc"));
        }
        if (detail.isEmpty()) {
            detail = toolchain.compilers.value(QStringLiteral("clang"));
        }
        item.detail = detail;
        item.requiresFile = true;
        if (item.detail.isEmpty()) {
            item.requiresFile = false;
            item.detail = tr("no compiler configured");
        }
        items.append(item);
    }

    // One row per enabled kit. kits() already resolved inheritToolchain,
    // so an unresolved toolchain reference shows up as empty compilers —
    // but naming the missing id is far more useful, so check it first.
    for (const LightCpp::Settings::Kit& kit : settings_->kits()) {
        if (!kit.enabled) {
            continue;
        }
        Item item;
        item.label = tr("Kit %1").arg(kit.name.isEmpty() ? kit.id : kit.name);
        if (kit.inheritToolchain && !kit.toolchainId.isEmpty()
            && !settings_->toolchain(kit.toolchainId).has_value()) {
            item.requiresFile = false;
            item.detail = tr("toolchain '%1' not found").arg(kit.toolchainId);
        } else if (kit.cppCompiler.isEmpty() && kit.cCompiler.isEmpty()) {
            item.requiresFile = false;
            item.detail = tr("no compiler configured");
        } else {
            item.detail = !kit.cppCompiler.isEmpty() ? kit.cppCompiler
                                                     : kit.cCompiler;
            item.requiresFile = true;
        }
        items.append(item);
    }
    return items;
}

void MainWindow::openFromRecent(const QString& path)
{
    closeStartupTab();
    if (isProjectDescriptorPath(path)) {
        openProjectFile(path, true);
    } else {
        onFileActivated(path);
    }
}

bool MainWindow::isProjectDescriptorPath(const QString& path)
{
    const QFileInfo info(path);
    if (info.fileName().compare(QStringLiteral("CMakeLists.txt"),
                                Qt::CaseInsensitive) == 0) {
        return true;
    }
    // In step with FolderContents' descriptor search: the formats the
    // project-backend plugins claim.
    const QString suffix = info.suffix().toLower();
    return suffix == QLatin1String("dev") || suffix == QLatin1String("vcxproj")
        || suffix == QLatin1String("vcproj") || suffix == QLatin1String("dsw")
        || suffix == QLatin1String("dsp");
}

void MainWindow::noteRecentFile(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        return;  // untitled buffers and virtual lint paths never land here
    }
    const QString absolute = info.absoluteFilePath();
    const LightCpp::Settings::Preferences prefs = settings_->preferences();
    QStringList recent = prefs.recentFiles;
    recent.removeAll(absolute);
    recent.prepend(absolute);
    while (recent.size() > kMaxRecentFiles) {
        recent.removeLast();
    }
    persistGeneralPrefs(recent, prefs.showStartupPage);
}

bool MainWindow::persistGeneralPrefs(const QStringList& recentFiles,
                                     bool showStartupPage)
{
    const QString path =
        QDir(LightCpp::Settings::SettingsStore::baseConfigDirectory())
            .filePath(QStringLiteral("preferences.json"));
    // Surgical update: read the document, replace only the "general" key of
    // the preferences document, leave everything else exactly as it is.
    // The file may be a single document or an array of documents.
    QByteArray raw;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        raw = file.readAll();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    QJsonArray recentArray;
    for (const QString& entry : recentFiles) {
        recentArray.append(entry);
    }
    const auto apply = [&recentArray, showStartupPage](const QJsonObject& obj) {
        QJsonObject general = obj.value(QLatin1String("general")).toObject();
        // Keep the flag this helper does not manage exactly where it was.
        if (!general.contains(QLatin1String("checkCompilersAtStartup"))) {
            general.insert(QStringLiteral("checkCompilersAtStartup"), true);
        }
        general.insert(QStringLiteral("recentFiles"), recentArray);
        general.insert(QStringLiteral("showStartupPage"), showStartupPage);
        QJsonObject out = obj;
        out.insert(QStringLiteral("general"), general);
        return out;
    };
    const auto save = [this, &path](const QJsonDocument& document) {
        const bool ok =
            LightCpp::Settings::SettingsStore::writeDocument(path, document);
        if (ok && settings_ != nullptr) {
            settings_->reload();
        }
        return ok;
    };
    if (doc.isArray()) {
        QJsonArray arr = doc.array();
        for (int i = 0; i < arr.size(); ++i) {
            const QJsonObject obj = arr.at(i).toObject();
            if (obj.value(QStringLiteral("type")).toString()
                == QLatin1String("preferences")) {
                arr[i] = apply(obj);
            }
        }
        return save(QJsonDocument(arr));
    }
    if (doc.isObject()) {
        return save(QJsonDocument(apply(doc.object())));
    }
    // No usable document yet: create a minimal preferences document.
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("preferences"));
    return save(QJsonDocument(apply(obj)));
}

QList<MainWindow::PluginCandidate> MainWindow::pluginCandidates() const
{
    using LoadedKind = LightCpp::Plugin::PluginManager::Loaded::Kind;
    QList<PluginCandidate> candidates;
    const QString pluginDir =
        QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    const auto loadedList =
        pluginManager_ != nullptr ? pluginManager_->loaded()
                                  : QList<LightCpp::Plugin::PluginManager::Loaded>{};
    const auto isLoaded = [&loadedList](const QString& absolute,
                                        LoadedKind kind) {
        for (const auto& entry : loadedList) {
            if (entry.kind == kind
                && QDir::fromNativeSeparators(entry.path)
                       == QDir::fromNativeSeparators(absolute)) {
                return true;
            }
        }
        return false;
    };
    const LightCpp::Settings::Preferences prefs = pluginPrefsSnapshot();

    QDir dir(pluginDir);
    for (const QString& file : dir.entryList({QStringLiteral("*.dll")},
                                             QDir::Files,
                                             QDir::Name | QDir::IgnoreCase)) {
        PluginCandidate c;
        c.relPath = file;
        c.absolutePath = dir.filePath(file);
        c.fileName = file;
        c.lua = false;
        c.loaded = isLoaded(c.absolutePath, LoadedKind::Cpp);
        c.disabled = prefs.disabledPlugins.contains(c.relPath,
                                                    Qt::CaseInsensitive);
        candidates.append(c);
    }
    QDir luaDir(dir.filePath(QStringLiteral("lua")));
    for (const QString& file : luaDir.entryList({QStringLiteral("*.lua")},
                                                QDir::Files,
                                                QDir::Name | QDir::IgnoreCase)) {
        PluginCandidate c;
        c.relPath = QStringLiteral("lua/") + file;
        c.absolutePath = luaDir.filePath(file);
        c.fileName = file;
        c.lua = true;
        c.loaded = isLoaded(c.absolutePath, LoadedKind::Lua);
        c.disabled = prefs.disabledPlugins.contains(c.relPath,
                                                    Qt::CaseInsensitive);
        candidates.append(c);
    }
    return candidates;
}

void MainWindow::onPluginDisable(const QString& relPath)
{
    const LightCpp::Settings::Preferences prefs = pluginPrefsSnapshot();
    if (prefs.disabledPlugins.contains(relPath, Qt::CaseInsensitive)) {
        return;
    }
    QStringList disabled = prefs.disabledPlugins;
    disabled.append(relPath);

    const QString absolute = QDir(QCoreApplication::applicationDirPath()
                                  + QStringLiteral("/plugins"))
                                 .filePath(relPath);
    // Lua plugins can leave right now; shared-library plugins keep running
    // until the restart (their dll stays mapped into this process).
    const QString normalized = QDir::fromNativeSeparators(absolute);
    if (pluginManager_ != nullptr
        && pluginManager_->unloadRegistered(normalized)) {
        // The script is gone, so its Plugins-menu entries must go with it:
        // a lingering action would call back into a closed Lua state.
        removePluginCommands(QFileInfo(normalized).completeBaseName());
        outputPanel_->appendPluginOutput(
            tr("Plugin disabled (unloaded): %1").arg(relPath));
    } else {
        outputPanel_->appendPluginOutput(
            tr("Plugin disabled — takes effect after a restart: %1")
                .arg(relPath));
    }
    // A C++ plugin just went back onto the disable list; whatever the
    // earlier "enable" promised, this disable is what the next start
    // applies (and a still-running one is covered by the loaded check in
    // refreshPluginPanel).
    cppReloadPending_ = false;
    persistPluginLists(disabled, prefs.pendingPluginDeletes);
    refreshPluginPanel();
}

void MainWindow::onPluginEnable(const QString& relPath)
{
    const LightCpp::Settings::Preferences prefs = pluginPrefsSnapshot();
    QStringList disabled;
    for (const QString& p : prefs.disabledPlugins) {
        if (p.compare(relPath, Qt::CaseInsensitive) != 0) {
            disabled.append(p);
        }
    }
    if (disabled.size() == prefs.disabledPlugins.size()) {
        return;  // not on the disable list
    }
    persistPluginLists(disabled, prefs.pendingPluginDeletes);

    // A Lua plugin comes back right away: load and register it now.
    if (relPath.startsWith(QLatin1String("lua/"), Qt::CaseInsensitive)
        && pluginManager_ != nullptr) {
        const QString absolute =
            QDir(QCoreApplication::applicationDirPath()
                 + QStringLiteral("/plugins"))
                .filePath(relPath);
        auto loaded = LightCpp::Lua::LuaPluginHost::loadPlugin(
            absolute, this);
        if (loaded.error.isEmpty()) {
            pluginManager_->registerPlugin(std::move(loaded.plugin), this,
                                           QDir::fromNativeSeparators(absolute));
            outputPanel_->appendPluginOutput(
                tr("Plugin enabled (loaded): %1").arg(relPath));
        } else {
            outputPanel_->appendPluginOutput(
                tr("Plugin %1 failed to load: %2").arg(relPath, loaded.error));
        }
    } else {
        // A C++ plugin that is not running only loads at the next start —
        // the plugin panel shows the restart banner for it. One that is
        // still loaded (it was disabled a moment ago and the dll cannot
        // leave) keeps running, so no restart is needed for it.
        const QString absolute =
            QDir(QCoreApplication::applicationDirPath()
                 + QStringLiteral("/plugins"))
                .filePath(relPath);
        const QString normalized = QDir::fromNativeSeparators(absolute);
        bool currentlyLoaded = false;
        if (pluginManager_ != nullptr) {
            for (const auto& entry : pluginManager_->loaded()) {
                if (QDir::fromNativeSeparators(entry.path) == normalized) {
                    currentlyLoaded = true;
                    break;
                }
            }
        }
        cppReloadPending_ = !currentlyLoaded;
        outputPanel_->appendPluginOutput(
            tr("Plugin enabled — takes effect after a restart: %1")
                .arg(relPath));
    }
    refreshPluginPanel();
}

void MainWindow::onPluginUninstall(const QString& relPath)
{
    const LightCpp::Settings::Preferences prefs = pluginPrefsSnapshot();
    const QString absolute = QDir(QCoreApplication::applicationDirPath()
                                  + QStringLiteral("/plugins"))
                                 .filePath(relPath);
    const QString normalized = QDir::fromNativeSeparators(absolute);
    const bool wasLoaded =
        pluginManager_ != nullptr
        && pluginManager_->unloadRegistered(normalized);

    // Loaded C++ dlls are locked by the process and cannot be deleted (or
    // renamed) while the IDE runs — schedule the deletion for the next
    // start. Everything else (Lua scripts, never-loaded dlls) deletes now.
    QStringList pending = prefs.pendingPluginDeletes;
    if (!QFile::remove(absolute)) {
        if (!pending.contains(relPath, Qt::CaseInsensitive)) {
            pending.append(relPath);
        }
        persistPluginLists(prefs.disabledPlugins, pending);
        outputPanel_->appendPluginOutput(
            tr("Plugin file is locked — it will be deleted on the next "
               "start: %1")
                .arg(relPath));
    } else {
        persistPluginLists(prefs.disabledPlugins, pending);
        outputPanel_->appendPluginOutput(
            tr("Plugin uninstalled: %1").arg(relPath));
    }
    if (wasLoaded) {
        removePluginCommands(QFileInfo(normalized).completeBaseName());
        outputPanel_->appendPluginOutput(
            tr("Plugin unloaded: %1").arg(relPath));
    }
    refreshPluginPanel();
}

void MainWindow::loadPlugins()
{
    pluginManager_ = new LightCpp::Plugin::PluginManager(this);
    QString pluginDir =
        QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    if (!QFileInfo::exists(pluginDir)) {
        // Build-tree layout: the raw exe sits deep inside the build directory
        // (.../src/LightCppApp), while the plugin dlls land in
        // <build>/plugins — only the staged dist/ copy gets a plugins/
        // directory next to the executable. Walking up a bounded number of
        // levels, and only accepting a directory that actually holds plugin
        // dlls, keeps a build-tree binary working instead of silently
        // starting with no plugins. The check also keeps an unrelated
        // ancestor "plugins" folder (an IDE's, a browser's) from hijacking.
        QDir probe = QDir(QCoreApplication::applicationDirPath());
        for (int up = 0; up < 4; ++up) {
            if (!probe.cdUp()) {
                break;
            }
            const QString candidate = probe.filePath(QStringLiteral("plugins"));
            const QDir candidateDir(candidate);
            if (candidateDir.exists()
                && !candidateDir.entryList(
                       QStringList{QStringLiteral("*Plugin*.dll")},
                       QDir::Files).isEmpty()) {
                pluginDir = candidate;
                outputPanel_->appendPluginOutput(
                    tr("No plugins directory next to the executable — using "
                       "the build tree one: %1")
                        .arg(QDir::toNativeSeparators(pluginDir)));
                break;
            }
        }
    }
    if (!QFileInfo::exists(pluginDir)) {
        outputPanel_->appendPluginOutput(
            tr("Plugin directory does not exist: %1 — shared-library and "
               "Lua plugins are unavailable.")
                .arg(QDir::toNativeSeparators(pluginDir)));
    }
    outputPanel_->appendPluginOutput(
        tr("Plugin directory: %1")
            .arg(QDir::toNativeSeparators(pluginDir)));

    const LightCpp::Settings::Preferences prefs = pluginPrefsSnapshot();

    // Finish uninstalling what had to be deferred last run: the locked dll
    // of a plugin that was uninstalled while the IDE was still using it.
    // Now, before anything is loaded, the file is free to delete.
    //
    // The survivors go into a second list instead of erasing from the one
    // being iterated: a range-for holds a const iterator, and removeAll()
    // invalidates it — undefined behaviour on a path that is only ever taken
    // after a plugin was uninstalled while in use, which is why it never
    // showed up in testing.
    QStringList pending = prefs.pendingPluginDeletes;
    QStringList stillPending;
    for (const QString& rel : std::as_const(pending)) {
        const QString absolute = QDir(pluginDir).filePath(rel);
        if (QFile::remove(absolute)) {
            outputPanel_->appendPluginOutput(
                tr("Deleted on start-up as requested earlier: %1")
                    .arg(rel));
        } else {
            stillPending.append(rel);
            outputPanel_->appendPluginOutput(
                tr("Still cannot delete (will retry next start): %1")
                    .arg(rel));
        }
    }
    if (stillPending.size() != prefs.pendingPluginDeletes.size()) {
        persistPluginLists(prefs.disabledPlugins, stillPending);
    }

    // Split the disable list by kind: shared plugins match by bare file
    // name, Lua scripts by their file name inside the lua/ subdirectory.
    QStringList disabledCpp;
    QStringList disabledLua;
    for (const QString& rel : prefs.disabledPlugins) {
        if (rel.startsWith(QLatin1String("lua/"), Qt::CaseInsensitive)) {
            disabledLua.append(rel.mid(4));
        } else {
            disabledCpp.append(rel);
        }
    }

    for (const QString& error :
         pluginManager_->loadSharedPlugins(pluginDir, this, disabledCpp)) {
        outputPanel_->appendPluginOutput(tr("Plugin error: %1").arg(error));
    }
    // Lua scripts live one level deeper: plugins/lua/*.lua, each in its own
    // sandbox, registered with the same manager as the C++ plugins.
    for (const QString& error :
         LightCpp::Lua::LuaPluginHost::loadPluginsFromDirectory(
             pluginDir + QStringLiteral("/lua"), this, pluginManager_, {},
             disabledLua)) {
        outputPanel_->appendPluginOutput(tr("Plugin error: %1").arg(error));
    }
    for (const auto& loaded : pluginManager_->loaded()) {
        outputPanel_->appendPluginOutput(
            tr("Plugin loaded: %1 %2 (%3)")
                .arg(loaded.name, loaded.version,
                     QDir::toNativeSeparators(loaded.path)));
    }
    if (pluginManager_->loaded().isEmpty()) {
        outputPanel_->appendPluginOutput(tr("No plugins found."));
    }
    refreshPluginPanel();
}

QWidget* MainWindow::createGitPlaceholder()
{
    auto* panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("sideGitPanel"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    auto* note = new QLabel(tr("The Git panel is not built yet — the Git "
                               "menu and status bar keep working."), panel);
    note->setObjectName(QStringLiteral("sideGitNote"));
    note->setWordWrap(true);
    note->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    note->setStyleSheet(QStringLiteral("color: gray; margin-top: 24px;"));
    layout->addWidget(note);
    layout->addStretch(1);
    return panel;
}

QWidget* MainWindow::createPluginPanel()
{
    auto* panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("pluginPanel"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    // A disabled C++ plugin keeps running until the restart (its dll is
    // mapped into this process), so the panel offers the restart right
    // where the change was made. Hidden until refreshPluginPanel() finds
    // a plugin in that state.
    restartBanner_ = new QWidget(panel);
    restartBanner_->setObjectName(QStringLiteral("pluginRestartBanner"));
    auto* bannerLayout = new QHBoxLayout(restartBanner_);
    bannerLayout->setContentsMargins(10, 8, 10, 8);
    bannerLayout->setSpacing(6);
    auto* bannerLabel =
        new QLabel(tr("A plugin change takes effect after a restart."),
                   restartBanner_);
    bannerLabel->setWordWrap(true);
    restartButton_ = new QPushButton(tr("Restart IDE"), restartBanner_);
    restartButton_->setObjectName(QStringLiteral("pluginRestartButton"));
    restartButton_->setToolTip(
        tr("Close this window and start LightC++ again"));
    connect(restartButton_, &QPushButton::clicked, this,
            [this] { onRestartIde(); });
    bannerLayout->addWidget(bannerLabel, 1);
    bannerLayout->addWidget(restartButton_, 0);
    restartBanner_->hide();
    layout->addWidget(restartBanner_);
    pluginList_ = new QListWidget(panel);
    pluginList_->setObjectName(QStringLiteral("pluginList"));
    pluginList_->setSelectionMode(QAbstractItemView::NoSelection);
    pluginList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    pluginList_->setSpacing(2);
    layout->addWidget(pluginList_);
    return panel;
}

void MainWindow::refreshPluginPanel()
{
    if (pluginList_ == nullptr) {
        return;
    }
    pluginList_->clear();
    const QList<PluginCandidate> candidates = pluginCandidates();
    // Metadata (name / version / publisher) comes from the loaded set;
    // unloaded candidates show their file name instead.
    const auto loadedFor = [this](const PluginCandidate& c)
        -> std::optional<LightCpp::Plugin::PluginManager::Loaded> {
        if (pluginManager_ == nullptr) {
            return std::nullopt;
        }
        for (const auto& entry : pluginManager_->loaded()) {
            if (QDir::fromNativeSeparators(entry.path)
                == QDir::fromNativeSeparators(c.absolutePath)) {
                return entry;
            }
        }
        return std::nullopt;
    };
    // Set when a C++ plugin is disabled but still loaded, or was enabled
    // without being loadable until the next start: the banner then offers
    // the restart that applies the change.
    bool needsRestart = cppReloadPending_;
    for (const PluginCandidate& c : candidates) {
        auto* item = new QListWidgetItem(pluginList_);
        item->setData(Qt::UserRole, c.relPath);
        auto* row = new QWidget;
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(5);
        const std::optional<LightCpp::Plugin::PluginManager::Loaded> entry =
            loadedFor(c);
        if (!c.lua && c.disabled && entry.has_value()) {
            needsRestart = true;
        }
        auto* nameLabel =
            new QLabel(entry.has_value() ? entry->name : c.fileName, row);
        nameLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
        if (c.disabled) {
            QFont italic = nameLabel->font();
            italic.setItalic(true);
            nameLabel->setFont(italic);
        }
        layout->addWidget(nameLabel);

        // The tag chips: publisher (when declared), version, the kind the
        // plugin's code came from, and the state for unloaded entries.
        auto* tags = new QHBoxLayout;
        tags->setSpacing(4);
        const auto addChip = [&tags, row](const QString& text,
                                          const QColor& background) {
            auto* chip = new QLabel(text, row);
            chip->setStyleSheet(
                QStringLiteral("background: %1; color: white;"
                               "border-radius: 7px; padding: 1px 8px;"
                               "font-size: 11px;")
                    .arg(background.name()));
            tags->addWidget(chip);
        };
        if (entry.has_value() && !entry->publisher.isEmpty()) {
            addChip(entry->publisher, QColor(107, 114, 128));   // gray
        }
        if (entry.has_value()) {
            addChip(entry->version, QColor(37, 99, 235));       // blue
        }
        addChip(c.lua ? QStringLiteral("Lua") : QStringLiteral("C++"),
                c.lua ? QColor(22, 163, 74)    // green
                      : QColor(217, 119, 6));  // amber
        if (c.disabled) {
            addChip(tr("Disabled"), QColor(100, 100, 100));
        } else if (!entry.has_value()) {
            addChip(tr("Not loaded"), QColor(140, 140, 140));
        }
        tags->addStretch(1);
        layout->addLayout(tags);

        // Actions. Disable is only offered for running plugins; Enable only
        // for disabled ones; Uninstall always.
        auto* actions = new QHBoxLayout;
        actions->setSpacing(6);
        actions->addStretch(1);
        QPushButton* toggle = nullptr;
        if (c.disabled) {
            toggle = new QPushButton(tr("Enable"), row);
            connect(toggle, &QPushButton::clicked, this,
                    [this, relPath = c.relPath] { onPluginEnable(relPath); });
        } else if (entry.has_value()) {
            toggle = new QPushButton(tr("Disable"), row);
            connect(toggle, &QPushButton::clicked, this,
                    [this, relPath = c.relPath] { onPluginDisable(relPath); });
        }
        if (toggle != nullptr) {
            actions->addWidget(toggle);
        }
        auto* uninstall = new QPushButton(tr("Uninstall"), row);
        connect(uninstall, &QPushButton::clicked, this,
                [this, relPath = c.relPath] { onPluginUninstall(relPath); });
        actions->addWidget(uninstall);
        layout->addLayout(actions);

        pluginList_->setItemWidget(item, row);
        item->setSizeHint(row->sizeHint());
    }
    if (restartBanner_ != nullptr) {
        restartBanner_->setVisible(needsRestart);
    }
}

OutputPanel::Channel MainWindow::serverLogChannel() const
{
    if (completionBackend_ == QLatin1String("ccls")) {
        return OutputPanel::ChannelCcls;
    }
    if (completionBackend_ == QLatin1String("libclang")) {
        return OutputPanel::ChannelLibClang;
    }
    return OutputPanel::ChannelServer;
}

OutputPanel::Channel MainWindow::serverProcessLogChannel() const
{
    // A message coming FROM the server process must be filed under the server
    // that produced it, not under whatever the completion backend currently
    // says. Routing by the setting sent a dead ccls.exe's parting line to the
    // Clangd Output pane the moment the user switched backends, which made an
    // unrelated crash look like the wrong server's failure.
    const QString name = QFileInfo(lsp_->serverPath()).fileName().toLower();
    if (name.contains(QLatin1String("ccls"))) {
        return OutputPanel::ChannelCcls;
    }
    return OutputPanel::ChannelServer;
}

void MainWindow::appendLanguageServerLog(const QString& text)
{
    switch (serverLogChannel()) {
    case OutputPanel::ChannelCcls:
        outputPanel_->appendCclsOutput(text);
        break;
    case OutputPanel::ChannelLibClang:
        outputPanel_->appendLibClangOutput(text);
        break;
    default:
        outputPanel_->appendServerLog(text);
        break;
    }
}

void MainWindow::appendServerProcessLog(const QString& text)
{
    switch (serverProcessLogChannel()) {
    case OutputPanel::ChannelCcls:
        outputPanel_->appendCclsOutput(text);
        break;
    default:
        outputPanel_->appendServerLog(text);
        break;
    }
}

void MainWindow::appendLibClangLog(const QString& text)
{
    outputPanel_->appendLibClangOutput(text);
}

QAction* MainWindow::findNamedAction(const QString& id) const
{
    for (QAction* action : findChildren<QAction*>()) {
        if (action->objectName() == id) {
            return action;
        }
    }
    return nullptr;
}

void MainWindow::log(const QString& message)
{
    outputPanel_->appendPluginOutput(message);
}

QString MainWindow::activeEditorPath() const
{
    const CodeEditor* editor = currentEditor();
    return editor == nullptr ? QString() : editor->filePath();
}

QString MainWindow::editorText() const
{
    const CodeEditor* editor = currentEditor();
    return editor == nullptr ? QString() : editor->toPlainText();
}

void MainWindow::setEditorText(const QString& text)
{
    if (CodeEditor* editor = currentEditor()) {
        editor->setPlainText(text);
    }
}

void MainWindow::insertAtCursor(const QString& text)
{
    if (CodeEditor* editor = currentEditor()) {
        editor->textCursor().insertText(text);
    }
}

void MainWindow::gotoLine(int line)
{
    if (CodeEditor* editor = currentEditor()) {
        const QTextBlock block = editor->document()->findBlockByNumber(line);
        if (!block.isValid()) {
            return;
        }
        QTextCursor cursor = editor->textCursor();
        cursor.setPosition(block.position());
        editor->setTextCursor(cursor);
        editor->centerCursor();
    }
}

QStringList MainWindow::actionIds() const
{
    QStringList ids;
    for (const QAction* action : findChildren<QAction*>()) {
        const QString name = action->objectName();
        if (!name.isEmpty()) {
            ids.append(name);
        }
    }
    return ids;
}

QString MainWindow::actionText(const QString& id) const
{
    const QAction* action = findNamedAction(id);
    return action == nullptr ? QString() : action->text();
}

void MainWindow::setActionText(const QString& id, const QString& text)
{
    if (QAction* action = findNamedAction(id)) {
        action->setText(text);
        // Toolbar buttons in TextBesideIcon mode show the icon text; keep
        // both in step so menus and the toolbar change together.
        action->setIconText(text);
    } else {
        log(tr("Plugin: no action named %1").arg(id));
    }
}

QStringList MainWindow::menuIds() const
{
    QStringList ids;
    for (const QMenu* menu : findChildren<QMenu*>()) {
        const QString name = menu->objectName();
        if (!name.isEmpty()) {
            ids.append(name);
        }
    }
    return ids;
}

QString MainWindow::menuTitle(const QString& id) const
{
    for (const QMenu* menu : findChildren<QMenu*>()) {
        if (menu->objectName() == id) {
            return menu->title();
        }
    }
    return {};
}

void MainWindow::setMenuTitle(const QString& id, const QString& title)
{
    for (QMenu* menu : findChildren<QMenu*>()) {
        if (menu->objectName() == id) {
            menu->setTitle(title);
            return;
        }
    }
    log(tr("Plugin: no menu named %1").arg(id));
}

void MainWindow::registerCommand(const QString& pluginName, const QString& id,
                                 const QString& label, const QString& shortcut,
                                 std::function<void()> callback)
{
    if (pluginsMenu_ == nullptr) {
        pluginsMenu_ = new QMenu(tr("&Plugins"), this);
        pluginsMenu_->setObjectName(QStringLiteral("menuPlugins"));
        // Insert before Help, so plugin commands sit with the other tools.
        const QList<QAction*> menuActions = menuBar()->actions();
        if (!menuActions.isEmpty()) {
            menuBar()->insertMenu(menuActions.last(), pluginsMenu_);
        } else {
            menuBar()->addMenu(pluginsMenu_);
        }
    }
    auto* action = new QAction(label, pluginsMenu_);
    action->setObjectName(QStringLiteral("pluginCommand.%1.%2")
                              .arg(pluginName, id));
    if (!shortcut.isEmpty()) {
        action->setShortcut(QKeySequence(shortcut));
        action->setToolTip(QStringLiteral("%1 (%2)").arg(label, shortcut));
    }
    connect(action, &QAction::triggered, this,
            [callback = std::move(callback)] { callback(); });
    pluginsMenu_->addAction(action);
}

void MainWindow::removePluginCommands(const QString& pluginName)
{
    if (pluginsMenu_ == nullptr || pluginName.isEmpty()) {
        return;
    }
    const QString prefix = QStringLiteral("pluginCommand.%1.").arg(pluginName);
    const QList<QAction*> actions = pluginsMenu_->actions();
    for (QAction* action : actions) {
        if (action->objectName().startsWith(prefix, Qt::CaseInsensitive)) {
            pluginsMenu_->removeAction(action);
            action->deleteLater();
        }
    }
    if (pluginsMenu_->actions().isEmpty()) {
        // No plugin registers a command any more: an empty submenu is worse
        // than none at all, so the Plugins menu itself goes away too.
        if (QAction* menuAction = pluginsMenu_->menuAction()) {
            menuBar()->removeAction(menuAction);
        }
        pluginsMenu_->deleteLater();
        pluginsMenu_ = nullptr;
    }
}

void MainWindow::onRestartIde()
{
    // Deferred to an accepted close so the unsaved-file prompts still run
    // first: a cancelled close keeps this instance and drops the restart
    // with it (closeEvent clears the flag).
    restartPending_ = true;
    close();
}

QList<MainWindow::ProjectStep> MainWindow::toProjectSteps(
    const QList<LightCpp::Plugin::BuildStep>& steps) const
{
    QList<ProjectStep> converted;
    converted.reserve(steps.size());
    for (const LightCpp::Plugin::BuildStep& step : steps) {
        converted.append(ProjectStep{step.program, step.arguments,
                                     step.workingDirectory, step.label});
    }
    return converted;
}

LightCpp::Plugin::BuildRequest MainWindow::makeBuildRequest() const
{
    LightCpp::Plugin::BuildRequest request;
    request.configuration = currentBuildConfigurationName();

    // Defaults straight from the MSYS2 toolchain, then the kit's overrides —
    // the same resolution the built-in paths used before .dev support moved
    // into a plugin.
    QString make = QDir(kMingwBin).filePath(QStringLiteral("mingw32-make.exe"));
    QString gxx = QDir(kMingwBin).filePath(QStringLiteral("g++.exe"));
    if (const auto kit = activeKit(); kit.has_value()) {
        const QString kitMake = kit->buildTools.value(QStringLiteral("gnuMake"));
        if (!kitMake.isEmpty() && QFileInfo::exists(kitMake)) {
            make = kitMake;
        }
        if (!kit->cppCompiler.isEmpty() && !isMsvcKit(*kit)) {
            gxx = kit->cppCompiler;
        }
        request.extraMakeArgs =
            settings_->effectiveBuildConfig(*kit, currentBuildConfigurationName())
                .buildToolArgs.value(QStringLiteral("gnuMake"));
    }
    request.make = make;
    request.compiler = gxx;
    return request;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Stop a running single-file compile so its child process doesn't
    // outlive the window (QProcess::~QProcess does not kill).
    if (compiler_ && compiler_->state() != QProcess::NotRunning) {
        compiler_->kill();
        compiler_->waitForFinished(2000);
    }
    // Same for a program started with Compile & Run / Run.
    if (runner_ && runner_->state() != QProcess::NotRunning) {
        runner_->kill();
        runner_->waitForFinished(2000);
    }
    // Prompt to save modified files.
    for (int i = 0; i < tabWidget_->count(); ++i) {
        if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
            if (ed->isModified()) {
                tabWidget_->setCurrentIndex(i);
                const auto reply = QMessageBox::warning(
                    this, tr("Unsaved Changes"),
                    tr("File \"%1\" has unsaved changes. Save?")
                        .arg(tabWidget_->tabText(i)),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
                if (reply == QMessageBox::Yes) {
                    onSaveFile();
                } else if (reply == QMessageBox::Cancel) {
                    // No restart either: the user asked to stay here.
                    restartPending_ = false;
                    event->ignore();
                    return;
                }
            }
        }
    }
    // Record the session for the next launch — after the save prompts, so
    // buffers the user chose to discard are not recorded as open tabs with
    // paths that then reopen dirty-free but stale.
    saveSessionState();
    event->accept();
    if (restartPending_) {
        restartPending_ = false;
        // Start the replacement before this process goes away: same command
        // line (minus argv[0]) and working directory. The new instance
        // re-reads the plugin lists, so a disabled C++ plugin is gone.
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                QCoreApplication::arguments().mid(1),
                                QDir::currentPath());
    }
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------
void MainWindow::createActions()
{
    actNew_ = new QAction(QIcon::fromTheme(QStringLiteral("document-new")), tr("&New"), this);
    actNew_->setShortcut(QKeySequence::New);
    connect(actNew_, &QAction::triggered, this, &MainWindow::onNewFile);

    actOpenFolder_ = new QAction(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Open &Folder..."), this);
    actOpenFolder_->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    actOpenFolder_->setStatusTip(
        tr("Show a folder in the Explorer; open it as a project if it holds one"));
    connect(actOpenFolder_, &QAction::triggered, this, &MainWindow::onOpenFolder);

    actOpen_ = new QAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("&Open..."), this);
    actOpen_->setShortcut(QKeySequence::Open);
    connect(actOpen_, &QAction::triggered, this, &MainWindow::onOpenFile);

    actSave_ = new QAction(QIcon::fromTheme(QStringLiteral("document-save")), tr("&Save"), this);
    actSave_->setShortcut(QKeySequence::Save);
    connect(actSave_, &QAction::triggered, this, &MainWindow::onSaveFile);

    actSaveAll_ = new QAction(QIcon::fromTheme(QStringLiteral("document-save-all")), tr("Save A&ll"), this);
    actSaveAll_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(actSaveAll_, &QAction::triggered, this, &MainWindow::onSaveAll);

    actExit_ = new QAction(tr("E&xit"), this);
    actExit_->setShortcut(QKeySequence::Quit);
    connect(actExit_, &QAction::triggered, this, &QWidget::close);

    // Panel visibility. The panels are part of the window layout, so these
    // simply show/hide them — no docking, no floating windows.
    actToggleExplorer_ = new QAction(tr("&Explorer Panel"), this);
    actToggleExplorer_->setCheckable(true);
    actToggleExplorer_->setChecked(true);
    actToggleExplorer_->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    // The Explorer now lives inside the vertical side tabs, so the toggle
    // shows and hides the whole activity bar.
    connect(actToggleExplorer_, &QAction::toggled,
            sideTabs_, &QWidget::setVisible);

    actToggleOutput_ = new QAction(tr("&Output Panel"), this);
    actToggleOutput_->setCheckable(true);
    actToggleOutput_->setChecked(true);
    actToggleOutput_->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
    connect(actToggleOutput_, &QAction::toggled,
            outputPanel_, &QWidget::setVisible);

    actSyntaxCheck_ = new QAction(tr("&Syntax Check"), this);
    actSyntaxCheck_->setCheckable(true);
    actSyntaxCheck_->setStatusTip(tr("Run clangd in the background and underline "
                                      "errors and warnings as you type"));
    connect(actSyntaxCheck_, &QAction::toggled,
            this, &MainWindow::onSyntaxCheckToggled);

    // --- single-file compile -------------------------------------------
    actCompile_ = new QAction(tr("Compil&e Current File"), this);
    actCompile_->setShortcut(QKeySequence(Qt::Key_F7));
    actCompile_->setStatusTip(tr("Compile the active file with the selected "
                                  "toolchain (gcc / g++ / clang / clang++ / cl.exe)"));
    connect(actCompile_, &QAction::triggered,
            this, &MainWindow::onCompileCurrentFile);

    // --- compile & run / run -------------------------------------------
    actCompileRun_ = new QAction(QIcon::fromTheme(QStringLiteral("run-build")),
                                 tr("Compile && &Run"), this);
    actCompileRun_->setShortcut(QKeySequence(Qt::Key_F5));
    actCompileRun_->setStatusTip(tr("Compile the active file, then run the "
                                     "program it produces"));
    connect(actCompileRun_, &QAction::triggered,
            this, &MainWindow::onCompileAndRun);

    actRun_ = new QAction(QIcon::fromTheme(QStringLiteral("media-playback-start")),
                          tr("&Run"), this);
    actRun_->setShortcut(QKeySequence(QStringLiteral("Ctrl+F5")));
    actRun_->setStatusTip(tr("Run the program from the last build "
                              "(without rebuilding)"));
    connect(actRun_, &QAction::triggered,
            this, &MainWindow::onRunCurrentFile);

    actStopRun_ = new QAction(QIcon::fromTheme(QStringLiteral("process-stop")),
                              tr("&Stop Program"), this);
    actStopRun_->setShortcut(QKeySequence(QStringLiteral("Shift+F5")));
    actStopRun_->setStatusTip(tr("Kill the program started by Run"));
    actStopRun_->setEnabled(false);
    connect(actStopRun_, &QAction::triggered,
            this, &MainWindow::onStopProgram);

    // --- debugging --------------------------------------------------------
    actToggleBreakpoint_ = new QAction(tr("Toggle &Breakpoint"), this);
    actToggleBreakpoint_->setShortcut(QKeySequence(Qt::Key_F9));
    actToggleBreakpoint_->setStatusTip(
        tr("Set or clear a breakpoint on the current line (F9)"));
    connect(actToggleBreakpoint_, &QAction::triggered,
            this, &MainWindow::onToggleBreakpoint);

    actDebugStart_ = new QAction(tr("Start &Debugging"), this);
    actDebugStart_->setShortcut(QKeySequence(Qt::Key_F4));
    actDebugStart_->setStatusTip(
        tr("Run the last build under the kit's debugger (GDB, LLDB or cdb)"));
    connect(actDebugStart_, &QAction::triggered,
            this, &MainWindow::onStartDebugging);

    actDebugStop_ = new QAction(tr("Stop &Debugging"), this);
    actDebugStop_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F5")));
    actDebugStop_->setStatusTip(tr("End the debug session"));
    actDebugStop_->setEnabled(false);
    connect(actDebugStop_, &QAction::triggered,
            this, &MainWindow::onStopDebugging);

    actDebugContinue_ = new QAction(tr("&Continue"), this);
    actDebugContinue_->setStatusTip(
        tr("Resume execution (while debugging, F5 continues as well)"));
    actDebugContinue_->setEnabled(false);
    connect(actDebugContinue_, &QAction::triggered, this, [this] {
        if (debugger_ != nullptr) {
            debugger_->cont();
        }
    });

    actDebugStepOver_ = new QAction(tr("Step &Over"), this);
    actDebugStepOver_->setShortcut(QKeySequence(QStringLiteral("F10")));
    actDebugStepOver_->setEnabled(false);
    connect(actDebugStepOver_, &QAction::triggered, this, [this] {
        if (debugger_ != nullptr) {
            debugger_->stepOver();
        }
    });

    actDebugStepInto_ = new QAction(tr("Step &Into"), this);
    actDebugStepInto_->setShortcut(QKeySequence(QStringLiteral("F11")));
    actDebugStepInto_->setEnabled(false);
    connect(actDebugStepInto_, &QAction::triggered, this, [this] {
        if (debugger_ != nullptr) {
            debugger_->stepInto();
        }
    });

    actDebugStepOut_ = new QAction(tr("Step O&ut"), this);
    actDebugStepOut_->setShortcut(QKeySequence(QStringLiteral("Shift+F11")));
    actDebugStepOut_->setEnabled(false);
    connect(actDebugStepOut_, &QAction::triggered, this, [this] {
        if (debugger_ != nullptr) {
            debugger_->stepOut();
        }
    });

    // The build environment (compilers, debugger, clangd, ...) comes from the
    // settings' kits in config/*.json — there is no hard-coded GCC/Clang/MSVC
    // menu any more. These three actions manage the configuration files instead.
    actSettings_ = new QAction(tr("&Settings..."), this);
    actSettings_->setShortcut(QKeySequence::Preferences);
    actSettings_->setStatusTip(
        tr("Edit toolchains, kits, defaults and preferences (config/*.json)"));
    connect(actSettings_, &QAction::triggered,
            this, &MainWindow::onSettings);

    actReloadSettings_ = new QAction(tr("&Reload Settings"), this);
    actReloadSettings_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+R")));
    actReloadSettings_->setStatusTip(
        tr("Re-read every config/*.json and re-apply kits, fonts and limits"));
    connect(actReloadSettings_, &QAction::triggered,
            this, &MainWindow::onReloadSettings);

    actOpenConfigDir_ = new QAction(tr("Open &Config Folder"), this);
    actOpenConfigDir_->setStatusTip(
        tr("Open the base configuration directory next to the executable"));
    connect(actOpenConfigDir_, &QAction::triggered, this, [this] {
        const QString dir = settings_->baseConfigDirectory();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    actAbout_ = new QAction(tr("&About LightC++..."), this);
    connect(actAbout_, &QAction::triggered, this, &MainWindow::onAbout);

    // --- code folding ---------------------------------------------------
    // The gutter carries a +/- box per foldable line, which is the way most
    // people fold; these exist for the keyboard and for the menu.
    actFold_ = new QAction(tr("&Fold"), this);
    actFold_->setShortcuts({QKeySequence(QStringLiteral("Alt+Shift+Left")),
                            QKeySequence(QStringLiteral("Ctrl+Shift+["))});
    actFold_->setStatusTip(tr("Collapse the innermost block on the current line"));
    connect(actFold_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->foldAtCaret();
        }
    });

    actUnfold_ = new QAction(tr("&Unfold"), this);
    actUnfold_->setShortcuts({QKeySequence(QStringLiteral("Alt+Shift+Right")),
                              QKeySequence(QStringLiteral("Ctrl+Shift+]"))});
    actUnfold_->setStatusTip(tr("Expand the collapsed block around the cursor"));
    connect(actUnfold_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->unfoldAtCaret();
        }
    });

    actFoldAll_ = new QAction(tr("Fold &All"), this);
    actFoldAll_->setStatusTip(tr("Collapse every foldable block in the file"));
    connect(actFoldAll_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->foldAll();
        }
    });

    actUnfoldAll_ = new QAction(tr("Unfold A&ll"), this);
    actUnfoldAll_->setStatusTip(tr("Expand every collapsed block in the file"));
    connect(actUnfoldAll_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->unfoldAll();
        }
    });

    // --- find / replace ---------------------------------------------------
    actFind_ = new QAction(tr("&Find..."), this);
    actFind_->setShortcut(QKeySequence::Find);
    actFind_->setStatusTip(tr("Search in the current file (regular expressions supported)"));
    connect(actFind_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            findBar_->openFor(ed, false);
        }
    });

    actFindNext_ = new QAction(tr("Find &Next"), this);
    actFindNext_->setShortcut(QKeySequence::FindNext);  // F3
    connect(actFindNext_, &QAction::triggered, this, [this] {
        findBar_->findNext();
    });

    actFindPrev_ = new QAction(tr("Find &Previous"), this);
    actFindPrev_->setShortcut(QKeySequence::FindPrevious);  // Shift+F3
    connect(actFindPrev_, &QAction::triggered, this, [this] {
        findBar_->findPrev();
    });

    actReplace_ = new QAction(tr("&Replace..."), this);
    actReplace_->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    actReplace_->setStatusTip(tr("Search and replace in the current file"));
    connect(actReplace_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            findBar_->openFor(ed, true);
        }
    });

    // --- clipboard / undo / redo -----------------------------------------
    // Menu entries for what the editor's context menu already does. The
    // shortcuts use the WidgetWithChildren context scoped to the tab widget,
    // so they only fire while an editor tab owns the keyboard — a QLineEdit
    // elsewhere in the window (the find bar, the terminal input rows) keeps
    // its own native Ctrl+C/V/X/Z handling instead of being starved by the
    // menu action.
    actUndo_ = new QAction(tr("&Undo"), this);
    actUndo_->setShortcut(QKeySequence::Undo);
    actUndo_->setStatusTip(tr("Undo the last editing change"));
    connect(actUndo_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->undo();
        }
    });

    actRedo_ = new QAction(tr("&Redo"), this);
    actRedo_->setShortcuts({QKeySequence::Redo,
                            QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
    actRedo_->setStatusTip(tr("Redo the last undone change"));
    connect(actRedo_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->redo();
        }
    });

    actCut_ = new QAction(tr("Cu&t"), this);
    actCut_->setShortcut(QKeySequence::Cut);
    actCut_->setStatusTip(tr("Cut the selection to the clipboard"));
    connect(actCut_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->cut();
        }
    });

    actCopy_ = new QAction(tr("&Copy"), this);
    actCopy_->setShortcut(QKeySequence::Copy);
    actCopy_->setStatusTip(tr("Copy the selection to the clipboard"));
    connect(actCopy_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->copy();
        }
    });

    actPaste_ = new QAction(tr("&Paste"), this);
    actPaste_->setShortcut(QKeySequence::Paste);
    actPaste_->setStatusTip(tr("Paste the clipboard at the cursor"));
    connect(actPaste_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->paste();
        }
    });

    for (QAction* clipboardAction :
         {actUndo_, actRedo_, actCut_, actCopy_, actPaste_}) {
        clipboardAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        tabWidget_->addAction(clipboardAction);
    }

    // --- search in project ------------------------------------------------
    actSearchInProject_ = new QAction(tr("Search in &Project..."), this);
    actSearchInProject_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    actSearchInProject_->setStatusTip(
        tr("Search every text file under the Explorer root (background "
           "thread; double-click a hit to open it at that line)"));
    connect(actSearchInProject_, &QAction::triggered,
            this, &MainWindow::onSearchInProject);

    // --- switch header / source -------------------------------------------
    actSwitchHeaderSource_ = new QAction(tr("Switch &Header/Source"), this);
    actSwitchHeaderSource_->setShortcut(QKeySequence(QStringLiteral("Alt+O")));
    actSwitchHeaderSource_->setStatusTip(
        tr("Open the header or source that pairs with the active file "
           "(same basename, .cpp/.cc/.cxx/.c <-> .h/.hh/.hpp/.hxx, next to "
           "the file first)"));
    connect(actSwitchHeaderSource_, &QAction::triggered,
            this, &MainWindow::onSwitchHeaderSource);

    // --- navigation: declaration / implementation / references ------------
    actGotoDeclaration_ = new QAction(tr("Go to &Declaration"), this);
    actGotoDeclaration_->setShortcut(QKeySequence(QStringLiteral("Ctrl+F12")));
    actGotoDeclaration_->setStatusTip(
        tr("Jump to the declaration of the symbol at the cursor (clangd)"));
    connect(actGotoDeclaration_, &QAction::triggered, this,
            [this] { requestNavigation(0); });

    actGotoImplementation_ = new QAction(tr("Go to &Implementation"), this);
    actGotoImplementation_->setShortcut(
        QKeySequence(QStringLiteral("Ctrl+F11")));
    actGotoImplementation_->setStatusTip(
        tr("Jump to the implementation of the symbol at the cursor (clangd)"));
    connect(actGotoImplementation_, &QAction::triggered, this,
            [this] { requestNavigation(1); });

    actFindReferences_ = new QAction(tr("Find &References"), this);
    actFindReferences_->setShortcut(QKeySequence(QStringLiteral("Shift+F12")));
    actFindReferences_->setStatusTip(
        tr("List every usage of the symbol at the cursor (clangd)"));
    connect(actFindReferences_, &QAction::triggered, this,
            [this] { requestNavigation(2); });

    actGoToSymbol_ = new QAction(tr("Go to &Symbol in File"), this);
    actGoToSymbol_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    actGoToSymbol_->setStatusTip(
        tr("Filter the symbols of the current document (clangd)"));
    connect(actGoToSymbol_, &QAction::triggered, this,
            [this] { showSymbolSearch(); });

    actRenameSymbol_ = new QAction(tr("Rename S&ymbol..."), this);
    actRenameSymbol_->setShortcut(
        QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    actRenameSymbol_->setStatusTip(
        tr("Rename the symbol at the cursor across the project (clangd)"));
    connect(actRenameSymbol_, &QAction::triggered, this,
            [this] { renameSymbolAtCursor(); });

    actQuickFix_ = new QAction(tr("&Quick Fix..."), this);
    actQuickFix_->setShortcut(QKeySequence(QStringLiteral("Ctrl+.")));
    actQuickFix_->setStatusTip(
        tr("Show code actions for the diagnostics on the caret line (clangd)"));
    connect(actQuickFix_, &QAction::triggered, this,
            [this] { requestQuickFix(); });

    // --- bookmarks ----------------------------------------------------------
    actToggleBookmark_ = new QAction(tr("&Toggle Bookmark"), this);
    actToggleBookmark_->setShortcut(QKeySequence(QStringLiteral("Ctrl+F2")));
    actToggleBookmark_->setStatusTip(
        tr("Mark or unmark the current line (blue square in the gutter)"));
    connect(actToggleBookmark_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->toggleBookmark();
        }
    });

    actNextBookmark_ = new QAction(tr("&Next Bookmark"), this);
    actNextBookmark_->setShortcut(QKeySequence(Qt::Key_F2));
    actNextBookmark_->setStatusTip(tr("Jump to the next bookmark (wraps)"));
    connect(actNextBookmark_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->nextBookmark();
        }
    });

    actPrevBookmark_ = new QAction(tr("&Previous Bookmark"), this);
    actPrevBookmark_->setShortcut(QKeySequence(QStringLiteral("Shift+F2")));
    actPrevBookmark_->setStatusTip(
        tr("Jump to the previous bookmark (wraps)"));
    connect(actPrevBookmark_, &QAction::triggered, this, [this] {
        if (CodeEditor* ed = currentEditor()) {
            ed->prevBookmark();
        }
    });

    // Greyed out until an editor tab becomes current; the tab switch below
    // and the clipboard changes re-evaluate it.
    updateEditActions();
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
            this, &MainWindow::updateEditActions);

    // --- code formatting ------------------------------------------------
    actFormat_ = new QAction(tr("&Format Document"), this);
    actFormat_->setShortcuts({QKeySequence(QStringLiteral("Shift+Alt+F")),
                              QKeySequence(QStringLiteral("Ctrl+Shift+I"))});
    actFormat_->setStatusTip(
        tr("Reindent and rebrace the whole file with clang-format.\n"
           "A .clang-format in the project wins; %1 is the fallback style.")
            .arg(QString::fromLatin1(LightCpp::App::CodeFormatter::kFallbackStyle)));
    connect(actFormat_, &QAction::triggered, this, &MainWindow::onFormatDocument);

    // --- batch formatting -----------------------------------------------
    actFormatFolder_ = new QAction(tr("Format Fol&der..."), this);
    actFormatFolder_->setStatusTip(
        tr("Reindent every C/C++ file under a folder with clang-format. "
           "A .clang-format in the tree wins; %1 is the fallback style. "
           "At/above %2 files you are asked to confirm first.")
            .arg(QString::fromLatin1(LightCpp::App::CodeFormatter::kFallbackStyle))
            .arg(LightCpp::App::BatchFormatter::kWarnThreshold));
    connect(actFormatFolder_, &QAction::triggered,
            this, &MainWindow::onFormatFolder);

    actFormatProject_ = new QAction(tr("Format Pro&ject Files"), this);
    actFormatProject_->setStatusTip(
        tr("Reindent every C/C++ file in the open project with clang-format."));
    connect(actFormatProject_, &QAction::triggered,
            this, &MainWindow::onFormatProject);

    // --- project actions -------------------------------------------------
    actOpenProject_ = new QAction(tr("Open Pro&ject..."), this);
    // Ctrl+Alt+O, not Ctrl+Shift+O: that one belongs to Go to Symbol in File
    // (the VSCode convention), and two actions sharing a shortcut is an
    // ambiguous-trigger lottery.
    actOpenProject_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+O")));
    actOpenProject_->setStatusTip(
        tr("Open a CMakeLists.txt or a Dev-C++ .dev project"));
    connect(actOpenProject_, &QAction::triggered, this, &MainWindow::onOpenProject);

    actConfigureProject_ = new QAction(tr("&Configure Project"), this);
    actConfigureProject_->setStatusTip(
        tr("Create the build tree for the selected configuration with CMake"));
    connect(actConfigureProject_, &QAction::triggered, this,
            &MainWindow::onConfigureProject);

    actBuildProject_ = new QAction(QIcon::fromTheme(QStringLiteral("run-build")),
                                   tr("&Build Project"), this);
    actBuildProject_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    actBuildProject_->setStatusTip(
        tr("Configure if needed, then build the whole project"));
    connect(actBuildProject_, &QAction::triggered, this, &MainWindow::onBuildProject);

    actRunProject_ = new QAction(QIcon::fromTheme(QStringLiteral("media-playback-start")),
                                 tr("Run Pro&ject"), this);
    // F6, not Ctrl+Shift+F5: that one is already Stop Debugging, and two
    // actions on the same key means neither works reliably — the menu shows
    // the same shortcut twice and which one fires depends on enable state.
    actRunProject_->setShortcut(QKeySequence(Qt::Key_F6));
    actRunProject_->setStatusTip(
        tr("Run the executable the last project build produced"));
    connect(actRunProject_, &QAction::triggered, this, &MainWindow::onRunProject);

    actCloseProject_ = new QAction(tr("&Close Project"), this);
    actCloseProject_->setStatusTip(tr("Forget the open project and its build tree"));
    connect(actCloseProject_, &QAction::triggered, this, &MainWindow::onCloseProject);

    // --- git --------------------------------------------------------------
    actGitRefresh_ = new QAction(tr("&Refresh Status"), this);
    actGitRefresh_->setStatusTip(tr("Re-read the working tree and show every change"));
    connect(actGitRefresh_, &QAction::triggered, this, &MainWindow::onGitRefresh);

    actGitStageAll_ = new QAction(tr("Stage &All Changes"), this);
    actGitStageAll_->setStatusTip(tr("git add -A — stage every change, including deletions"));
    connect(actGitStageAll_, &QAction::triggered, this, &MainWindow::onGitStageAll);

    actGitUnstageAll_ = new QAction(tr("&Unstage All Changes"), this);
    actGitUnstageAll_->setStatusTip(tr("git reset — take everything back out of the index"));
    connect(actGitUnstageAll_, &QAction::triggered, this, &MainWindow::onGitUnstageAll);

    actGitCommit_ = new QAction(tr("&Commit..."), this);
    actGitCommit_->setStatusTip(tr("Commit what is staged, with a message you type"));
    connect(actGitCommit_, &QAction::triggered, this, &MainWindow::onGitCommit);

    actGitLog_ = new QAction(tr("Show &Log"), this);
    actGitLog_->setStatusTip(tr("git log --oneline for the current repository"));
    connect(actGitLog_, &QAction::triggered, this, &MainWindow::onGitLog);

    actGitDiff_ = new QAction(tr("Diff &Working Tree"), this);
    actGitDiff_->setStatusTip(tr("Show the unstaged changes of the active file"));
    connect(actGitDiff_, &QAction::triggered, this, [this] { showGitDiff(false); });

    actGitDiffStaged_ = new QAction(tr("Diff &Staged"), this);
    actGitDiffStaged_->setStatusTip(tr("Show the staged changes of the active file"));
    connect(actGitDiffStaged_, &QAction::triggered, this, [this] { showGitDiff(true); });

    actGitInit_ = new QAction(tr("&Initialise Repository"), this);
    actGitInit_->setStatusTip(tr("git init in the directory git would run in"));
    connect(actGitInit_, &QAction::triggered, this, &MainWindow::onGitInit);

    actGitRefresh_->setEnabled(false);
    actGitStageAll_->setEnabled(false);
    actGitUnstageAll_->setEnabled(false);
    actGitCommit_->setEnabled(false);
    actGitLog_->setEnabled(false);
    actGitDiff_->setEnabled(false);
    actGitDiffStaged_->setEnabled(false);

    // Stable object names: this is the address space plugins use through
    // IdeServices::setActionText / actionText. The names are the ids the
    // plugin API documents (e.g. "actionNew", "actionBuildProject").
    const QHash<QAction*, QString> actionNames = {
        {actNew_, "actionNew"},
        {actOpen_, "actionOpen"},
        {actOpenFolder_, "actionOpenFolder"},
        {actSave_, "actionSave"},
        {actSaveAll_, "actionSaveAll"},
        {actExit_, "actionExit"},
        {actToggleExplorer_, "actionToggleExplorer"},
        {actToggleOutput_, "actionToggleOutput"},
        {actSyntaxCheck_, "actionSyntaxCheck"},
        {actCompile_, "actionCompile"},
        {actCompileRun_, "actionCompileRun"},
        {actRun_, "actionRun"},
        {actStopRun_, "actionStopRun"},
        {actToggleBreakpoint_, "actionToggleBreakpoint"},
        {actDebugStart_, "actionDebugStart"},
        {actDebugStop_, "actionDebugStop"},
        {actDebugContinue_, "actionDebugContinue"},
        {actDebugStepOver_, "actionDebugStepOver"},
        {actDebugStepInto_, "actionDebugStepInto"},
        {actDebugStepOut_, "actionDebugStepOut"},
        {actSettings_, "actionSettings"},
        {actReloadSettings_, "actionReloadSettings"},
        {actOpenConfigDir_, "actionOpenConfigDir"},
        {actAbout_, "actionAbout"},
        {actFold_, "actionFold"},
        {actUnfold_, "actionUnfold"},
        {actFoldAll_, "actionFoldAll"},
        {actUnfoldAll_, "actionUnfoldAll"},
        {actFind_, "actionFind"},
        {actFindNext_, "actionFindNext"},
        {actFindPrev_, "actionFindPrev"},
        {actReplace_, "actionReplace"},
        {actUndo_, "actionUndo"},
        {actRedo_, "actionRedo"},
        {actCut_, "actionCut"},
        {actCopy_, "actionCopy"},
        {actPaste_, "actionPaste"},
        {actSearchInProject_, "actionSearchInProject"},
        {actSwitchHeaderSource_, "actionSwitchHeaderSource"},
        {actGotoDeclaration_, "actionGotoDeclaration"},
        {actGotoImplementation_, "actionGotoImplementation"},
        {actFindReferences_, "actionFindReferences"},
        {actGoToSymbol_, "actionGoToSymbol"},
        {actRenameSymbol_, "actionRenameSymbol"},
        {actQuickFix_, "actionQuickFix"},
        {actToggleBookmark_, "actionToggleBookmark"},
        {actNextBookmark_, "actionNextBookmark"},
        {actPrevBookmark_, "actionPrevBookmark"},
        {actFormat_, "actionFormat"},
        {actFormatFolder_, "actionFormatFolder"},
        {actFormatProject_, "actionFormatProject"},
        {actOpenProject_, "actionOpenProject"},
        {actConfigureProject_, "actionConfigureProject"},
        {actBuildProject_, "actionBuildProject"},
        {actRunProject_, "actionRunProject"},
        {actCloseProject_, "actionCloseProject"},
        {actGitRefresh_, "actionGitRefresh"},
        {actGitStageAll_, "actionGitStageAll"},
        {actGitUnstageAll_, "actionGitUnstageAll"},
        {actGitCommit_, "actionGitCommit"},
        {actGitLog_, "actionGitLog"},
        {actGitDiff_, "actionGitDiff"},
        {actGitDiffStaged_, "actionGitDiffStaged"},
        {actGitInit_, "actionGitInit"},
    };
    for (auto it = actionNames.constBegin(); it != actionNames.constEnd();
         ++it) {
        it.key()->setObjectName(it.value());
    }
}

void MainWindow::createMenuBar()
{
    // Every menu carries a stable objectName: plugins address menus by id
    // (ide.ui.setMenuTitle("menuFile", ...)), so the ids are contract.
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("menuFile"));
    fileMenu->addAction(actNew_);
    fileMenu->addAction(actOpen_);
    fileMenu->addAction(actOpenFolder_);
    fileMenu->addSeparator();
    fileMenu->addAction(actSave_);
    fileMenu->addAction(actSaveAll_);
    fileMenu->addSeparator();
    fileMenu->addAction(actExit_);

    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->setObjectName(QStringLiteral("menuEdit"));
    editMenu->addAction(actUndo_);
    editMenu->addAction(actRedo_);
    editMenu->addSeparator();
    editMenu->addAction(actCut_);
    editMenu->addAction(actCopy_);
    editMenu->addAction(actPaste_);
    editMenu->addSeparator();
    editMenu->addAction(actFind_);
    editMenu->addAction(actFindNext_);
    editMenu->addAction(actFindPrev_);
    editMenu->addAction(actReplace_);
    editMenu->addSeparator();
    editMenu->addAction(actSearchInProject_);
    editMenu->addSeparator();
    editMenu->addAction(actSwitchHeaderSource_);
    editMenu->addSeparator();
    editMenu->addAction(actGotoDeclaration_);
    editMenu->addAction(actGotoImplementation_);
    editMenu->addAction(actFindReferences_);
    editMenu->addAction(actGoToSymbol_);
    editMenu->addSeparator();
    editMenu->addAction(actRenameSymbol_);
    editMenu->addAction(actQuickFix_);
    editMenu->addSeparator();
    editMenu->addAction(actToggleBookmark_);
    editMenu->addAction(actNextBookmark_);
    editMenu->addAction(actPrevBookmark_);
    editMenu->addSeparator();
    editMenu->addAction(actFormat_);
    editMenu->addAction(actFormatFolder_);
    editMenu->addSeparator();
    auto* foldingMenu = editMenu->addMenu(tr("&Folding"));
    foldingMenu->setObjectName(QStringLiteral("menuFolding"));
    foldingMenu->addAction(actFold_);
    foldingMenu->addAction(actUnfold_);
    foldingMenu->addSeparator();
    foldingMenu->addAction(actFoldAll_);
    foldingMenu->addAction(actUnfoldAll_);
    editMenu->addSeparator();
    editMenu->addAction(actSettings_);

    auto* projectMenu = menuBar()->addMenu(tr("&Project"));
    projectMenu->setObjectName(QStringLiteral("menuProject"));
    projectMenu->addAction(actOpenProject_);
    projectMenu->addSeparator();
    projectMenu->addAction(actConfigureProject_);
    projectMenu->addAction(actBuildProject_);
    projectMenu->addAction(actRunProject_);
    projectMenu->addSeparator();
    projectMenu->addAction(actFormatProject_);
    projectMenu->addSeparator();
    projectMenu->addAction(actCloseProject_);

    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->setObjectName(QStringLiteral("menuTools"));
    toolsMenu->addAction(actSyntaxCheck_);
    toolsMenu->addAction(actReloadSettings_);
    toolsMenu->addAction(actOpenConfigDir_);
    toolsMenu->addSeparator();
    toolsMenu->addAction(actCompile_);
    toolsMenu->addAction(actCompileRun_);
    toolsMenu->addAction(actRun_);
    toolsMenu->addAction(actStopRun_);

    auto* debugMenu = menuBar()->addMenu(tr("&Debug"));
    debugMenu->setObjectName(QStringLiteral("menuDebug"));
    debugMenu->addAction(actToggleBreakpoint_);
    debugMenu->addSeparator();
    debugMenu->addAction(actDebugStart_);
    debugMenu->addAction(actDebugStop_);
    debugMenu->addSeparator();
    debugMenu->addAction(actDebugContinue_);
    debugMenu->addAction(actDebugStepOver_);
    debugMenu->addAction(actDebugStepInto_);
    debugMenu->addAction(actDebugStepOut_);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("menuView"));
    viewMenu->addAction(actToggleExplorer_);
    viewMenu->addAction(actToggleOutput_);

    auto* gitMenu = menuBar()->addMenu(tr("&Git"));
    gitMenu->setObjectName(QStringLiteral("menuGit"));
    gitMenu->addAction(actGitRefresh_);
    gitMenu->addSeparator();
    gitMenu->addAction(actGitStageAll_);
    gitMenu->addAction(actGitUnstageAll_);
    gitMenu->addAction(actGitCommit_);
    gitMenu->addSeparator();
    gitMenu->addAction(actGitLog_);
    gitMenu->addAction(actGitDiff_);
    gitMenu->addAction(actGitDiffStaged_);
    gitMenu->addSeparator();
    gitMenu->addAction(actGitInit_);

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->setObjectName(QStringLiteral("menuHelp"));
    helpMenu->addAction(actAbout_);
}

void MainWindow::createToolBar()
{
    auto* tb = addToolBar(tr("Main"));
    tb->setMovable(false);
    tb->setIconSize(QSize(20, 20));
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->addAction(actNew_);
    tb->addAction(actOpen_);
    tb->addAction(actSave_);
    tb->addAction(actSaveAll_);
    tb->addSeparator();
    tb->addAction(actCompile_);
    tb->addAction(actCompileRun_);
    tb->addAction(actRun_);
    tb->addSeparator();
    tb->addAction(actBuildProject_);
    tb->addAction(actRunProject_);

    // Everything past the spacer goes to the far right, which is where a
    // build configuration belongs: it is a setting, not an action.
    auto* spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    tb->addWidget(createKitWidget());
    tb->addWidget(createBuildConfigurationWidget());
}

QWidget* MainWindow::createKitWidget()
{
    auto* container = new QWidget(this);
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 4, 0);
    layout->setSpacing(6);

    layout->addWidget(new QLabel(tr("Kit:"), container));

    kitCombo_ = new QComboBox(container);
    kitCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    kitCombo_->setToolTip(
        tr("The build environment used to compile and to resolve includes.\n"
           "Defined in config/kits.json next to the executable."));
    // Picking a kit changes the compiler (and with it which headers clangd
    // must resolve against) and the configuration list below it.
    connect(kitCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        const auto kit = activeKit();
        if (kit.has_value()) {
            outputPanel_->appendBuildOutput(
                tr("Kit: %1 (%2)").arg(kit->name.isEmpty() ? kit->id : kit->name,
                                       kit->cppCompiler));
        }
        applyToolchainHeaders();
        applyPreferences();  // kit styles (clang-format/clangd) follow the kit
        syncConfigCombo();
        // A different kit means a different compiler, so whatever was built
        // is no longer the thing this kit produces. syncConfigCombo() blocks
        // its own signals, so the configuration combo cannot report this.
        invalidateBuiltExecutables();
    });
    layout->addWidget(kitCombo_);
    return container;
}

void MainWindow::syncKitUi()
{
    if (kitCombo_ == nullptr) {
        return;
    }
    const QString previous = kitCombo_->currentData().toString();
    QSignalBlocker blocker(kitCombo_);
    kitCombo_->clear();
    // Enabled, usable kits only: a kit missing a compiler is not selectable.
    for (const LightCpp::Settings::Kit& kit : settings_->kits()) {
        if (!kit.enabled || !kit.isValid()) {
            continue;
        }
        kitCombo_->addItem(kit.name.isEmpty() ? kit.id : kit.name, kit.id);
    }
    // The settings' default kit wins; otherwise keep the previous choice,
    // otherwise the first entry.
    int index = kitCombo_->findData(settings_->defaults().defaultKitId);
    if (index < 0) {
        index = kitCombo_->findData(previous);
    }
    if (index < 0) {
        index = kitCombo_->count() > 0 ? 0 : -1;
    }
    kitCombo_->setCurrentIndex(index);
    syncConfigCombo();
}

QWidget* MainWindow::createBuildConfigurationWidget()
{
    auto* container = new QWidget(this);
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 4, 0);
    layout->setSpacing(6);

    layout->addWidget(new QLabel(tr("Configuration:"), container));

    configCombo_ = new QComboBox(container);
    // The entries are the active kit's buildConfigurations (Debug/Release
    // when the kit defines none); what the toolbar shows is exactly the
    // name the kit's flags are looked up by, and for CMake projects it is
    // also the CMAKE_BUILD_TYPE spelling.
    configCombo_->setToolTip(
        tr("Flags used when compiling, taken from the kit's build "
           "configurations.\n"
           "Debug adds debug info and leaves optimisation off;\n"
           "Release optimises and defines NDEBUG."));
    connect(configCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        outputPanel_->appendBuildOutput(
            tr("Build configuration: %1").arg(currentBuildConfigurationName()));
        // The binaries on disk belong to the configuration that was selected
        // when they were built. Keeping them cached here is how Run/Debug(F4)
        // ends up starting the old Debug build while the toolbar says Release.
        invalidateBuiltExecutables();
    });
    syncConfigCombo();
    layout->addWidget(configCombo_);
    return container;
}

void MainWindow::syncConfigCombo()
{
    if (configCombo_ == nullptr) {
        return;
    }
    const QString previous = configCombo_->currentData().toString();
    QSignalBlocker blocker(configCombo_);
    configCombo_->clear();
    QStringList names = {QStringLiteral("Debug"), QStringLiteral("Release")};
    const auto kit = activeKit();
    if (kit.has_value()) {
        names = settings_->buildConfigurationNames(*kit);
    }
    for (const QString& name : names) {
        configCombo_->addItem(name, name);
    }
    // Keep the previous selection when the new list still has it; else the
    // settings' default configuration; else Debug; else the first entry.
    int index = configCombo_->findData(previous);
    if (index < 0) {
        index = configCombo_->findData(
            settings_->defaults().defaultBuildConfiguration);
    }
    if (index < 0) {
        index = configCombo_->findData(QStringLiteral("Debug"));
    }
    if (index < 0) {
        index = configCombo_->count() > 0 ? 0 : -1;
    }
    configCombo_->setCurrentIndex(index);
}

QString MainWindow::currentBuildConfigurationName() const
{
    if (configCombo_ == nullptr) {
        return QStringLiteral("Debug");
    }
    const QString name = configCombo_->currentData().toString();
    return name.isEmpty() ? QStringLiteral("Debug") : name;
}

LightCpp::Project::BuildConfiguration MainWindow::buildConfiguration() const
{
    // Parsed back through the same function CMake settings are read with, so
    // an unexpected label degrades to the default instead of crashing.
    return LightCpp::Project::buildConfigurationFromName(
               currentBuildConfigurationName())
        .value_or(LightCpp::Project::BuildConfiguration::Debug);
}

std::optional<LightCpp::Settings::Kit> MainWindow::activeKit() const
{
    if (settings_ == nullptr || kitCombo_ == nullptr) {
        return std::nullopt;
    }
    const QString id = kitCombo_->currentData().toString();
    if (id.isEmpty()) {
        return std::nullopt;
    }
    return settings_->kit(id);
}

bool MainWindow::isMsvcKit(const LightCpp::Settings::Kit& kit) const
{
    // The family the kit declares is authoritative; a kit that names cl.exe
    // outright (no family, or one that does not cover MSVC) is MSVC too.
    // Anything else is treated as GNU/clang-style.
    return kit.compilerFamily.compare(QLatin1String("msvc"),
                                      Qt::CaseInsensitive) == 0
           || kit.cppCompiler.endsWith(QLatin1String("cl.exe"),
                                       Qt::CaseInsensitive);
}

bool MainWindow::activeKitIsMsvc() const
{
    const auto kit = activeKit();
    return kit.has_value() && isMsvcKit(*kit);
}

QString MainWindow::kitToolchainDirectory() const
{
    const auto kit = activeKit();
    if (!kit.has_value() || kit->cCompiler.isEmpty()) {
        return {};
    }
    return QFileInfo(kit->cCompiler).absolutePath();
}

void MainWindow::ensureMsvcToolchainDetected()
{
    // The detection runs on its own thread from startup on; a build can be the
    // first thing that needs its answer. Wait for it — bounded, and the status
    // bar says why the window pauses — instead of reporting "no toolchain" for
    // something still being looked up.
    if (msvcDetector_ == nullptr || !msvcDetector_->isRunning()) {
        return;
    }
    statusBar()->showMessage(tr("Looking for the MSVC toolchain…"));
    const bool done = msvcDetector_->waitForFinished();
    statusBar()->clearMessage();
    if (done) {
        // Blocking here means the queued detected() has not been delivered yet
        // (that needs the event loop), so apply the result directly; the later
        // copy is a no-op.
        onMsvcDetected(msvcDetector_->result());
    }
}

void MainWindow::invalidateBuiltExecutables()
{
    const bool hadOne = !lastProjectExe_.isEmpty() || !lastBuildExe_.isEmpty();
    lastProjectExe_.clear();
    lastBuildExe_.clear();
    if (hadOne && outputPanel_ != nullptr) {
        outputPanel_->appendBuildOutput(
            tr("Build output for the previous selection dropped — build again "
               "before running."));
    }
    updateProjectActions();
}

void MainWindow::createStatusBar()
{
    statusCursor_->setText(tr("Ln 1, Col 1"));
    statusLanguage_->setText(tr("C++"));
    statusFile_->setText(tr("Untitled"));

    // Branch / drift / dirty marker. Hidden until a repository turns up: a
    // folder outside git must not show a git slot at all. It reports, it does
    // not act.
    statusGit_->setObjectName(QStringLiteral("statusGit"));
    statusGit_->setToolTip(tr("Git branch and working tree state"));
    statusGit_->hide();

    // Encoding combo — rightmost in the status bar.
    // Layout: [UTF-8, ANSI (<local name>), <other encodings alphabetical>]
    // The user can change it to re-open the file with a forced codec.
    const QList<ComboEntry> entries = buildEncodingList();
    for (const ComboEntry& e : entries) {
        encodingCombo_->addItem(e.display,
                                QVariant::fromValue(e.encoding));
    }
    encodingCombo_->setMinimumWidth(160);
    connect(encodingCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onEncodingChanged);

    // Line-ending combo — sits next to the encoding drop-down. Shows the
    // active buffer's line endings (LF or CRLF); picking one sets a per-tab
    // override that the next save honours, converting the whole buffer.
    if (eolCombo_ == nullptr) {
        eolCombo_ = new QComboBox(this);
    }
    eolCombo_->clear();
    eolCombo_->addItems({QStringLiteral("LF"), QStringLiteral("CRLF")});
    eolCombo_->setToolTip(
        tr("Line endings of the active file. LF saves with \\n, CRLF with "
           "\\r\\n; the choice applies when the file is saved."));
    connect(eolCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onEolChanged);

    statusBar()->addWidget(statusFile_, 3);
    // statusGit_ is NOT added here: a hidden permanent widget still leaves
    // its separator behind in the status bar, so the slot is inserted only
    // when a repository actually reports in (onGitStatusReady) and removed
    // again the moment it is gone.
    statusBar()->addPermanentWidget(statusLanguage_);
    statusBar()->addPermanentWidget(statusCursor_);
    statusBar()->addPermanentWidget(eolCombo_);
    statusBar()->addPermanentWidget(encodingCombo_);
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
void MainWindow::onNewFile()
{
    const QString title = tr("Untitled-%1").arg(++untitledCounter_);
    CodeEditor* ed = createEditorTab(title);
    ed->setEncodingLabel(QStringLiteral("UTF-8"));
    // Give unsaved buffers a virtual .cpp path so the language server can
    // check them: clangd keys everything off file URIs and the extension
    // decides the parse mode. The file itself never touches the disk, but the
    // directory has to exist — clangd sets its VFS working directory to the
    // document's parent and otherwise fails with
    //   "failed to set CWD to .../lightcpp-untitled: No such file or directory"
    // on every didOpen, which it then retries for each diagnostic.
    const QString virtualDir = QDir(QDir::tempPath())
                                   .filePath(QStringLiteral("lightcpp-untitled"));
    QDir().mkpath(virtualDir);
    ed->setLintPath(QDir(virtualDir).filePath(title + QStringLiteral(".cpp")));
    updateEncodingCombo(ed);
    updateEolCombo(ed);
}

void MainWindow::onOpenFile()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Open Files"), QString(),
        tr("All Files (*);;C/C++ Sources (*.c *.cpp *.cc *.cxx *.h *.hpp);;"
           "CMake (*.cmake CMakeLists.txt);;Text (*.txt)"));
    for (const auto& path : paths) {
        openFileInTab(path);
    }
}

QString MainWindow::effectiveLineEnding(CodeEditor* ed) const
{
    // Priority: the per-tab override from the status-bar combo, then the
    // line-ending style the file itself was loaded with (keep what is
    // there — an LF file must not become CRLF because a global default
    // says so), then the global files.lineEnding setting for new files.
    QString eol = ed->property("lineEndingOverride")
                      .toString();
    if (eol.isEmpty()) {
        eol = ed->property("detectedLineEnding").toString();
    }
    if (eol.isEmpty()) {
        eol = lineEndingForSaving_;
    }
    return eol;
}

std::optional<QByteArray> MainWindow::encodedSavePayload(CodeEditor* ed)
{
    namespace LT = LightCpp::Text;

    // The tab's encoding — what the drop-down shows — decides the bytes on
    // disk. An unparseable label falls back to UTF-8 (the historical
    // behavior for buffers that never touched the encoding UI).
    const auto parsed = LT::fromName(ed->encodingLabel().toUtf8());
    const LT::TextEncoding encoding =
        parsed.value_or(LT::TextEncoding::Utf8);

    // UTF-16/32 files get a BOM so detection on reopen is unambiguous;
    // UTF-8 stays BOM-less as before.
    const bool addBom = encoding == LT::TextEncoding::Utf16Le
                        || encoding == LT::TextEncoding::Utf16Be
                        || encoding == LT::TextEncoding::Utf32Le
                        || encoding == LT::TextEncoding::Utf32Be;

    // Newline translation happens on the text (before encoding): a byte
    // level 0x0A->0x0D0A replace would corrupt UTF-16/32 payloads.
    const QString eol = effectiveLineEnding(ed);
    QString text = ed->toPlainText();
    if (eol == QLatin1String("CRLF")) {
        text.replace(u'\n', QStringLiteral("\r\n"));
    }

    const LT::ConvertResult result = LT::encodeFromQString(text, encoding, addBom);
    if (!result.hasValue()) {
        QMessageBox::warning(
            this, tr("Encoding Error"),
            tr("Cannot save %1 in %2: %3.\nThe text contains characters the "
               "encoding cannot represent; switch the encoding (e.g. to "
               "UTF-8) or remove those characters.")
                .arg(tabTitleFor(ed),
                     QString::fromLatin1(LT::toName(encoding)),
                     QString::fromLatin1(LT::toString(result.error()))));
        return std::nullopt;
    }
    return result.value();
}

void MainWindow::onSaveFile()
{
    CodeEditor* ed = currentEditor();
    if (!ed) return;

    QString path = ed->filePath();
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(
            this, tr("Save File"), QString(),
            tr("All Files (*);;C++ Sources (*.cpp *.h);;Text (*.txt)"));
        if (path.isEmpty()) return;
        ed->setFilePath(path);
    }

    if (!saveEditorToDisk(ed)) {
        // The write is atomic, so a failure at this point means the file on
        // disk still holds exactly what it held before — worth saying, since
        // the opposite assumption is what makes a failed save frightening.
        QMessageBox::warning(
            this, tr("Save Error"),
            tr("Cannot write to %1.\n\nThe file is unchanged; see the Build "
               "output for the reason.").arg(path));
    }
}

/// The disk-write half of saving, without any user interaction: encodes the
/// buffer, writes it, syncs the language server and the problem list. Used
/// by Ctrl+S and by the auto-save timer alike; returns false when the
/// payload cannot be produced or the file cannot be written.
bool MainWindow::saveEditorToDisk(CodeEditor* ed)
{
    const QString path = ed->filePath();
    const std::optional<QByteArray> payload = encodedSavePayload(ed);
    if (!payload.has_value()) {
        return false;
    }

    // Atomic write: the bytes land in a temporary file beside the target and
    // only replace it once they are complete. Truncating the target first
    // (QIODevice::Truncate, as this used to do) means a full disk, a locked
    // file or a power cut destroys the old contents *and* leaves nothing of
    // the new ones — while the UI below still reported "Saved".
    if (const std::optional<LightCpp::Text::WriteFileError> writeError =
            LightCpp::Text::writeFileAtomically(path, *payload)) {
        outputPanel_->appendBuildOutput(
            tr("Cannot save %1: %2")
                .arg(path,
                     QString::fromUtf8(LightCpp::Text::toString(*writeError))));
        return false;
    }

    // Our own write will fire fileChanged within milliseconds; the
    // timestamp marks it as ours so the watcher does not reload.
    selfWriteMs_.insert(path, QDateTime::currentMSecsSinceEpoch());
    // QFileSystemWatcher silently drops files that were deleted while
    // watched, so a save-as-new-file (or a recreate) needs a re-add.
    if (!fileWatcher_->files().contains(path)) {
        fileWatcher_->addPath(path);
    }
    ed->setModified(false);
    // The buffer now matches what was written: record the style that
    // was actually used as the file's line endings, so the status-bar
    // combo and the next save agree with the bytes on disk even after
    // a save-as-new-file.
    ed->setProperty("detectedLineEnding", effectiveLineEnding(ed));
    updateEolCombo(ed);
    const int idx = tabWidget_->indexOf(ed);
    if (idx >= 0) {
        tabWidget_->setTabText(idx, QFileInfo(path).fileName());
    }
    statusFile_->setText(path);
    statusLanguage_->setText(languageFromSuffix(path));
    outputPanel_->appendBuildOutput(tr("Saved: %1").arg(path));
    // Re-sync the language server: the buffer may have moved from its
    // virtual (untitled) path to the real location, and the extension may
    // have changed the parse mode. changeDocument() sends didChange and
    // opens the document itself when the server has not seen it yet, so a
    // save no longer emits a second didOpen() for an already-open file —
    // which the shared save path would otherwise repeat on every F7 too.
    const QString oldLint = ed->lintPath();
    if (lsp_->isRunning()) {
        if (!oldLint.isEmpty() && QFileInfo(oldLint).absoluteFilePath()
                                   != QFileInfo(path).absoluteFilePath()) {
            lsp_->closeDocument(oldLint);
        }
        ed->setLintPath(path);
        if (isCppLikeFile(path)) {
            lsp_->changeDocument(path, ed->toPlainText());
        }
    }
    // The problem list is keyed by lint path — re-key it, otherwise the
    // entries would end up orphaned under the old virtual path.
    const QString newLint = ed->lintPath();
    if (oldLint != newLint && diagnosticsByFile_.contains(oldLint)) {
        diagnosticsByFile_.insert(newLint, diagnosticsByFile_.take(oldLint));
        rebuildProblems();
    }
    // languageService.clangFormat.formatOnSave: the buffer is formatted
    // after the write. The answer is asynchronous, so the formatted text
    // lands as a modification the user saves like any other edit.
    if (formatOnSave_ && clangFormatEnabled_ && isCppLikeFile(path)) {
        onFormatDocument();
    }
    return true;
}

void MainWindow::onAutoSaveTimeout()
{
    // Silent pass over the open buffers: anything modified with a real file
    // name goes back to disk. Untitled buffers stay untouched (no path, and
    // prompting for one would defeat the point of auto-save), read-only
    // editors cannot be written anyway.
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i));
        if (ed == nullptr || !ed->isModified() || ed->isReadOnly()
            || ed->filePath().isEmpty()) {
            continue;
        }
        if (saveEditorToDisk(ed)) {
            outputPanel_->appendBuildOutput(
                tr("Auto-saved: %1").arg(ed->filePath()));
        }
    }
}

void MainWindow::onSaveAll()
{
    for (int i = 0; i < tabWidget_->count(); ++i) {
        tabWidget_->setCurrentIndex(i);
        onSaveFile();
    }
}

void MainWindow::onCloseTab(int index)
{
    if (qobject_cast<StartupPage*>(tabWidget_->widget(index)) != nullptr) {
        // The start page owns no buffer; its close goes through the shared
        // path, which removes the tab and deletes the page.
        closeStartupTab();
        return;
    }
    if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(index))) {
        if (ed->isModified()) {
            const auto reply = QMessageBox::warning(
                this, tr("Unsaved Changes"),
                tr("File \"%1\" has unsaved changes. Save?")
                    .arg(tabWidget_->tabText(index)),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
            if (reply == QMessageBox::Yes) {
                tabWidget_->setCurrentIndex(index);
                onSaveFile();
            } else if (reply == QMessageBox::Cancel) {
                return;
            }
        }
    }
    if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(index))) {
        const QString lintPath = ed->lintPath();
        if (!lintPath.isEmpty() && lsp_->isRunning()
            && isCppLikeFile(lintPath)) {
            lsp_->closeDocument(lintPath);
        }
        // The buffer is going away — drop its Problems entries too.
        if (diagnosticsByFile_.remove(lintPath) > 0) {
            rebuildProblems();
        }
        watchEditorFile(ed, false);
    }
    tabWidget_->removeTab(index);
}

void MainWindow::onFileActivated(const QString& path)
{
    openFileInTab(path);
    // Opening a file elsewhere can move the whole session into a different
    // repository, so the git pane follows the active file.
    syncGitWorkingDirectory();
}

void MainWindow::onCursorPositionChanged()
{
    CodeEditor* ed = currentEditor();
    if (!ed) {
        statusCursor_->clear();
        return;
    }
    const QTextCursor c = ed->textCursor();
    const int line = c.blockNumber() + 1;
    // column = position within block + 1
    int col = c.positionInBlock() + 1;
    // Adjust for tab width (simple: 1 tab = 4 visual cols)
    const QString blockText = c.block().text().left(c.positionInBlock());
    col += blockText.count(u'\t') * 3;
    statusCursor_->setText(tr("Ln %1, Col %2").arg(line).arg(col));
    updateBreadcrumb();
}

void MainWindow::onEncodingChanged(int index)
{
    if (index < 0) return;
    const auto encoding = encodingCombo_->itemData(index)
                              .value<LightCpp::Text::TextEncoding>();
    CodeEditor* ed = currentEditor();
    if (!ed) return;

    // If the encoding matches what the editor already shows, no-op.
    if (ed->encodingLabel() == QString::fromLatin1(
            LightCpp::Text::toName(encoding))) {
        return;
    }
    reloadWithEncoding(encoding);
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, tr("About LightC++"),
        tr("<h3>LightC++</h3>"
           "<p>A lightweight C/C++ IDE built with Qt6.</p>"
           "<p>Modules: LightCppCore (UTF-8), LightCppText (encoding detection), "
           "LightCppApp (GUI).</p>"
           "<p>Version: 0.1.0 &mdash; plain-text editor phase.</p>"));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
CodeEditor* MainWindow::currentEditor() const
{
    return qobject_cast<CodeEditor*>(tabWidget_->currentWidget());
}

void MainWindow::updateEditActions()
{
    const bool hasEditor = currentEditor() != nullptr;
    actUndo_->setEnabled(hasEditor);
    actRedo_->setEnabled(hasEditor);
    actCut_->setEnabled(hasEditor);
    actCopy_->setEnabled(hasEditor);
    // Paste also needs something on the clipboard.
    actPaste_->setEnabled(hasEditor
                          && !QGuiApplication::clipboard()->text().isEmpty());
}

void MainWindow::onSearchInProject()
{
    // The search scope is the Explorer root when one is open, else the
    // active file's folder. With neither, ask once — no silent global scan.
    QString root = fileExplorer_->rootPath();
    if (root.isEmpty()) {
        if (CodeEditor* ed = currentEditor();
            ed != nullptr && !ed->filePath().isEmpty()) {
            root = QFileInfo(ed->filePath()).absolutePath();
        }
    }
    if (root.isEmpty()) {
        root = QFileDialog::getExistingDirectory(
            this, tr("Choose the Folder to Search In"), QString());
        if (root.isEmpty()) {
            return;
        }
    }

    auto* dialog = new ProjectSearchDialog(root, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &ProjectSearchDialog::resultActivated,
            this, [this](const QString& path, int line) {
        CodeEditor* ed = openFileInTab(path);
        if (ed == nullptr) {
            return;
        }
        // gotoLine() takes a 0-based block number; search hits are 1-based.
        gotoLine(line - 1);
        ed->setFocus();
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::onSwitchHeaderSource()
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr || ed->filePath().isEmpty()) {
        return;
    }
    const QFileInfo info(ed->filePath());
    const QString ext = info.suffix().toLower();

    // The two extension families and their translation table. A .h pairs
    // with either kind of source; a .c prefers C sources but still falls
    // back to C++ ones.
    static const QStringList headerExts = {
        QStringLiteral("h"), QStringLiteral("hh"), QStringLiteral("hpp"),
        QStringLiteral("hxx")};
    static const QStringList sourceExts = {
        QStringLiteral("cpp"), QStringLiteral("cc"), QStringLiteral("cxx"),
        QStringLiteral("c++"), QStringLiteral("c")};

    QStringList targets;
    if (ext == QLatin1String("h") || ext == QLatin1String("hh")
        || ext == QLatin1String("hpp") || ext == QLatin1String("hxx")) {
        targets = sourceExts;
    } else if (ext == QLatin1String("c")) {
        targets = {QStringLiteral("h"), QStringLiteral("hh"),
                   QStringLiteral("hpp"), QStringLiteral("hxx"),
                   QStringLiteral("cpp"), QStringLiteral("cc"),
                   QStringLiteral("cxx"), QStringLiteral("c++")};
    } else if (sourceExts.contains(ext)) {
        targets = headerExts;
    } else {
        outputPanel_->appendBuildOutput(
            tr("Switch Header/Source: %1 is neither a C/C++ header nor a "
               "source file.").arg(info.fileName()));
        return;
    }

    // Same directory first — the overwhelmingly common layout and instant
    // to check (a handful of stat calls, not a tree walk).
    const QDir dir = info.absoluteDir();
    const QString base = info.completeBaseName();
    for (const QString& targetExt : targets) {
        const QString candidate = dir.filePath(base + u'.' + targetExt);
        if (QFileInfo::exists(candidate)) {
            openFileInTab(candidate);
            return;
        }
    }
    outputPanel_->appendBuildOutput(
        tr("Switch Header/Source: no %1 next to %2.")
            .arg(targets.join(QStringLiteral("/")), info.fileName()));
}

void MainWindow::requestNavigation(int kind)
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr || !lsp_->isRunning()) {
        return;
    }
    const QString lintPath = ed->lintPath();
    if (lintPath.isEmpty() || !isCppLikeFile(lintPath)) {
        return;
    }
    // Navigation answers lean on clangd's index: same gates as Ctrl+Click
    // go-to-definition.
    if (!clangdNavigationEnabled_
        || fileExceeds(lintPath, largeFileParseMB_)
        || fileExceeds(lintPath, largeFileIndexMB_)) {
        return;
    }
    // Push the current text first (lint didChange is debounced), then ask.
    lsp_->changeDocument(lintPath, ed->toPlainText());
    const int line = ed->textCursor().blockNumber();
    // columnNumber() counts UTF-16 code units within the block — exactly
    // what LSP positions use.
    const int column = ed->textCursor().columnNumber();

    int reqId = 0;
    switch (kind) {
    case 0:
        reqId = lsp_->requestDeclaration(lintPath, line, column);
        break;
    case 1:
        reqId = lsp_->requestImplementation(lintPath, line, column);
        break;
    case 2:
        reqId = lsp_->requestReferences(lintPath, line, column);
        break;
    default:
        return;
    }
    if (reqId > 0) {
        latestLocationRequestId_ = reqId;
        navigationKinds_[reqId] = kind;
    }
}

void MainWindow::showSignatureHelp(
    const LightCpp::App::LspSignatureHelp& help)
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr) {
        return;
    }
    if (help.label.isEmpty()) {
        // Not inside a call (or the caret left the argument list).
        ed->showSignatureHelp(QString());
        return;
    }
    // Rich text: the active parameter bolded inside the escaped label. The
    // ranges are cut BEFORE escaping — toHtmlEscaped() changes the length
    // (& < → &lt;), so slicing afterwards would point at the wrong bytes.
    QString html;
    if (help.paramStart >= 0 && help.paramEnd > help.paramStart
        && help.paramEnd <= help.label.size()) {
        html = help.label.left(help.paramStart).toHtmlEscaped()
               + QStringLiteral("<b>")
               + help.label.mid(help.paramStart,
                                help.paramEnd - help.paramStart)
                     .toHtmlEscaped()
               + QStringLiteral("</b>")
               + help.label.mid(help.paramEnd).toHtmlEscaped();
    } else {
        html = help.label.toHtmlEscaped();
    }
    ed->showSignatureHelp(html);
}

// ---------------------------------------------------------------------------
// Outline / document symbols
// ---------------------------------------------------------------------------

QWidget* MainWindow::createOutlinePanel()
{
    outlineTree_ = new QTreeWidget(this);
    outlineTree_->setObjectName(QStringLiteral("outlineTree"));
    outlineTree_->setColumnCount(1);
    outlineTree_->setHeaderHidden(true);
    outlineTree_->setUniformRowHeights(true);
    outlineTree_->setMinimumWidth(140);
    outlineTree_->setContextMenuPolicy(Qt::NoContextMenu);
    // A palette placeholder while nothing has been parsed yet.
    outlineTree_->setWordWrap(false);

    // Single click jumps (VSCode jumps on click in its outline).
    connect(outlineTree_, &QTreeWidget::itemClicked,
            this, [this](QTreeWidgetItem* item, int) {
        if (item != nullptr) {
            jumpToSymbolLine(item->data(0, Qt::UserRole).toInt());
        }
    });

    auto* box = new QWidget(this);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(outlineTree_);
    return box;
}

void MainWindow::requestOutline()
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr || !lsp_->isRunning()) {
        return;
    }
    const QString lintPath = ed->lintPath();
    if (lintPath.isEmpty() || !isCppLikeFile(lintPath)) {
        return;
    }
    // Push the buffer first: the lint didChange is debounced, so a fresh
    // edit would otherwise produce a stale outline.
    lsp_->changeDocument(lintPath, ed->toPlainText());
    const int reqId = lsp_->requestDocumentSymbols(lintPath);
    if (reqId > 0) {
        latestOutlineRequestId_ = reqId;
    }
}

void MainWindow::applyOutlineSymbols(
    const QString& filePath,
    const QList<LightCpp::App::LspSymbol>& symbols)
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr || ed->lintPath() != filePath) {
        return;  // the answer describes a tab that is no longer active
    }
    if (outlineTree_ == nullptr) {
        return;  // the outline panel was never created — nothing to fill
    }

    outlineTree_->clear();
    outlineEntries_.clear();
    outlineFilePath_ = filePath;

    // Walk the (possibly nested) symbol list, building tree items and the
    // flattened cache used by the Ctrl+Shift+O dialog in one pass. Top-level
    // symbols hang off the tree's invisible root item.
    std::function<void(const LightCpp::App::LspSymbol&, QTreeWidgetItem*, int)>
        addSymbol = [&](const LightCpp::App::LspSymbol& symbol,
                        QTreeWidgetItem* parent, int depth) {
            auto* item = new QTreeWidgetItem(
                parent != nullptr ? parent
                                  : outlineTree_->invisibleRootItem());
            QString label = symbol.name;
            if (!symbol.detail.isEmpty()) {
                label += QStringLiteral("  ");
                label += symbol.detail.left(80);
            }
            item->setText(0, label);
            item->setToolTip(0, symbol.detail.isEmpty() ? symbol.name
                                                        : symbol.detail);
            item->setData(0, Qt::UserRole, symbol.line);

            OutlineEntry entry;
            entry.name = symbol.name;
            entry.detail = symbol.detail;
            entry.kind = symbol.kind;
            entry.line = symbol.line;
            entry.endLine = symbol.endLine;
            entry.depth = depth;
            outlineEntries_.append(entry);

            for (const LightCpp::App::LspSymbol& child : symbol.children) {
                addSymbol(child, item, depth + 1);
            }
        };
    for (const LightCpp::App::LspSymbol& symbol : symbols) {
        addSymbol(symbol, nullptr, 0);
    }
    outlineTree_->expandToDepth(0);
    updateBreadcrumb();
}

void MainWindow::jumpToSymbolLine(int line)
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr || line < 0) {
        return;
    }
    const QTextBlock block = ed->document()->findBlockByNumber(line);
    if (!block.isValid()) {
        return;
    }
    QTextCursor cursor(block);
    ed->setTextCursor(cursor);
    ed->centerCursor();
    ed->setFocus();
}

void MainWindow::updateBreadcrumb()
{
    CodeEditor* ed = currentEditor();
    // The enclosing-symbol chain: every outline entry whose body spans the
    // caret line, in document order (the flat list is depth-first, so outer
    // scopes come first — exactly breadcrumb order).
    QString key;
    // The cached entries describe one specific file: after a tab switch the
    // chain must hide instead of showing (and jumping to) the previous
    // file's scopes.
    if (ed != nullptr && !outlineEntries_.isEmpty()
        && ed->lintPath() == outlineFilePath_) {
        const int caretLine = ed->textCursor().blockNumber();
        QList<const OutlineEntry*> chain;
        for (const OutlineEntry& entry : outlineEntries_) {
            if (entry.line <= caretLine && caretLine <= entry.endLine) {
                chain.append(&entry);
                key += entry.name + u'\x1F';
            }
        }
        if (chain.size() > 5) {
            chain = chain.mid(chain.size() - 5);  // innermost five scopes
        }
        if (!chain.isEmpty()) {
            key = QStringLiteral("%1|%2").arg(ed->lintPath()).arg(key);
            if (key == breadcrumbKey_) {
                return;  // same scope chain — nothing to rebuild
            }
            breadcrumbKey_ = key;

            while (breadcrumbLayout_->count() > 1) {
                QLayoutItem* item = breadcrumbLayout_->takeAt(0);
                if (item->widget() != nullptr) {
                    item->widget()->deleteLater();
                }
                delete item;
            }
            for (int i = 0; i < chain.size(); ++i) {
                if (i > 0) {
                    auto* sep = new QLabel(QStringLiteral("\u203A"),
                                           breadcrumbBar_);
                    breadcrumbLayout_->insertWidget(
                        breadcrumbLayout_->count() - 1, sep);
                }
                const OutlineEntry* entry = chain[i];
                auto* button = new QPushButton(entry->name, breadcrumbBar_);
                button->setFlat(true);
                button->setCursor(Qt::PointingHandCursor);
                button->setToolTip(entry->detail.isEmpty()
                                       ? entry->name
                                       : entry->detail);
                connect(button, &QPushButton::clicked, this,
                        [this, line = entry->line] { jumpToSymbolLine(line); });
                breadcrumbLayout_->insertWidget(
                    breadcrumbLayout_->count() - 1, button);
            }
            breadcrumbBar_->show();
            return;
        }
    }
    breadcrumbKey_.clear();
    breadcrumbBar_->hide();
}

void MainWindow::showSymbolSearch()
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr) {
        return;
    }
    // The cache holds one file's symbols, and after a tab switch it still
    // describes the file that was active when the answer arrived. Listing
    // those used to send the caret of the file on screen to a line that means
    // nothing there, so the cache is only used when it is this file's.
    const bool cacheMatchesEditor =
        !ed->lintPath().isEmpty() && ed->lintPath() == outlineFilePath_;
    if (!cacheMatchesEditor) {
        // Nothing usable cached (another file's symbols, a stale file, or the
        // answer has not arrived): ask once and let the user retry in a
        // moment rather than answer with somebody else's outlines.
        requestOutline();
    }

    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(tr("Go to Symbol in File"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(dialog);

    auto* filterEdit = new QLineEdit(dialog);
    filterEdit->setPlaceholderText(
        tr("Filter symbols (Enter jumps, Esc closes)"));
    layout->addWidget(filterEdit);

    auto* list = new QListWidget(dialog);
    list->setUniformItemSizes(true);
    layout->addWidget(list, 1);

    if (!cacheMatchesEditor) {
        // Say what is happening instead of presenting an empty box: the
        // first Ctrl+Shift+O after a tab switch lands here.
        const QString message = isCppLikeFile(ed->lintPath())
            ? tr("Reading symbols for %1 — press Ctrl+Shift+O again in a "
                 "moment.")
                  .arg(tabTitleFor(ed))
            : tr("No symbol outline for this kind of file.");
        auto* pending = new QListWidgetItem(message, list);
        pending->setFlags(Qt::NoItemFlags);
        dialog->show();
        return;
    }

    for (const OutlineEntry& entry : outlineEntries_) {
        QString label = entry.name;
        if (!entry.detail.isEmpty()) {
            label += QStringLiteral("  ") + entry.detail.left(80);
        }
        auto* item = new QListWidgetItem(label, list);
        item->setData(Qt::UserRole, entry.line);
    }
    if (list->count() > 0) {
        list->setCurrentRow(0);
    }

    auto jumpFrom = [this, dialog, list](QListWidgetItem* item) {
        if (item == nullptr) {
            return;
        }
        jumpToSymbolLine(item->data(Qt::UserRole).toInt());
        dialog->close();
    };
    connect(list, &QListWidget::itemActivated, this,
            [jumpFrom](QListWidgetItem* item) { jumpFrom(item); });
    connect(list, &QListWidget::itemClicked, this,
            [jumpFrom](QListWidgetItem* item) { jumpFrom(item); });

    connect(filterEdit, &QLineEdit::textChanged, this,
            [list](const QString& text) {
                const QString needle = text.trimmed();
                for (int i = 0; i < list->count(); ++i) {
                    QListWidgetItem* item = list->item(i);
                    item->setHidden(!needle.isEmpty()
                                    && !item->text().contains(
                                        needle, Qt::CaseInsensitive));
                }
                // Keep a visible row selected: first unhidden item.
                for (int i = 0; i < list->count(); ++i) {
                    if (!list->item(i)->isHidden()) {
                        list->setCurrentRow(i);
                        break;
                    }
                }
            });
    connect(filterEdit, &QLineEdit::returnPressed, this, [list, jumpFrom]() {
        jumpFrom(list->currentItem());
    });

    dialog->resize(560, 420);
    filterEdit->setFocus();
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

// ---------------------------------------------------------------------------
// Workspace edits: rename / quick fix
// ---------------------------------------------------------------------------

int MainWindow::applyWorkspaceEdits(
    const QList<LightCpp::App::LspTextEdit>& edits, int* skippedFiles)
{
    if (skippedFiles != nullptr) {
        *skippedFiles = 0;
    }
    // Group per open editor. QSet preserves insertion order? No — use a
    // QList of pairs to keep a deterministic order.
    QList<QPair<CodeEditor*, QList<LightCpp::App::LspTextEdit>>> grouped;
    QSet<QString> missingFiles;
    for (const auto& edit : edits) {
        CodeEditor* owner = nullptr;
        for (int i = 0; i < tabWidget_->count(); ++i) {
            auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i));
            if (ed != nullptr
                && QFileInfo(ed->lintPath()).absoluteFilePath()
                       == QFileInfo(edit.filePath).absoluteFilePath()) {
                owner = ed;
                break;
            }
        }
        if (owner == nullptr) {
            missingFiles.insert(edit.filePath);
            continue;
        }        // Find-or-append the editor's bucket (server edits for one document
        // are usually adjacent, so the loop is short).
        int bucket = -1;
        for (int i = 0; i < grouped.size(); ++i) {
            if (grouped[i].first == owner) {
                bucket = i;
                break;
            }
        }
        if (bucket < 0) {
            grouped.append(qMakePair(owner,
                                     QList<LightCpp::App::LspTextEdit>()));
            bucket = grouped.size() - 1;
        }
        grouped[bucket].second.append(edit);
    }

    int applied = 0;
    int droppedEdits = 0;
    for (const auto& pair : grouped) {
        CodeEditor* ed = pair.first;
        QList<LightCpp::App::LspTextEdit> list = pair.second;
        // Bottom-up: sort descending by (line, column) so applying one edit
        // never shifts the positions of the ones still to come.
        std::sort(list.begin(), list.end(),
                  [](const LightCpp::App::LspTextEdit& a,
                     const LightCpp::App::LspTextEdit& b) {
                      if (a.line != b.line) {
                          return a.line > b.line;
                      }
                      return a.column > b.column;
                  });
        QTextCursor cursor(ed->document());
        cursor.beginEditBlock();
        for (const auto& edit : list) {
            const QTextBlock startBlock =
                ed->document()->findBlockByNumber(edit.line);
            const QTextBlock endBlock =
                ed->document()->findBlockByNumber(edit.endLine);
            if (!startBlock.isValid() || !endBlock.isValid()) {
                ++droppedEdits;  // the buffer drifted past the server's view
                continue;
            }
            // Clamp to the real line extents: a stale column (or the line's
            // newline position) would otherwise place a cursor at an invalid
            // position and silently corrupt the following edits.
            const int startCol =
                qBound(0, edit.column, qMax(startBlock.length() - 1, 0));
            const int endCol =
                qBound(0, edit.endColumn, qMax(endBlock.length() - 1, 0));
            cursor.setPosition(startBlock.position() + startCol);
            cursor.setPosition(endBlock.position() + endCol,
                               QTextCursor::KeepAnchor);
            cursor.insertText(edit.newText);
            ++applied;
        }
        cursor.endEditBlock();
    }
    if (skippedFiles != nullptr) {
        *skippedFiles = missingFiles.size();
    }
    // Unopened files are not edited at all — naming them is the only way the
    // user can finish a rename by hand instead of discovering the half-renamed
    // state at build time.
    for (const QString& missing : std::as_const(missingFiles)) {
        outputPanel_->appendProgramOutput(
            tr("Edit not applied — file is not open in a tab: %1")
                .arg(missing));
    }
    if (droppedEdits > 0) {
        outputPanel_->appendProgramOutput(
            tr("%1 edit(s) were dropped because the document changed since "
               "the server computed them")
                .arg(droppedEdits));
    }
    return applied;
}

void MainWindow::renameSymbolAtCursor()
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr || !lsp_->isRunning()) {
        return;
    }
    const QString lintPath = ed->lintPath();
    if (lintPath.isEmpty() || !isCppLikeFile(lintPath)) {
        return;
    }
    // The identifier under the caret prefills the prompt.
    QTextCursor word = ed->textCursor();
    word.select(QTextCursor::WordUnderCursor);
    const QString currentName = word.selectedText();

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, tr("Rename Symbol"), tr("New name for \"%1\":").arg(currentName),
        QLineEdit::Normal, currentName, &ok);
    if (!ok || newName.isEmpty() || newName == currentName) {
        return;
    }
    // clangd wants the caret ON the symbol; WordUnderCursor's anchor may sit
    // before the first character — use the selection start which is inside
    // the word either way.
    const int line = word.blockNumber();
    const int column = word.selectionStart() - word.block().position();
    lsp_->changeDocument(lintPath, ed->toPlainText());
    const int reqId = lsp_->requestRename(lintPath, line, column, newName);
    if (reqId > 0) {
        latestRenameRequestId_ = reqId;
    } else {
        // 0 means the request never went out — the handshake has not finished,
        // the document is not open, or the server is down. Dropping that made
        // the prompt a dead end: it closed and nothing else happened, with no
        // hint that the rename was never attempted.
        appendServerProcessLog(
            tr("Rename was not requested: the language server is not ready."));
        statusBar()->showMessage(
            tr("Rename needs a running language server — see the language "
               "server output."),
            6000);
    }
}

void MainWindow::requestQuickFix()
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr || !lsp_->isRunning()) {
        return;
    }
    const QString lintPath = ed->lintPath();
    if (lintPath.isEmpty() || !isCppLikeFile(lintPath)) {
        return;
    }
    // Collect the diagnostics whose range touches the caret's line — that
    // is the context clangd matches fixes against.
    const int caretLine = ed->textCursor().blockNumber();
    QList<LightCpp::App::LspDiagnostic> nearby;
    for (const auto& d : diagnosticsByFile_.value(lintPath)) {
        if (caretLine >= d.startLine - 0 && caretLine <= d.endLine + 0) {
            nearby.append(d);
        }
    }
    lsp_->changeDocument(lintPath, ed->toPlainText());
    const int reqId = lsp_->requestCodeActions(
        lintPath, caretLine, ed->textCursor().positionInBlock(), nearby);
    if (reqId > 0) {
        latestCodeActionRequestId_ = reqId;
    } else {
        // Same dead end as Rename: the menu item is enabled, so the user is
        // entitled to know the request was never sent.
        appendServerProcessLog(tr(
            "Quick fixes were not requested: the language server is not ready."));
        statusBar()->showMessage(
            tr("Quick fixes need a running language server — see the language "
               "server output."),
            6000);
    }
}

QString MainWindow::tabTitleFor(const CodeEditor* editor) const
{
    const int index = tabWidget_->indexOf(editor);
    if (index < 0) {
        return tr("the active file");
    }
    QString title = tabWidget_->tabText(index);
    if (title.endsWith(u'\u25CF')) {
        title = title.left(title.length() - 2);  // drop " ●"
    }
    return title;
}

CodeEditor* MainWindow::createEditorTab(const QString& title)
{
    auto* editor = new CodeEditor(this);
    // The settings' fonts.editor choice applies to every editor created from
    // now on; the toolbar-font path goes through applyPreferences().
    if (!editorFont_.family().isEmpty()) {
        editor->setEditorFont(editorFont_);
    }
    editor->setLongLinesWrap(longLinesWrap_);
    editor->setShowWhitespaces(editorsShowWhitespaces_);
    editor->setShowIndentGuides(editorsShowIndentGuides_);
    connect(editor, &QPlainTextEdit::cursorPositionChanged,
            this, &MainWindow::onCursorPositionChanged);
    // The tab's modification dot follows the document's modified flag, not
    // textChanged: QTextDocument emits modificationChanged() in both
    // directions — the first edit after save marks it, setModified(false)
    // on save/load clears it, and undoing back to the saved state clears
    // it too — so the dot can never drift out of sync with the real state.
    connect(editor->document(), &QTextDocument::modificationChanged,
            this, [this, editor](bool modified) {
        const int idx = tabWidget_->indexOf(editor);
        if (idx < 0) {
            return;
        }
        QString t = tabWidget_->tabText(idx);
        // Add/remove the modification dot.
        if (modified && !t.endsWith(u'\u25CF')) {
            t += QStringLiteral(" \u25CF");
            tabWidget_->setTabText(idx, t);
        } else if (!modified && t.endsWith(u'\u25CF')) {
            t = t.left(t.length() - 2);
            tabWidget_->setTabText(idx, t);
        }
    });

    connect(editor, &QPlainTextEdit::textChanged, this, [this, editor]() {
        // Debounce a didChange notification to the language server.
        scheduleLint(editor);
    });

    // Diagnostics flow: clangd -> editor (squiggles) -> problem list. The
    // editor normalises positions before reporting, so the list and the
    // underlines always agree.
    // The merging pass (applyCompilerDiagnosticsToEditors) pushes LSP+compiler
    // diagnostics INTO the editor; the echo this signal produces must not be
    // stored back into diagnosticsByFile_ — that map stays LSP-only, or the
    // compiler entries would leak into the LSP problem list.
    connect(editor, &CodeEditor::diagnosticsChanged,
            this, [this](const QString& path,
                          const QList<LightCpp::App::LspDiagnostic>& diagnostics) {
        if (applyingCompilerDiagnostics_) {
            return;
        }
        if (path.isEmpty()) {
            return;
        }
        if (diagnostics.isEmpty()) {
            diagnosticsByFile_.remove(path);
        } else {
            diagnosticsByFile_.insert(path, diagnostics);
        }
        rebuildProblems();
    });

    // Forward manual / trigger-char completion requests from the editor to
    // the configured backend (clangd/ccls over LSP, or libclang in-process).
    // The requestId is tracked in pendingCompletions_ so the async response
    // can be routed back to the right popup.
    QPointer<CodeEditor> edPtr(editor);
    connect(editor, &CodeEditor::completionRequested,
            this, [this, edPtr](int line, int column,
                                 const QString& triggerChar) {
        requestCompletionFor(edPtr, line, column, triggerChar);
    });

    // Ctrl+Click / F12 in the editor becomes a textDocument/definition
    // request. Same contract as completion: push the current text first
    // (lint didChange is debounced, so clangd would otherwise resolve the
    // position against a stale buffer), then remember the id so the async
    // answer can be routed back.
    connect(editor, &CodeEditor::definitionRequested,
            this, [this, edPtr](int line, int column) {
        CodeEditor* ed = edPtr;
        if (ed == nullptr || !lsp_->isRunning()) {
            return;
        }
        const QString lintPath = ed->lintPath();
        if (lintPath.isEmpty() || !isCppLikeFile(lintPath)) {
            return;
        }
        // Navigation answers lean on clangd's index: languageService.clangd.
        // indexing switches it off, and files above the large-file limits
        // (parse or index) are skipped as well.
        if (!clangdNavigationEnabled_
            || fileExceeds(lintPath, largeFileParseMB_)
            || fileExceeds(lintPath, largeFileIndexMB_)) {
            return;
        }
        lsp_->changeDocument(lintPath, ed->toPlainText());
        const int reqId = lsp_->requestDefinition(lintPath, line, column);
        if (reqId > 0) {
            latestDefinitionId_ = reqId;
            pendingDefinitions_[reqId] = edPtr;
        }
    });

    // `(` / `,` inside a call asks for signature help. Same push-text-first
    // contract; the answer is shown as a tooltip anchored at the caret of
    // the editor that asked, guarded by the newest-request id.
    connect(editor, &CodeEditor::signatureHelpRequested,
            this, [this, edPtr](int line, int column) {
        CodeEditor* ed = edPtr;
        if (ed == nullptr || !lsp_->isRunning()) {
            return;
        }
        const QString lintPath = ed->lintPath();
        if (lintPath.isEmpty() || !isCppLikeFile(lintPath)) {
            return;
        }
        lsp_->changeDocument(lintPath, ed->toPlainText());
        const int reqId = lsp_->requestSignatureHelp(lintPath, line, column);
        if (reqId > 0) {
            latestSignatureHelpId_ = reqId;
        }
    });

    tabWidget_->addTab(editor, title);
    tabWidget_->setCurrentWidget(editor);
    return editor;
}

CodeEditor* MainWindow::openFileInTab(const QString& path)
{
    // Check if already open.
    for (int i = 0; i < tabWidget_->count(); ++i) {
        if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
            if (ed->filePath() == path) {
                tabWidget_->setCurrentIndex(i);
                // Navigating back to an already-open file still counts as
                // using it: move it to the top of the recent list.
                noteRecentFile(path);
                return ed;
            }
        }
    }

    // Use LightCppText::readDetectedTextFile to detect encoding and convert.
    using namespace LightCpp::Text;
    const TextFileResult result = readDetectedTextFile(path);
    if (!result.hasValue()) {
        outputPanel_->appendBuildOutput(
            tr("Error opening %1: %2")
                .arg(path, toString(result.error())));
        return nullptr;
    }

    const TextFileContent& content = result.value();
    // Large files: syntax highlighting is the first thing to go — the
    // highlighter re-parses the whole document on every keystroke.
    const bool tooBigToHighlight = fileExceeds(path, largeFileHighlightMB_);
    auto* editor = createEditorTab(QFileInfo(path).fileName());
    if (tooBigToHighlight) {
        editor->setSyntaxHighlightEnabled(false);
        noteLargeFile(path, tr("syntax highlighting"));
    }
    editor->setFilePath(path);
    editor->loadUtf8(content.utf8);

    // A file the user cannot write opens read-only: editing it would only
    // produce a save error later, so the editor says so up front.
    const QFileInfo openedInfo(path);
    if (openedInfo.exists() && !openedInfo.isWritable()) {
        editor->setReadOnly(true);
        outputPanel_->appendBuildOutput(tr("Read-only: %1").arg(path));
    }

    setEncodingInfo(editor, toName(content.encoding));
    // Remember how the file on disk ends its lines, so saving keeps that
    // style instead of rewriting every line to the global default (an LF
    // file edited by one line used to come back all-CRLF).
    editor->setProperty("detectedLineEnding",
                        QString::fromLatin1(LightCpp::Text::toString(content.lineEndings)));
    statusFile_->setText(path);
    updateEolCombo(editor);
    statusLanguage_->setText(languageFromSuffix(path));
    outputPanel_->appendBuildOutput(
        tr("Opened: %1  [%2, %3 bytes%4]")
            .arg(path)
            .arg(toName(content.encoding))
            .arg(content.byteSize)
            .arg(content.hadBom ? QStringLiteral(", BOM") : QString()));

    // A file that really opened goes to the top of the recent list the
    // start-up page shows.
    noteRecentFile(path);

    // Push the freshly opened document to the language server — unless the
    // large-file parse limit says even that is too much.
    if (lsp_->isRunning() && isCppLikeFile(path)) {
        if (fileExceeds(path, largeFileParseMB_)) {
            noteLargeFile(path, tr("parsing"));
        } else {
            lsp_->openDocument(path, languageIdForFile(path),
                               QString::fromUtf8(content.utf8));
        }
    }

    watchEditorFile(editor, true);

    return editor;
}

void MainWindow::reloadWithEncoding(LightCpp::Text::TextEncoding encoding)
{
    CodeEditor* ed = currentEditor();
    if (!ed) return;
    if (ed->filePath().isEmpty()) {
        // Untitled document — just relabel and let the user keep typing.
        ed->setEncodingLabel(QString::fromLatin1(
            LightCpp::Text::toName(encoding)));
        return;
    }

    using namespace LightCpp::Text;

    // Re-reading reads the bytes again: the buffer is replaced and the undo
    // history goes with it. This used to happen without a word, so a stray
    // touch on the status-bar drop-down sent the editor back to the on-disk
    // version and the user only found out later. Ask first — and put the
    // combo back when they decline, or the status bar would claim an
    // encoding the buffer is not using.
    if (ed->isModified()) {
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this, tr("Reload with a Different Encoding"),
            tr("<p><b>%1</b> has unsaved changes.</p>"
               "<p>Re-reading the file as %2 replaces the buffer and clears "
               "the undo history, so those changes will be lost.</p>"
               "<p>Reload and discard them?</p>")
                .arg(QFileInfo(ed->filePath()).fileName(),
                     QString::fromLatin1(toName(encoding))),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            updateEncodingCombo(ed);
            return;
        }
    }

    const TextFileResult result =
        readTextFileWithEncoding(ed->filePath(), encoding);
    if (!result.hasValue()) {
        outputPanel_->appendBuildOutput(
            tr("Reload failed for %1: %2")
                .arg(ed->filePath(), toString(result.error())));
        // Restore the combo to the editor's previous label so the UI stays
        // consistent.
        updateEncodingCombo(ed);
        return;
    }

    const TextFileContent& content = result.value();
    ed->loadUtf8(content.utf8);
    setEncodingInfo(ed, toName(encoding));
    ed->setProperty("detectedLineEnding",
                    QString::fromLatin1(LightCpp::Text::toString(content.lineEndings)));
    outputPanel_->appendBuildOutput(
        tr("Reloaded: %1  [%2]")
            .arg(ed->filePath())
            .arg(toName(encoding)));
    updateEolCombo(ed);
}

void MainWindow::updateEolCombo(CodeEditor* ed)
{
    if (eolCombo_ == nullptr) {
        return;
    }
    if (ed == nullptr) {
        eolCombo_->setEnabled(false);
        return;
    }
    eolCombo_->setEnabled(true);

    // Per-tab override wins (the user picked one for this tab); otherwise
    // the style the file was loaded with — the buffer itself always reads
    // as LF because QTextDocument normalizes, so the disk detection is the
    // only honest source — falling back to the global save default for
    // buffers that were never loaded from disk.
    QString eol = ed->property("lineEndingOverride")
                      .toString();
    if (eol.isEmpty()) {
        eol = ed->property("detectedLineEnding").toString();
    }
    if (eol.isEmpty()) {
        const QString text = ed->toPlainText();
        if (text.contains(QStringLiteral("\r\n"))) {
            eol = QStringLiteral("CRLF");
        } else if (text.contains(u'\n')) {
            eol = QStringLiteral("LF");
        } else {
            eol = lineEndingForSaving_;
        }
    }
    const QSignalBlocker blocker(eolCombo_);
    eolCombo_->setCurrentText(eol);
}

void MainWindow::onEolChanged(int index)
{
    if (index < 0) {
        return;
    }
    CodeEditor* ed = currentEditor();
    if (ed == nullptr) {
        return;
    }
    ed->setProperty("lineEndingOverride",
                    eolCombo_->currentText());
}

void MainWindow::watchEditorFile(CodeEditor* ed, bool watch)
{
    if (ed == nullptr || ed->filePath().isEmpty()) {
        return;
    }
    const QString absolute = QFileInfo(ed->filePath()).absoluteFilePath();
    if (watch) {
        if (!fileWatcher_->files().contains(absolute)) {
            fileWatcher_->addPath(absolute);
        }
    } else {
        fileWatcher_->removePath(absolute);
    }
}

void MainWindow::reloadEditorFromDisk(CodeEditor* ed)
{
    using namespace LightCpp::Text;
    const TextFileResult result = readDetectedTextFile(ed->filePath());
    if (!result.hasValue()) {
        outputPanel_->appendBuildOutput(
            tr("Reload failed for %1: %2")
                .arg(ed->filePath(), toString(result.error())));
        return;
    }
    const TextFileContent& content = result.value();
    // loadUtf8 replaces the text but the fold rebuild re-attaches the
    // collapsed regions by index + header text, so folds survive a reload.
    ed->loadUtf8(content.utf8);
    setEncodingInfo(ed, toName(content.encoding));
    ed->setProperty("detectedLineEnding",
                    QString::fromLatin1(LightCpp::Text::toString(content.lineEndings)));
    // The file may have become writable (or read-only) outside the IDE.
    ed->setReadOnly(!QFileInfo(ed->filePath()).isWritable());
    if (lsp_->isRunning() && isCppLikeFile(ed->lintPath())
        && !fileExceeds(ed->filePath(), largeFileParseMB_)) {
        // changeDocument(): the buffer changed under the server's feet, so
        // didChange is the right notification; it opens the document if the
        // server has not seen it yet, instead of a second didOpen for one it
        // already has.
        lsp_->changeDocument(ed->lintPath(), QString::fromUtf8(content.utf8));
    }
    outputPanel_->appendBuildOutput(
        tr("Reloaded (changed on disk): %1").arg(ed->filePath()));
    updateEolCombo(ed);
}

void MainWindow::onWatchedFileChanged(const QString& path)
{
    CodeEditor* ed = nullptr;
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto* candidate = qobject_cast<CodeEditor*>(tabWidget_->widget(i));
        if (candidate != nullptr
            && QFileInfo(candidate->filePath()).absoluteFilePath()
                   == QFileInfo(path).absoluteFilePath()) {
            ed = candidate;
            break;
        }
    }
    if (ed == nullptr) {
        // Nobody has this file open; stop watching it.
        fileWatcher_->removePath(path);
        return;
    }

    // A write from this process fires the same signal. Within the window it
    // is ours: skip, and make sure the path is still watched.
    if (const qint64 writtenAt = selfWriteMs_.value(path, 0);
        writtenAt != 0
        && QDateTime::currentMSecsSinceEpoch() - writtenAt < 750) {
        return;
    }
    selfWriteMs_.remove(path);

    if (!QFileInfo::exists(path)) {
        outputPanel_->appendBuildOutput(
            tr("File deleted on disk: %1").arg(path));
        return;
    }

    if (ed->isModified()) {
        // Do not touch the buffer — the user has unsaved work. Just say so.
        outputPanel_->appendBuildOutput(
            tr("File changed on disk with unsaved changes in the editor: %1")
                .arg(path));
    } else {
        reloadEditorFromDisk(ed);
    }
    // A deleted-then-recreated file is dropped from the watch list.
    if (QFileInfo::exists(path) && !fileWatcher_->files().contains(path)) {
        fileWatcher_->addPath(path);
    }
}

void MainWindow::updateEncodingCombo(CodeEditor* editor)
{
    if (!editor) return;
    const QString enc = editor->encodingLabel();
    // Match by canonical encoding name (slot 1 may show "ANSI (GBK)" but
    // the editor stores the canonical name, e.g. "GBK"; we resolve via
    // fromName so both forms select the same slot).
    const auto parsed = LightCpp::Text::fromName(enc.toUtf8());
    QSignalBlocker blocker(encodingCombo_);
    if (parsed.has_value()) {
        for (int i = 0; i < encodingCombo_->count(); ++i) {
            const auto e = encodingCombo_->itemData(i)
                                .value<LightCpp::Text::TextEncoding>();
            if (e == *parsed) {
                encodingCombo_->setCurrentIndex(i);
                return;
            }
        }
    }
    // Fallback: match by display text.
    for (int i = 0; i < encodingCombo_->count(); ++i) {
        if (encodingCombo_->itemText(i) == enc) {
            encodingCombo_->setCurrentIndex(i);
            return;
        }
    }
    encodingCombo_->setCurrentIndex(0);  // default to UTF-8
}

void MainWindow::setEncodingInfo(CodeEditor* editor, const QString& encodingName)
{
    editor->setEncodingLabel(encodingName);
    updateEncodingCombo(editor);
}
// ---------------------------------------------------------------------------
// Syntax check (clangd over LSP)
// ---------------------------------------------------------------------------
void MainWindow::onSyntaxCheckToggled(bool enabled)
{
    if (enabled) {
        lsp_->start();
        appendLanguageServerLog(
            tr("Starting language server: %1").arg(lsp_->serverPath()));
    } else {
        lsp_->stop();
        appendLanguageServerLog(tr("Language server stopped"));
        // Clear all diagnostic markers and any pending completion popups.
        pendingCompletions_.clear();
        pendingDefinitions_.clear();
        // In-flight declaration/implementation/references answers are dead
        // with the server: drop them so a stale response is never applied.
        navigationKinds_.clear();
        for (int i = 0; i < tabWidget_->count(); ++i) {
            if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
                ed->setDiagnostics({});
                ed->dismissPopup();
            }
        }
        // The server is gone, so its diagnostics are no longer meaningful.
        diagnosticsByFile_.clear();
        rebuildProblems();
    }
}

// ---------------------------------------------------------------------------
// Problems list
// ---------------------------------------------------------------------------
void MainWindow::rebuildProblems()
{
    QList<ProblemEntry> problems;
    for (auto it = diagnosticsByFile_.constBegin();
         it != diagnosticsByFile_.constEnd(); ++it) {
        for (const LightCpp::App::LspDiagnostic& d : it.value()) {
            ProblemEntry entry;
            entry.filePath = it.key();
            entry.line = d.startLine;
            entry.column = d.startColumn;
            entry.severity = d.severity;
            entry.message = d.message;
            entry.code = d.code;
            problems.append(entry);
        }
    }
    // Stable order: by file, then by position. QHash iteration is arbitrary,
    // so sorting is what keeps the list from jumping around between updates.
    std::sort(problems.begin(), problems.end(),
              [](const ProblemEntry& a, const ProblemEntry& b) {
        if (a.filePath != b.filePath) {
            return a.filePath.compare(b.filePath, Qt::CaseInsensitive) < 0;
        }
        if (a.line != b.line) return a.line < b.line;
        return a.column < b.column;
    });
    outputPanel_->setProblems(problems);
}

// ---------------------------------------------------------------------------
// Compiler problems (parsed from the compiler / build-system log)
// ---------------------------------------------------------------------------
void MainWindow::clearCompilerProblems()
{
    compilerProblems_.clear();
    outputPanel_->setCompilerProblems({});
    // Strip the compiler markers off the editors; the LSP squiggles come
    // straight back from diagnosticsByFile_ (that map stays LSP-only).
    applyCompilerDiagnosticsToEditors();
}

void MainWindow::consumeCompilerLines(const QString& text, QString& tail)
{
    if (text.isEmpty()) {
        return;
    }
    const QStringList lines = (tail + text).split(QLatin1Char('\n'));
    // The last element is the unfinished remainder ("" after a trailing
    // newline) — kept for the next chunk.
    for (int i = 0; i < lines.size() - 1; ++i) {
        processCompilerLogLine(lines.at(i));
    }
    tail = lines.last();
}

void MainWindow::processCompilerLogLine(const QString& line)
{
    QString text = line;
    while (text.endsWith(QLatin1Char('\r'))) {
        text.chop(1);
    }
    if (text.isEmpty()) {
        return;
    }
    const std::optional<LightCpp::Project::CompilerDiagnostic> parsed =
        LightCpp::Project::parseCompilerDiagnostic(text.toStdString());
    if (!parsed.has_value()) {
        return;
    }

    ProblemEntry entry;
    entry.severity = parsed->severity == 2 ? 2 : 1;
    entry.message = QString::fromStdString(parsed->message);
    entry.line = parsed->line;
    entry.column = parsed->column;
    if (!parsed->file.empty()) {
        // The compiler prints absolute paths for single-file compiles, but
        // build systems often print paths relative to the step's working
        // directory — resolve those so entries (and editor markers) always
        // point at a real file.
        entry.filePath = QDir(currentBuildWorkingDir_).absoluteFilePath(
            QString::fromStdString(parsed->file));
    }
    // Entries without a location (driver errors) keep an empty filePath and
    // are listed under the "Build" group without navigation.

    compilerProblems_.append(entry);
    outputPanel_->setCompilerProblems(compilerProblems_);
    // Live markers: the squiggle appears while the compiler is still running.
    applyCompilerDiagnosticsToEditors();
}

void MainWindow::applyCompilerDiagnosticsToEditors()
{
    applyingCompilerDiagnostics_ = true;
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i));
        if (ed == nullptr) {
            continue;
        }
        const QString editorPath =
            QFileInfo(ed->lintPath()).absoluteFilePath();

        // Base layer: the LSP diagnostics for this editor, from the
        // LSP-only store (matched by absolute path, as everywhere else).
        QList<LightCpp::App::LspDiagnostic> merged;
        for (auto it = diagnosticsByFile_.constBegin();
             it != diagnosticsByFile_.constEnd(); ++it) {
            if (QFileInfo(it.key()).absoluteFilePath() == editorPath) {
                merged = it.value();
                break;
            }
        }
        QSet<int> lspLines;
        for (const LightCpp::App::LspDiagnostic& d : merged) {
            lspLines.insert(d.startLine);
        }
        // Compiler layer: added only where the LSP has nothing to say on
        // the same line — the LSP's view wins when both report a position.
        for (const ProblemEntry& p : compilerProblems_) {
            if (p.filePath.isEmpty()) {
                continue;  // driver errors have no editor position
            }
            if (QFileInfo(p.filePath).absoluteFilePath() != editorPath) {
                continue;
            }
            if (lspLines.contains(p.line)) {
                continue;
            }
            LightCpp::App::LspDiagnostic d;
            d.startLine = p.line;
            d.startColumn = p.column;
            d.endLine = p.line;
            d.endColumn = p.column;
            d.severity = p.severity;
            d.message = p.message;
            d.code = QStringLiteral("compiler");
            merged.append(d);
        }
        // The echo this triggers is suppressed by the guard flag so the
        // merged list never leaks back into diagnosticsByFile_.
        ed->setDiagnostics(merged);
    }
    applyingCompilerDiagnostics_ = false;
}

void MainWindow::onProblemActivated(const QString& filePath, int line, int column)
{
    jumpToLocation(filePath, line, column);
}

void MainWindow::jumpToLocation(const QString& filePath, int line, int column)
{
    // A diagnostic or a definition may belong to an unsaved buffer, whose
    // "path" is a virtual temp location rather than a real file — look for an
    // open tab first and only fall back to loading from disk.
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i));
        if (ed == nullptr) {
            continue;
        }
        if (QFileInfo(ed->lintPath()).absoluteFilePath()
                == QFileInfo(filePath).absoluteFilePath()) {
            tabWidget_->setCurrentIndex(i);
            ed->gotoLocation(line, column);
            return;
        }
    }
    if (CodeEditor* ed = openFileInTab(filePath)) {
        ed->gotoLocation(line, column);
    } else {
        outputPanel_->appendBuildOutput(
            tr("Cannot jump to %1: the file could not be opened.").arg(filePath));
    }
}

void MainWindow::onFormatDocument()
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr) {
        outputPanel_->showChannel(OutputPanel::ChannelBuild);
        outputPanel_->appendBuildOutput(tr("No active file to format."));
        return;
    }
    if (formatter_ == nullptr) {
        formatter_ = new LightCpp::App::CodeFormatter(this);
        connect(formatter_, &LightCpp::App::CodeFormatter::formatted, this,
                &MainWindow::onFormatted);
        connect(formatter_, &LightCpp::App::CodeFormatter::failed, this,
                &MainWindow::onFormatFailed);
    }
    if (!LightCpp::App::CodeFormatter::isAvailable()) {
        const QString message = tr("clang-format was not found next to the "
                                   "compilers (%1) or on PATH.")
                                    .arg(LightCpp::App::kMingwBin);
        outputPanel_->showChannel(OutputPanel::ChannelBuild);
        outputPanel_->appendBuildOutput(message);
        statusBar()->showMessage(message, 6000);
        return;
    }

    // The buffer's own file names the style search; an untitled buffer has
    // none, and clang-format then falls back to the default style, which is
    // what it would do anyway for a file that does not exist yet.
    latestFormatRequest_ =
        formatter_->format(ed->toPlainText(), ed->filePath());
    formatTarget_ = ed;
    statusBar()->showMessage(tr("Formatting %1...").arg(tabTitleFor(ed)), 2000);
}

void MainWindow::onFormatted(int requestId, const QString& text)
{
    // Only the newest request counts: an earlier run's answer describes text
    // the user has since changed, and applying it would throw that away.
    if (requestId != latestFormatRequest_ || formatTarget_.isNull()) {
        return;
    }
    CodeEditor* ed = formatTarget_;
    // A format that changes nothing must not dirty the buffer or the undo
    // stack, so report it rather than pretending work happened.
    const bool changed = (text != ed->toPlainText());
    ed->applyFormattedText(text);
    if (changed) {
        // The tab's modification dot is maintained by the editor's textChanged
        // handler, so there is nothing to set here beyond the caret, which
        // applyFormattedText() has already restored.
        statusBar()->showMessage(tr("Formatted %1").arg(tabTitleFor(ed)), 4000);
    } else {
        statusBar()->showMessage(tr("%1 was already formatted").arg(tabTitleFor(ed)),
                                 4000);
    }
}

void MainWindow::onFormatFailed(int requestId, const QString& message)
{
    if (requestId != latestFormatRequest_) {
        return;
    }
    outputPanel_->showChannel(OutputPanel::ChannelBuild);
    outputPanel_->appendBuildOutput(tr("Formatting failed: %1").arg(message));
    statusBar()->showMessage(tr("Formatting failed — see the Build Log"), 6000);
}

// ---------------------------------------------------------------------------
// Batch formatting (clang-format -i over a folder / the project)
// ---------------------------------------------------------------------------
void MainWindow::onFormatFolder()
{
    const QString dir =
        QFileDialog::getExistingDirectory(this, tr("Format Folder"));
    if (!dir.isEmpty()) {
        formatFolder(dir);
    }
}

void MainWindow::onFormatProject()
{
    formatFolder(projectRootDirectory());
}

void MainWindow::formatFolder(const QString& dir)
{
    outputPanel_->showChannel(OutputPanel::ChannelBuild);
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        outputPanel_->appendBuildOutput(
            tr("Format folder: %1 is not a directory.").arg(dir));
        return;
    }
    if (!LightCpp::App::CodeFormatter::isAvailable()) {
        const QString message = tr("clang-format was not found next to the "
                                   "compilers (%1) or on PATH.")
                                    .arg(LightCpp::App::kMingwBin);
        outputPanel_->appendBuildOutput(message);
        statusBar()->showMessage(message, 6000);
        return;
    }

    const QStringList files = collectFormatFiles(dir);
    if (files.isEmpty()) {
        outputPanel_->appendBuildOutput(
            tr("Format folder: no C/C++ files under %1.").arg(dir));
        return;
    }

    // Buffers with unsaved edits are skipped, not rewritten: clang-format
    // would change the disk copy out from under them, and a later save
    // would throw that formatting away anyway. The user's edits win.
    const QSet<QString> dirty = openModifiedFilePaths();
    QStringList runnable;
    runnable.reserve(files.size());
    for (const QString& file : files) {
        if (!dirty.contains(QFileInfo(file).absoluteFilePath())) {
            runnable.append(file);
        }
    }
    const int skipped = files.size() - runnable.size();

    // At/above the threshold a batch is a large diff — ask before rewriting
    // a whole tree the user may only have meant to look at.
    if (runnable.size() >= LightCpp::App::BatchFormatter::kWarnThreshold) {
        QString text =
            tr("About to reformat %1 files under\n%2\n\n"
               "This rewrites the files in place. Continue?")
                .arg(runnable.size())
                .arg(QDir::toNativeSeparators(dir));
        if (skipped > 0) {
            text += tr("\n\n%1 open file(s) with unsaved changes will be "
                       "skipped.")
                        .arg(skipped);
        }
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("Format Many Files"), text,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            outputPanel_->appendBuildOutput(
                tr("Batch format cancelled (%1 files).").arg(runnable.size()));
            return;
        }
    }

    if (batchFormatter_ == nullptr) {
        batchFormatter_ = new LightCpp::App::BatchFormatter(this);
        connect(batchFormatter_, &LightCpp::App::BatchFormatter::progress,
                this, &MainWindow::onBatchProgress);
        connect(batchFormatter_, &LightCpp::App::BatchFormatter::finished,
                this, &MainWindow::onBatchFinished);
    }

    batchFormattedPaths_.clear();
    if (skipped > 0) {
        outputPanel_->appendBuildOutput(
            tr("Skipping %1 open file(s) with unsaved changes.").arg(skipped));
    }
    outputPanel_->appendBuildOutput(
        tr("--- Formatting %1 files under %2 ---")
            .arg(runnable.size())
            .arg(QDir::toNativeSeparators(dir)));
    latestBatchRequestId_ = batchFormatter_->formatFiles(runnable, dir);
    statusBar()->showMessage(tr("Formatting %1/%2...")
                                 .arg(0)
                                 .arg(runnable.size()));
}

void MainWindow::onBatchProgress(int done, int total, const QString& file,
                                 bool success)
{
    // Successes are collected so the matching open tabs can be reloaded at
    // the end; failures are already listed in the finished() summary, so no
    // per-file log line here.
    if (success) {
        batchFormattedPaths_.append(file);
    }
    statusBar()->showMessage(tr("Formatting %1/%2: %3")
                                 .arg(done)
                                 .arg(total)
                                 .arg(QFileInfo(file).fileName()));
}

void MainWindow::onBatchFinished(int requestId, int formatted, int failed,
                                 const QStringList& failures)
{
    if (requestId != latestBatchRequestId_) {
        return;  // a superseded batch reporting late
    }
    latestBatchRequestId_ = 0;
    outputPanel_->showChannel(OutputPanel::ChannelBuild);
    outputPanel_->appendBuildOutput(
        tr("--- Batch format finished: %1 formatted, %2 failed ---")
            .arg(formatted)
            .arg(failed));
    for (const QString& failure : failures) {
        outputPanel_->appendBuildOutput(failure);
    }
    reloadFormattedOpenTabs(batchFormattedPaths_);
    batchFormattedPaths_.clear();
    if (failed == 0) {
        statusBar()->showMessage(
            tr("Batch format finished: %1 file(s)").arg(formatted), 6000);
    } else {
        statusBar()->showMessage(
            tr("Batch format finished with %1 failure(s) — see the Build Log")
                .arg(failed),
            6000);
    }
}

QStringList MainWindow::collectFormatFiles(const QString& dir)
{
    const QString root = QDir(dir).absolutePath();
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        // Build output and vendored dependencies are not ours to reformat:
        // skipping them keeps the batch fast and the diff reviewable. Only
        // directory components are excluded — the chosen root itself is
        // never excluded, so an explicit pick wins.
        const QString relative = QDir(root).relativeFilePath(path);
        QStringList parts =
            relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            parts.removeLast();  // drop the file name
        }
        bool excluded = false;
        for (const QString& part : parts) {
            if (part.startsWith(QLatin1Char('.'))
                || part == QLatin1String("build")
                || part == QLatin1String("CMakeFiles")
                || part == QLatin1String("third_party")
                || part == QLatin1String("node_modules")) {
                excluded = true;
                break;
            }
        }
        if (excluded || !isCppLikeFile(path)) {
            continue;
        }
        files.append(QFileInfo(path).absoluteFilePath());
    }
    // Sorted so the Build Log reads the same on every run.
    std::sort(files.begin(), files.end());
    return files;
}

void MainWindow::reloadFormattedOpenTabs(const QStringList& paths)
{
    if (paths.isEmpty()) {
        return;
    }
    QSet<QString> rewritten;
    rewritten.reserve(paths.size());
    for (const QString& path : paths) {
        rewritten.insert(QFileInfo(path).absoluteFilePath());
    }
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i));
        // Modified buffers keep the user's edits — the batch skipped their
        // files, so there is nothing on disk to pick up either.
        if (ed == nullptr || ed->isModified()) {
            continue;
        }
        const QString path = ed->filePath();
        if (path.isEmpty()
            || !rewritten.contains(QFileInfo(path).absoluteFilePath())) {
            continue;
        }
        // Re-read with the tab's own encoding: the batch only reflowed the
        // bytes clang-format accepted, so the encoding the file was opened
        // with is still the right one.
        using namespace LightCpp::Text;
        const auto parsed = fromName(ed->encodingLabel().toUtf8());
        if (!parsed.has_value()) {
            continue;
        }
        const TextFileResult result = readTextFileWithEncoding(path, *parsed);
        if (!result.hasValue()) {
            outputPanel_->appendBuildOutput(
                tr("Reload after format failed for %1: %2")
                    .arg(path, toString(result.error())));
            continue;
        }
        // loadUtf8 clears the modified flag; the textChanged handler then
        // re-pushes the document to clangd through the lint debounce.
        ed->loadUtf8(result.value().utf8);
        ed->setProperty(
            "detectedLineEnding",
            QString::fromLatin1(
                LightCpp::Text::toString(result.value().lineEndings)));
    }
}

QSet<QString> MainWindow::openModifiedFilePaths() const
{
    QSet<QString> result;
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i));
        if (ed == nullptr || !ed->isModified()) {
            continue;
        }
        // Untitled buffers have no path — they were never going to be in a
        // folder listing anyway.
        const QString path = ed->filePath();
        if (!path.isEmpty()) {
            result.insert(QFileInfo(path).absoluteFilePath());
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Project (CMake / Dev-C++)
// ---------------------------------------------------------------------------
void MainWindow::onOpenFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Folder"));
    if (!dir.isEmpty()) {
        openFolder(dir);
    }
}

void MainWindow::openFolder(const QString& dir)
{
    if (!LightCpp::Project::isDirectory(dir)) {
        outputPanel_->showChannel(OutputPanel::ChannelBuild);
        outputPanel_->appendBuildOutput(
            tr("Cannot open folder: %1 is not a directory.").arg(dir));
        return;
    }

    // A folder the user picked by hand is a root in its own right, so the
    // Explorer is pointed at it *before* anything is decided about projects.
    // If it turns out to hold one, openProjectFile() moves the root down to
    // where the project actually starts.
    fileExplorer_->setRootPath(dir);
    outputPanel_->showChannel(OutputPanel::ChannelBuild);
    outputPanel_->appendBuildOutput(tr("Opened folder: %1").arg(dir));

    const auto descriptor = LightCpp::Project::projectFileIn(dir);
    if (!descriptor.has_value()) {
        // Moving to a folder that is not a project leaves the previous project
        // behind: Build would otherwise keep building something the user has
        // visibly navigated away from.
        if (projectKind_ != ProjectKind::None) {
            const QString previous = projectRootDirectory();
            clearProjectState();
            outputPanel_->appendBuildOutput(
                tr("Project closed: %1 was left behind.").arg(previous));
        }
        outputPanel_->appendBuildOutput(
            tr("No project descriptor here — showing the folder on its own."));
        // The status-bar git slot follows the folder just opened: leaving a
        // repository behind (or entering one) must show up immediately.
        syncGitWorkingDirectory();
        // Leaving the previous project also drops its config/ overlay; the
        // base settings apply again from here on.
        syncSettingsOverlays();
        return;
    }

    // Passing showInExplorer: the descriptor's directory is the project root,
    // which is the folder worth showing — and for a .dev it is the same
    // directory, so a project always ends up rooted at its own folder.
    openProjectFile(descriptor->filePath, true);
}

void MainWindow::onOpenProject()
{
    // One dialog for every format. The combined first entry is assembled
    // from the backends' own filters, so a new project plugin shows up here
    // without this code learning its extension — asking the user to say
    // which format they have would be asking them to make a distinction the
    // file already makes.
    QStringList pluginFilters;
    if (pluginManager_ != nullptr) {
        pluginFilters = pluginManager_->projectFilters();
    }
    QStringList allPatterns{QStringLiteral("CMakeLists.txt")};
    for (const QString& filter : pluginFilters) {
        // "Dev-C++ (*.dev)" → "*.dev", folded into the combined line.
        const qsizetype open = filter.indexOf(QLatin1Char('('));
        const qsizetype close = filter.lastIndexOf(QLatin1Char(')'));
        if (open >= 0 && close > open) {
            allPatterns.append(filter.mid(open + 1, close - open - 1)
                                   .split(QLatin1Char(' '), Qt::SkipEmptyParts));
        }
    }
    QString filter = tr("Projects (%1)").arg(allPatterns.join(QLatin1Char(' ')));
    filter += QStringLiteral(";;CMake (CMakeLists.txt)");
    if (!pluginFilters.isEmpty()) {
        filter += QStringLiteral(";;") + pluginFilters.join(QStringLiteral(";;"));
    }
    filter += QStringLiteral(";;") + tr("All files (*)");
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Open Project"), QString(), filter);
    if (!path.isEmpty()) {
        openProjectFile(path, true);
    }
}

void MainWindow::onCloseProject()
{
    if (projectKind_ == ProjectKind::None) {
        return;
    }
    outputPanel_->showChannel(OutputPanel::ChannelBuild);
    clearProjectState();
    // No project, no overlay: the project's config/ stops applying here.
    syncSettingsOverlays();
    outputPanel_->appendBuildOutput(tr("Project closed."));
}

void MainWindow::clearProjectState()
{
    // Remember where the project was before the state is torn down: the
    // Explorer is still rooted there, and the user's folder should survive
    // closing the project they opened inside it.
    const QString root = projectRootDirectory();
    projectKind_ = ProjectKind::None;
    cmakeProject_.reset();
    hasDevProject_ = false;
    devSummary_ = {};
    projectPath_.clear();
    lastProjectExe_.clear();
    setWindowTitle(QStringLiteral("LightC++"));
    updateProjectActions();
    syncGitWorkingDirectory();
    if (!root.isEmpty() && fileExplorer_ != nullptr
        && fileExplorer_->rootPath() == root) {
        // Nothing was opened *around* the project, so there is no outer folder
        // to fall back to — the Explorer keeps showing where the user was
        // rather than emptying itself out.
        outputPanel_->appendBuildOutput(
            tr("The Explorer still shows %1.").arg(root));
    }
}

void MainWindow::onConfigureProject()
{
    if (projectKind_ != ProjectKind::CMake || !cmakeProject_.has_value()) {
        outputPanel_->showChannel(OutputPanel::ChannelBuild);
        outputPanel_->appendBuildOutput(
            tr("Only a CMake project needs configuring — a .dev project is "
               "configured by the file itself."));
        return;
    }
    const LightCpp::Project::BuildConfiguration config = buildConfiguration();
    const LightCpp::Project::CMakeProject& project = *cmakeProject_;
    projectSteps_ = {ProjectStep{
        QStringLiteral("cmake"),
        LightCpp::Project::configureArguments(project, config),
        LightCpp::Project::buildDirectory(project, config),
        tr("Configuring %1 [%2]").arg(project.sourceDir,
                                      LightCpp::Project::toString(config)),
    }};
    startProjectBuildSteps();
}

void MainWindow::onBuildProject()
{
    projectSteps_ = projectBuildSteps();
    if (!projectSteps_.isEmpty()) {
        startProjectBuildSteps();
    }
}

void MainWindow::onRunProject()
{
    if (projectKind_ == ProjectKind::None) {
        outputPanel_->showChannel(OutputPanel::ChannelBuild);
        outputPanel_->appendBuildOutput(tr("No project is open."));
        return;
    }
    // Ask the build tree rather than trusting a remembered path: the user may
    // have built outside the IDE, or switched configuration in between.
    if (projectKind_ == ProjectKind::CMake && cmakeProject_.has_value()) {
        const QString tree = LightCpp::Project::buildDirectory(*cmakeProject_,
                                                               buildConfiguration());
        if (const QString preferred =
                LightCpp::Project::preferredExecutable(tree);
            !preferred.isEmpty()) {
            lastProjectExe_ = preferred;
        }
    }
    if (lastProjectExe_.isEmpty() || !QFileInfo::exists(lastProjectExe_)) {
        outputPanel_->showChannel(OutputPanel::ChannelBuild);
        outputPanel_->appendBuildOutput(
            tr("Nothing to run yet — build the project first (%1).")
                .arg(actBuildProject_->shortcut().toString()));
        return;
    }
    startProgram(lastProjectExe_, QFileInfo(lastProjectExe_).absolutePath());
}

void MainWindow::openProjectFile(const QString& path, bool showInExplorer)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        outputPanel_->showChannel(OutputPanel::ChannelBuild);
        outputPanel_->appendBuildOutput(tr("Cannot open project: %1 does not exist.").arg(path));
        return;
    }

    outputPanel_->showChannel(OutputPanel::ChannelBuild);

    if (info.fileName().compare(QStringLiteral("CMakeLists.txt"),
                                Qt::CaseInsensitive) == 0) {
        auto project = LightCpp::Project::CMakeProject{info.absolutePath()};
        cmakeProject_ = project;
        hasDevProject_ = false;
        devSummary_ = {};
        projectKind_ = ProjectKind::CMake;
        projectPath_ = info.absoluteFilePath();
        lastProjectExe_.clear();
        outputPanel_->appendBuildOutput(
            tr("CMake project opened: %1").arg(project.sourceDir));
        outputPanel_->appendBuildOutput(
            tr("Build trees: %1/<Debug|Release> (selected by the toolbar)")
                .arg(LightCpp::Project::buildDirectory(project, buildConfiguration())));
    } else {
        // Everything that is not a CMakeLists.txt belongs to a project
        // backend plugin. The first backend that claims the file owns it —
        // the IDE core knows no other concrete format itself.
        LightCpp::Plugin::ProjectBackend* backend =
            pluginManager_ == nullptr
                ? nullptr
                : pluginManager_->backendFor(info.absoluteFilePath());
        if (backend == nullptr) {
            outputPanel_->appendBuildOutput(
                tr("Unrecognised project file: %1\n"
                   "Open a CMakeLists.txt, or install a plugin that handles "
                   "this format.")
                    .arg(info.fileName()));
            return;
        }
        QString error;
        LightCpp::Plugin::ProjectSummary summary;
        if (!backend->openProject(info.absoluteFilePath(), &summary, &error)) {
            outputPanel_->appendBuildOutput(
                tr("Cannot read %1: %2").arg(path, error));
            return;
        }
        devSummary_ = summary;
        hasDevProject_ = true;
        cmakeProject_.reset();
        projectKind_ = ProjectKind::DevCpp;
        projectPath_ = info.absoluteFilePath();
        lastProjectExe_ = summary.outputPath;
        // The format that claimed the file names the banner; every backend
        // that fills formatName gets "X project opened", the rest stay
        // generic rather than claiming a format they are not.
        const QString formatName = summary.formatName.isEmpty()
                                       ? tr("Project")
                                       : summary.formatName;
        outputPanel_->appendBuildOutput(
            tr("%1 project opened: %2 [%3, %4 unit(s)]")
                .arg(formatName,
                     summary.name.isEmpty() ? info.fileName() : summary.name,
                     summary.targetLabel)
                .arg(summary.unitCount));
        outputPanel_->appendBuildOutput(
            tr("Output: %1").arg(lastProjectExe_));
    }

    // The Explorer follows the project rather than the descriptor: a CMake
    // project found by climbing lives at its source dir, and showing the file
    // that was named would hide the tree the user is about to work in.
    if (showInExplorer) {
        fileExplorer_->setRootPath(projectRootDirectory());
    }

    setWindowTitle(QStringLiteral("LightC++ — %1").arg(projectDisplayName()));
    // The descriptor goes on top of the recent list (the start-up page
    // routes it back through openFromRecent on the next launch).
    noteRecentFile(info.absoluteFilePath());
    updateProjectActions();
    syncGitWorkingDirectory();
    // The project's own config/ directory (if any) now overlays the
    // executable's settings.
    syncSettingsOverlays();
}

QString MainWindow::projectRootDirectory() const
{
    if (projectKind_ == ProjectKind::CMake && cmakeProject_.has_value()) {
        return cmakeProject_->sourceDir;
    }
    if (projectKind_ == ProjectKind::DevCpp && hasDevProject_) {
        return devSummary_.projectDir;
    }
    return projectPath_.isEmpty() ? QString() : QFileInfo(projectPath_).absolutePath();
}

QString MainWindow::projectDisplayName() const
{
    if (projectPath_.isEmpty()) {
        return {};
    }
    const QFileInfo info(projectPath_);
    if (projectKind_ == ProjectKind::CMake) {
        return info.absolutePath();
    }
    if (hasDevProject_ && !devSummary_.name.isEmpty()) {
        return devSummary_.name;
    }
    return info.fileName();
}

QList<MainWindow::ProjectStep> MainWindow::projectBuildSteps()
{
    outputPanel_->showChannel(OutputPanel::ChannelBuild);

    const LightCpp::Project::BuildConfiguration config = buildConfiguration();

    if (projectKind_ == ProjectKind::CMake && cmakeProject_.has_value()) {
        const LightCpp::Project::CMakeProject& project = *cmakeProject_;
        QList<ProjectStep> steps;
        // The active kit's cmake when it names one, else whatever is on
        // PATH. The kit's buildToolArgs.cmake are appended to the configure
        // step only — they configure the build tree; the build step's shape
        // stays the project's own.
        QString cmakeProgram = QStringLiteral("cmake");
        QStringList extraConfigureArgs;
        if (const auto kit = activeKit(); kit.has_value()) {
            const QString kitCmake =
                kit->buildTools.value(QStringLiteral("cmake"));
            if (!kitCmake.isEmpty() && QFileInfo::exists(kitCmake)) {
                cmakeProgram = kitCmake;
            }
            extraConfigureArgs =
                settings_
                    ->effectiveBuildConfig(*kit, currentBuildConfigurationName())
                    .buildToolArgs.value(QStringLiteral("cmake"));
        }
        // Configure only when this configuration's tree does not exist yet.
        // Re-running it would overwrite the cache the user may have edited.
        if (!LightCpp::Project::isConfigured(project, config)) {
            steps.append(ProjectStep{
                cmakeProgram,
                LightCpp::Project::configureArguments(project, config)
                    + extraConfigureArgs,
                LightCpp::Project::buildDirectory(project, config),
                tr("Configuring %1 [%2]").arg(project.sourceDir,
                                              LightCpp::Project::toString(config)),
            });
        }
        steps.append(ProjectStep{
            cmakeProgram,
            LightCpp::Project::buildArguments(project, config),
            project.sourceDir,
            tr("Building [%1]").arg(LightCpp::Project::toString(config)),
        });
        return steps;
    }

    if (projectKind_ == ProjectKind::DevCpp && hasDevProject_) {
        // The backend parsed the file when it was opened; it answers with
        // the steps itself (custom makefile, or one compiler invocation over
        // the whole unit list). The IDE only resolves the tools.
        LightCpp::Plugin::ProjectBackend* backend =
            pluginManager_ == nullptr
                ? nullptr
                : pluginManager_->backendFor(devSummary_.filePath);
        if (backend == nullptr) {
            outputPanel_->appendBuildOutput(
                tr("The plugin that opened %1 is no longer loaded — cannot "
                   "build.")
                    .arg(devSummary_.filePath));
            return {};
        }
        QList<ProjectStep> steps =
            toProjectSteps(backend->buildSteps(makeBuildRequest()));
        if (steps.isEmpty()) {
            // The backend declined (its build tools are missing, most
            // likely) — a silent "nothing happens" is the one outcome worse
            // than a failure, so say why the queue is empty.
            outputPanel_->appendBuildOutput(
                tr("No build steps for %1 — the required build tools are "
                   "missing. See the Plugins channel for details.")
                    .arg(devSummary_.filePath));
        }
        return steps;
    }

    outputPanel_->appendBuildOutput(tr("No project is open."));
    return {};
}

void MainWindow::startProjectBuildSteps()
{
    if (projectProcess_ != nullptr
        && projectProcess_->state() != QProcess::NotRunning) {
        outputPanel_->appendBuildOutput(tr("A project build is already running."));
        projectSteps_.clear();
        return;
    }
    if (projectSteps_.isEmpty()) {
        return;
    }

    // A fresh project build starts with a fresh problem list, like a
    // single-file compile: the stale entries of the previous run must not
    // resurface mixed into the new ones.
    clearCompilerProblems();
    projectLineTail_.clear();

    if (projectProcess_ == nullptr) {
        projectProcess_ = new QProcess(this);
        // One stream: CMake writes warnings to stderr and progress to stdout,
        // and interleaving them in the order they happened reads better than
        // two separate halves of the log.
        projectProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(projectProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            if (projectProcess_ == nullptr) return;
            // Probed decoding: MSVC speaks the local code page, MinGW UTF-8.
            const QString text = projectBuildDecoder_.decode(
                projectProcess_->readAllStandardOutput());
            if (!text.isEmpty()) {
                outputPanel_->appendBuildOutput(text);
                // Same line-splitting as the single-file compiler: a chunk
                // may end mid-line, so the remainder is kept for next time.
                consumeCompilerLines(text, projectLineTail_);
            }
        });
        connect(projectProcess_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            if (projectProcess_ == nullptr) return;
            const QString tail = projectBuildDecoder_.flush();
            if (!tail.isEmpty()) {
                outputPanel_->appendBuildOutput(tail);
            }
            consumeCompilerLines(tail, projectLineTail_);
            if (!projectLineTail_.isEmpty()) {
                processCompilerLogLine(projectLineTail_);
                projectLineTail_.clear();
            }
            if (status != QProcess::NormalExit || code != 0) {
                finishProjectBuild(false, code);
                return;
            }
            runNextProjectStep();  // next step, or done
        });
        connect(projectProcess_, &QProcess::errorOccurred, this, [this] {
            if (projectProcess_ == nullptr) return;
            if (projectProcess_->error() != QProcess::FailedToStart) {
                return;
            }
            outputPanel_->appendBuildOutput(
                tr("Cannot start %1. Is the toolchain installed and on PATH?")
                    .arg(projectProcess_->program()));
            finishProjectBuild(false, -1);
        });
    }
    runNextProjectStep();
}

void MainWindow::runNextProjectStep()
{
    if (projectSteps_.isEmpty()) {
        finishProjectBuild(true, 0);
        return;
    }
    const ProjectStep step = projectSteps_.takeFirst();

    QDir().mkpath(step.workingDirectory);
    // Relative diagnostic paths the step prints ("src/main.cpp:3:5: ...")
    // are resolved against the step's own working directory.
    currentBuildWorkingDir_ = step.workingDirectory;
    outputPanel_->showChannel(OutputPanel::ChannelBuild);
    outputPanel_->appendBuildOutput(
        tr("--- %1 ---").arg(step.label.isEmpty() ? step.program : step.label));
    outputPanel_->appendBuildOutput(
        QStringLiteral("> %1 %2").arg(step.program, step.arguments.join(QLatin1Char(' '))));

    // PATH, in this order: for an MSVC kit the toolset's own bin directory
    // (cl.exe is unusable without it), then the kit's compiler directory (so
    // CMake's compiler detection picks the compiler the kit names — otherwise
    // it finds whatever g++ the IDE's tool directory happens to hold and the
    // project builds with a different toolchain than F7 and clangd use), then
    // the IDE's build-tool directory (cmake/ninja/make plus the MinGW runtime
    // DLLs the binaries it produces need), then everything already on PATH.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList pathParts;

    if (activeKitIsMsvc()) {
        // Exactly what vcvarsall.bat would otherwise have computed, and the
        // same environment the single-file compile builds: cl.exe gets
        // INCLUDE/LIB and its own bin directory, so F7 and Build Project agree
        // on the toolset instead of the project quietly compiling with the
        // IDE's MinGW.
        ensureMsvcToolchainDetected();
        if (!msvcToolchain_.isValid()) {
            outputPanel_->showChannel(OutputPanel::ChannelBuild);
            outputPanel_->appendBuildOutput(
                tr("No MSVC toolchain available: %1").arg(msvcToolchain_.problem));
            finishProjectBuild(false, -1);
            return;
        }
        pathParts.append(msvcToolchain_.binDir);
        // Set rather than appended: a stale environment inherited from a VS
        // developer prompt must not point the compiler at another toolset.
        env.insert(QStringLiteral("INCLUDE"),
                   msvcToolchain_.includeDirs.join(QLatin1Char(';')));
        env.insert(QStringLiteral("LIB"),
                   msvcToolchain_.libDirs.join(QLatin1Char(';')));
    }

    const QString kitBin = kitToolchainDirectory();
    if (!kitBin.isEmpty()) {
        pathParts.append(kitBin);
    }
    pathParts.append(kMingwBin);
    pathParts.append(env.value(QStringLiteral("PATH")));
    env.insert(QStringLiteral("PATH"), pathParts.join(QDir::listSeparator()));
    projectProcess_->setProcessEnvironment(env);
    projectProcess_->setWorkingDirectory(step.workingDirectory);
    projectProcess_->start(step.program, step.arguments);
}

void MainWindow::finishProjectBuild(bool succeeded, int exitCode)
{
    projectSteps_.clear();
    if (succeeded) {
        outputPanel_->appendBuildOutput(tr("--- Project build succeeded ---"));
        // Resolve what to run. The build tree is the authority: the IDE may not
        // have been the one that produced the binary.
        if (projectKind_ == ProjectKind::CMake && cmakeProject_.has_value()) {
            lastProjectExe_ = LightCpp::Project::preferredExecutable(
                LightCpp::Project::buildDirectory(*cmakeProject_, buildConfiguration()));
        } else if (hasDevProject_) {
            lastProjectExe_ = devSummary_.outputPath;
        }
        if (!lastProjectExe_.isEmpty()) {
            outputPanel_->appendBuildOutput(tr("Run Project will start: %1").arg(lastProjectExe_));
        }
    } else {
        outputPanel_->appendBuildOutput(
            tr("--- Project build failed (exit %1) ---").arg(exitCode));
        // Failed builds jump the panel to the parsed problem list.
        if (!compilerProblems_.isEmpty()) {
            outputPanel_->showChannel(OutputPanel::ChannelProblemsCompiler);
        }
    }
    updateProjectActions();
}

void MainWindow::updateProjectActions()
{
    const bool hasProject = (projectKind_ != ProjectKind::None);
    const bool hasRunTarget = hasProject && !lastProjectExe_.isEmpty()
                              && QFileInfo::exists(lastProjectExe_);
    if (actConfigureProject_ != nullptr) {
        actConfigureProject_->setEnabled(projectKind_ == ProjectKind::CMake);
    }
    if (actBuildProject_ != nullptr) {
        actBuildProject_->setEnabled(hasProject);
    }
    if (actRunProject_ != nullptr) {
        actRunProject_->setEnabled(hasRunTarget);
    }
    if (actCloseProject_ != nullptr) {
        actCloseProject_->setEnabled(hasProject);
    }
    if (actFormatProject_ != nullptr) {
        actFormatProject_->setEnabled(hasProject);
    }
}

// ---------------------------------------------------------------------------
// Git
// ---------------------------------------------------------------------------
QString MainWindow::gitWorkingDirectory() const
{
    if (projectKind_ == ProjectKind::CMake && cmakeProject_.has_value()) {
        return cmakeProject_->sourceDir;
    }
    if (projectKind_ == ProjectKind::DevCpp && hasDevProject_) {
        return devSummary_.projectDir;
    }
    // An open Explorer folder is the session's working set, and it wins
    // over a stray editor tab: after browsing to a non-repository folder
    // the git slot must go (bug report 2026-09-30: the branch of whatever
    // directory the process was started in stayed on).
    if (fileExplorer_ != nullptr) {
        const QString root = fileExplorer_->rootPath();
        if (!root.isEmpty() && QFileInfo::exists(root)) {
            return root;
        }
    }
    if (CodeEditor* ed = currentEditor(); ed != nullptr && !ed->filePath().isEmpty()) {
        return QFileInfo(ed->filePath()).absolutePath();
    }
    // Nothing open: no project, no folder, no file. The user's Explorer
    // shows "This PC" — there is no working set for git to describe, and
    // falling back to the process's start-up directory would drag in
    // whatever repository the IDE happened to be launched from (bug
    // report 2026-09-30: launched from dist, git reported LightCpp while
    // the Explorer showed "This PC"). An empty result means "no git".
    return {};
}

void MainWindow::syncGitWorkingDirectory()
{
    if (git_ == nullptr) {
        return;
    }
    const QString dir = gitWorkingDirectory();
    if (dir.isEmpty()) {
        // No working set at all: drop the previous directory together with
        // the slot and the remembered status, so nothing from a repository
        // the IDE was launched from survives.
        if (!git_->workingDirectory().isEmpty()) {
            git_->setWorkingDirectory(QString());
            inRepository_ = false;
            gitStatus_ = {};
            statusBar()->removeWidget(statusGit_);
            gitSlotInserted_ = false;
            statusGit_->hide();
            statusGit_->setToolTip(QString());
            setGitActionsEnabled();
        }
        return;
    }
    if (git_->workingDirectory() == dir) {
        return;
    }
    git_->setWorkingDirectory(dir);
    git_->refreshStatus();
}

void MainWindow::onGitRefresh()
{
    syncGitWorkingDirectory();
    // An explicit refresh, so the answer includes every changed file. The
    // automatic ones stay to a single line to keep the pane readable.
    gitStatusWantsFileList_ = true;
    git_->refreshStatus();
}

void MainWindow::onGitStatusReady(const LightCpp::Project::GitStatusSummary& summary)
{
    // `git status` fails outside a repository, and the client reports that as
    // an empty summary rather than as an error to print — so "no branch and no
    // entries" means there is nothing to track here.
    inRepository_ = !summary.branch.isEmpty() || summary.detached
                    || !summary.entries.isEmpty();
    gitStatus_ = summary;

    setGitActionsEnabled();
    if (!inRepository_) {
        // Outside a repository the slot disappears entirely instead of sitting
        // there as an empty label — removeWidget, not just hide(): a hidden
        // widget keeps its separator line in the status bar.
        statusBar()->removeWidget(statusGit_);
        gitSlotInserted_ = false;
        statusGit_->hide();
        statusGit_->setToolTip(QString());
        if (gitStatusWantsFileList_) {
            outputPanel_->showChannel(OutputPanel::ChannelGit);
            outputPanel_->appendGitOutput(
                tr("--- %1 is not inside a git repository ---")
                    .arg(git_->workingDirectory()));
            outputPanel_->appendGitOutput(
                tr("Git > Initialise Repository creates one here."));
        }
        gitStatusWantsFileList_ = false;
        return;
    }

    // (Re-)insert the slot at the head of the permanent group — its place
    // before the language/cursor/encoding widgets — then bring it up.
    // Qt indexes the bar's TEMPORARY and PERMANENT widgets in one combined
    // list; statusFile_ (stretch 3) is the only temporary widget, so the
    // permanent group starts at combined index 1. insertPermanentWidget(0)
    // would land inside the temporary region: Qt warns "Index out of range"
    // and falls back to appending after the encoding combo. Never test
    // parentWidget() here — removeWidget() does not reparent, so it still
    // reports the status bar even when the slot is out of the item list.
    if (!gitSlotInserted_) {
        statusBar()->insertPermanentWidget(1, statusGit_);
        gitSlotInserted_ = true;
    }
    statusGit_->show();
    statusGit_->setText(summary.label());
    statusGit_->setToolTip(LightCpp::Project::describeGitStatus(summary));
    showGitStatus(summary, gitStatusWantsFileList_);
    gitStatusWantsFileList_ = false;
}

void MainWindow::setGitActionsEnabled()
{
    if (actGitRefresh_ != nullptr) {
        actGitRefresh_->setEnabled(true);
    }
    if (actGitStageAll_ != nullptr) {
        actGitStageAll_->setEnabled(inRepository_);
    }
    if (actGitUnstageAll_ != nullptr) {
        actGitUnstageAll_->setEnabled(inRepository_ && gitStatus_.stagedCount() > 0);
    }
    if (actGitCommit_ != nullptr) {
        actGitCommit_->setEnabled(inRepository_ && gitStatus_.stagedCount() > 0);
    }
    if (actGitLog_ != nullptr) {
        actGitLog_->setEnabled(inRepository_);
    }
    if (actGitDiff_ != nullptr) {
        actGitDiff_->setEnabled(inRepository_);
    }
    if (actGitDiffStaged_ != nullptr) {
        actGitDiffStaged_->setEnabled(inRepository_);
    }
    if (actGitInit_ != nullptr) {
        actGitInit_->setEnabled(!inRepository_);
    }
}

void MainWindow::showGitStatus(const LightCpp::Project::GitStatusSummary& summary,
                               bool withFileList)
{
    outputPanel_->showChannel(OutputPanel::ChannelGit);
    outputPanel_->appendGitOutput(
        tr("--- %1 ---").arg(summary.label().isEmpty() ? tr("no branch")
                                                       : summary.label()));
    outputPanel_->appendGitOutput(LightCpp::Project::describeGitStatus(summary));
    if (!withFileList) {
        return;
    }
    for (const LightCpp::Project::GitEntry& entry : summary.entries) {
        QString line = QStringLiteral("%1  %2").arg(entry.code(), entry.path);
        if (!entry.originalPath.isEmpty()) {
            line += QStringLiteral("   (was %1)").arg(entry.originalPath);
        }
        outputPanel_->appendGitOutput(line);
    }
}

void MainWindow::onGitCommandFinished(const QString& title, const QString& text,
                                      int exitCode)
{
    outputPanel_->showChannel(OutputPanel::ChannelGit);
    outputPanel_->appendGitOutput(
        exitCode == 0 ? tr("--- %1 ---").arg(title)
                      : tr("--- %1 (exit %2) ---").arg(title).arg(exitCode));
    if (!text.isEmpty()) {
        outputPanel_->appendGitOutput(text);
    }
    // Every command can change the branch, the index or the working tree, so
    // the status bar is re-read rather than guessed at.
    if (inRepository_ || exitCode == 0) {
        git_->refreshStatus();
    }
}

void MainWindow::onGitStageAll()
{
    git_->run({QStringLiteral("add"), QStringLiteral("--all")},
              QStringLiteral("git add --all"));
}

void MainWindow::onGitUnstageAll()
{
    // `git reset` with no paths is the documented way to clear the index; on a
    // branch with no commits yet it has nothing to reset to and says so, which
    // is a better outcome than silently doing nothing.
    git_->run({QStringLiteral("reset"), QStringLiteral("--quiet")},
              QStringLiteral("git reset"));
}

void MainWindow::onGitCommit()
{
    if (!inRepository_) {
        return;
    }
    bool accepted = false;
    const QString message = QInputDialog::getMultiLineText(
        this, tr("Commit"), tr("Commit message:"), QString(), &accepted);
    if (!accepted || message.trimmed().isEmpty()) {
        return;
    }
    git_->run({QStringLiteral("commit"), QStringLiteral("-m"), message.trimmed()},
              QStringLiteral("git commit"));
}

void MainWindow::onGitLog()
{
    git_->run({QStringLiteral("log"), QStringLiteral("--oneline"),
               QStringLiteral("-n"), QStringLiteral("40")},
              QStringLiteral("git log --oneline -n 40"));
}

void MainWindow::showGitDiff(bool staged)
{
    QStringList args{QStringLiteral("diff")};
    if (staged) {
        args << QStringLiteral("--staged");
    }
    // Narrowing to the active file is what makes the command useful from the
    // editor; with nothing open it shows the whole tree, which is still the
    // right answer to "what have I changed".
    if (CodeEditor* ed = currentEditor(); ed != nullptr && !ed->filePath().isEmpty()) {
        args << QStringLiteral("--") << QDir::fromNativeSeparators(ed->filePath());
    }
    git_->run(args, staged ? tr("git diff --staged") : tr("git diff"));
}

void MainWindow::onGitInit()
{
    if (inRepository_) {
        return;
    }
    git_->setWorkingDirectory(gitWorkingDirectory());
    git_->run({QStringLiteral("init")}, QStringLiteral("git init"));
}

void MainWindow::onLintTimerTimeout()
{
    auto* ed = pendingLintEditor_;
    if (!ed) return;
    pendingLintEditor_ = nullptr;
    if (!lsp_->isRunning()) return;
    // Untitled buffers use a virtual path (see onNewFile), so they are
    // linted too. changeDocument auto-opens on the first send.
    const QString lintPath = ed->lintPath();
    if (lintPath.isEmpty() || !isCppLikeFile(lintPath)) {
        return;
    }
    lsp_->changeDocument(lintPath, ed->toPlainText());
    // The buffer just changed on the server side — refresh the outline in
    // the same round trip (cheap; clangd reuses the parsed AST).
    requestOutline();
}

void MainWindow::scheduleLint(CodeEditor* editor)
{
    pendingLintEditor_ = editor;
    lintDebounceTimer_->start();
}

bool MainWindow::isCppLikeFile(const QString& path)
{
    const QString suf = QFileInfo(path).suffix().toLower();
    return suf == "c" || suf == "cpp" || suf == "cc" || suf == "cxx"
           || suf == "h" || suf == "hpp" || suf == "hh" || suf == "hxx"
           || suf == "inl" || suf == "ipp";
}

QString MainWindow::languageIdForFile(const QString& path)
{
    const QString suf = QFileInfo(path).suffix().toLower();
    if (suf == "c") return QStringLiteral("c");
    if (suf == "h") return QStringLiteral("c");  // ambiguous; clangd detects
    return QStringLiteral("cpp");
}

// ---------------------------------------------------------------------------
// Single-file build & run
// ---------------------------------------------------------------------------
MainWindow::BuildTarget MainWindow::resolveBuildTarget(CodeEditor* editor) const
{
    BuildTarget target;
    if (editor == nullptr) {
        return target;
    }
    if (!editor->filePath().isEmpty()) {
        target.srcPath = editor->filePath();
    } else {
        // An unsaved buffer still has to exist on disk for the compiler, so
        // it is built from a temp copy. The stem comes from the tab title
        // (minus the modification dot) so repeat builds overwrite each other
        // instead of littering one file per keystroke-free retry.
        const int idx = tabWidget_->indexOf(editor);
        QString stem = idx >= 0
                ? QFileInfo(tabWidget_->tabText(idx)).completeBaseName()
                : QStringLiteral("Untitled");
        stem.remove(QStringLiteral(" \u25CF"));
        if (stem.isEmpty()) {
            stem = QStringLiteral("Untitled");
        }
        target.tempSource = true;
        target.srcPath =
            QDir(QDir(QDir::tempPath())
                     .filePath(QStringLiteral("lightcpp-compile")))
                .filePath(stem + QStringLiteral(".cpp"));
    }
    // The binary always sits next to its source, so Run looks exactly where
    // the last build wrote without having to remember anything.
    target.exePath = QDir(QFileInfo(target.srcPath).absolutePath())
                         .filePath(QFileInfo(target.srcPath).completeBaseName()
                                   + QStringLiteral(".exe"));
    return target;
}

QString MainWindow::systemConsoleCharset()
{
    static const QString charset = [] {
        // What a fresh console window decodes the program's narrow bytes
        // as: cmd — and so ConsolePauser and the standalone terminals —
        // starts at the OEM default code page (936, GBK, on zh-CN Windows).
        UINT codePage = GetOEMCP();
        if (codePage == 0) {
            codePage = GetACP();
        }
        switch (codePage) {
        case 65001: return QStringLiteral("UTF-8");
        case 936:   return QStringLiteral("GBK");
        case 950:   return QStringLiteral("BIG5");
        case 932:   return QStringLiteral("SHIFT_JIS");
        case 949:   return QStringLiteral("UHC");
        default:    return QStringLiteral("CP%1").arg(codePage);
        }
    }();
    return charset;
}

QString MainWindow::requiredExecCharset() const
{
    // Captured Program Output decodes each line (UTF-8 vs local code page
    // probe), so any encoding displays — keep the compiler's default. Every
    // console-backed mode (Console Pauser, the terminals) shows the bytes
    // in a real console window, which expects the system's default code
    // page: the narrow string literals must be compiled for it.
    if (programOutputMode_ == QLatin1String("program")) {
        return {};
    }
    return systemConsoleCharset();
}

void MainWindow::onCompileCurrentFile()
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr) {
        outputPanel_->showChannel(OutputPanel::ChannelBuild);
        outputPanel_->appendBuildOutput(tr("No active file to compile."));
        runAfterBuild_ = false;
        return;
    }
    const BuildTarget target = resolveBuildTarget(ed);
    const QString& srcPath = target.srcPath;

    // The compiler needs real bytes on disk. For an unsaved Untitled buffer
    // write a temp .cpp file; for a saved file just flush the buffer to
    // its path so the compiler reads the latest content.
    if (target.tempSource) {
        QDir().mkpath(QFileInfo(srcPath).absolutePath());
        if (std::optional<QByteArray> payload = encodedSavePayload(ed);
            payload.has_value()) {
            if (const std::optional<LightCpp::Text::WriteFileError> error =
                    LightCpp::Text::writeFileAtomically(srcPath, *payload)) {
                outputPanel_->showChannel(OutputPanel::ChannelBuild);
                outputPanel_->appendBuildOutput(
                    tr("Cannot write temp file %1: %2")
                        .arg(srcPath,
                             QString::fromUtf8(
                                 LightCpp::Text::toString(*error))));
                runAfterBuild_ = false;
                return;
            }
        } else {
            runAfterBuild_ = false;
            return;  // encoding error already reported by the helper
        }
    } else if (ed->isModified()) {
        // Go through the shared save path rather than writing here. Besides
        // the atomic write, it records selfWriteMs_ — without that timestamp
        // the file watcher saw this write as an external change and reloaded
        // the buffer, so every F7 cleared the undo history and dropped
        // whatever had been typed since.
        if (!saveEditorToDisk(ed)) {
            // saveEditorToDisk() already explained the reason in the channel.
            outputPanel_->showChannel(OutputPanel::ChannelBuild);
            outputPanel_->appendBuildOutput(
                tr("Build aborted: %1 could not be saved.").arg(srcPath));
            runAfterBuild_ = false;
            return;
        }
    }

    // Pick the compiler from the active kit (the toolbar's Kit drop-down).
    // Headers compile as C++ by default. Without any kit the built-in MSYS2
    // GCC applies, exactly as before kits existed.
    const QString suf = QFileInfo(srcPath).suffix().toLower();
    const bool isC = (suf == "c");
    const QString outExe = target.exePath;
    const std::optional<LightCpp::Settings::Kit> kit = activeKit();

    QString compilerExe;
    QStringList args;
    QStringList linkFlags;  // appended after the source, for GNU-style
    QStringList toolDirs;   // prepended to the child's PATH
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    if (kit.has_value() && isMsvcKit(*kit)) {
        ensureMsvcToolchainDetected();
        if (!msvcToolchain_.isValid()) {
            outputPanel_->showChannel(OutputPanel::ChannelBuild);
            outputPanel_->appendBuildOutput(
                tr("No MSVC toolchain available: %1").arg(msvcToolchain_.problem));
            runAfterBuild_ = false;
            return;
        }
        compilerExe = kit->cppCompiler;
        toolDirs << msvcToolchain_.binDir;

        // cl always emits a .obj and there is no flag that suppresses it, so it
        // is sent to temp rather than left in the user's source tree. The .exe
        // must still land next to the source — "Run" looks there.
        const QString objDir = QDir(QDir::tempPath())
                                   .filePath(QStringLiteral("lightcpp-compile/msvc"));
        QDir().mkpath(objDir);
        const QString objPath = QDir(objDir).filePath(
            QFileInfo(srcPath).completeBaseName() + QStringLiteral(".obj"));

        // /nologo /W4 /EHsc are the usual notice-level settings; /MT links the
        // C runtime statically so the exe runs on its own, playing the same
        // role as -static-libgcc -static-libstdc++ on the MinGW side. No /std:
        // — the compiler's own default is what gets run.
        args = {QStringLiteral("/nologo"), QStringLiteral("/W4"),
                QStringLiteral("/EHsc"), QStringLiteral("/MT")};
        if (!isC) {
            // cl infers the language from the extension, so a .h would compile
            // as C. /TP forces C++, matching what the GCC path does by picking
            // g++ for headers.
            args << QStringLiteral("/TP");
        }
        // The kit's own Debug/Release definitions when it has any, else the
        // built-in table: Debug is /Od /Zi (/Zi gives cdb its PDB — without
        // it MSVC debugging starts but hits nothing), Release adds /O2
        // /DNDEBUG.
        bool kitDefinesConfig = false;
        for (const LightCpp::Settings::KitBuildConfiguration& config :
             kit->buildConfigurations) {
            if (config.name == currentBuildConfigurationName()) {
                kitDefinesConfig = true;
                break;
            }
        }
        if (kitDefinesConfig) {
            args += settings_
                        ->effectiveBuildConfig(*kit, currentBuildConfigurationName())
                        .compileFlags;
        } else {
            args += LightCpp::Project::flagsFor(buildConfiguration(),
                                                LightCpp::Project::CompilerFamily::Msvc);
        }
        // The kit's global flags come last, so they can override the base.
        args += kit->compileFlags;
        args << (QStringLiteral("/Fe") + QDir::toNativeSeparators(outExe))
             << (QStringLiteral("/Fo") + QDir::toNativeSeparators(objPath))
             << QDir::toNativeSeparators(srcPath);

        // cl.exe already emits narrow literals in the system ANSI code page
        // — exactly what a console expects — so no flag is needed; record
        // the charset so the run-mode rebuild check knows this binary
        // matches every run mode.
        lastBuildExecCharset_ = systemConsoleCharset();

        // Exactly what vcvarsall.bat would otherwise have computed. Set rather
        // than appended so a stale environment inherited from a VS dev prompt
        // cannot point the compiler at a different toolset.
        env.insert(QStringLiteral("INCLUDE"),
                   msvcToolchain_.includeDirs.join(QLatin1Char(';')));
        env.insert(QStringLiteral("LIB"),
                   msvcToolchain_.libDirs.join(QLatin1Char(';')));
    } else {
        // GNU-style: the kit's compilers, or the built-in MSYS2 GCC when no
        // kit is selected. The binary dir travels with the compiler: the
        // runtime DLLs it (and its output) need live next to it.
        QString cppCompiler =
            kit.has_value() && !kit->cppCompiler.isEmpty()
                ? kit->cppCompiler
                : QDir(kMingwBin).filePath(QStringLiteral("g++.exe"));
        QString cCompiler =
            kit.has_value() && !kit->cCompiler.isEmpty()
                ? kit->cCompiler
                : QDir(kMingwBin).filePath(QStringLiteral("gcc.exe"));
        compilerExe = isC ? cCompiler : cppCompiler;
        toolDirs << QFileInfo(compilerExe).absolutePath();

        if (kit.has_value()) {
            // Compile flags and the C++ standard from the kit: the named
            // build configuration on top of the kit's own flags.
            const LightCpp::Settings::EffectiveBuildConfig effective =
                settings_->effectiveBuildConfig(*kit,
                                                currentBuildConfigurationName());
            args = effective.compileFlags;
            if (!effective.cppStandard.isEmpty()) {
                args << (QStringLiteral("-std=") + effective.cppStandard);
            }
            linkFlags = effective.linkFlags;
        } else {
            // -g for Debug, -O2 -DNDEBUG for Release; both are in the same
            // place the project builds read their flags from.
            args = {QStringLiteral("-Wall"), QStringLiteral("-Wextra")};
            args += LightCpp::Project::flagsFor(buildConfiguration(),
                                                LightCpp::Project::CompilerFamily::Gnu);
        }
        // Console-backed run modes show the program's bytes in a real
        // console, which speaks the system's default code page — not
        // MinGW's UTF-8 default. Compile the narrow string literals for
        // that charset; captured Program Output keeps the compiler default
        // (its decoder probes each line instead).
        //
        // clang (clang++/clang) only implements -fexec-charset for UTF-8 and
        // rejects any other value outright ("invalid value 'GBK'"), so the
        // flag is a GCC-only knob: skip it for the clang family and record
        // the charset the binary actually got. Claiming the required charset
        // in lastBuildExecCharset_ (instead of the real UTF-8) is deliberate:
        // a rebuild could not honour it either, and recording a mismatch
        // would make every console-backed run ask for a futile rebuild.
        const bool clangFamily = QFileInfo(compilerExe).fileName()
                                     .contains(QStringLiteral("clang"),
                                               Qt::CaseInsensitive);
        const QString execCharset = requiredExecCharset();
        lastBuildExecCharset_ = execCharset;
        if (!execCharset.isEmpty()) {
            if (clangFamily) {
                outputPanel_->appendBuildOutput(
                    tr("Note: clang has no -fexec-charset support for %1; "
                       "string literals stay UTF-8 (console output of "
                       "non-ASCII text may be garbled).")
                        .arg(execCharset));
            } else {
                args << (QStringLiteral("-fexec-charset=") + execCharset);
            }
        }
        args << srcPath << QStringLiteral("-o") << outExe;
        if (!linkFlags.isEmpty()) {
            args += linkFlags;
        }
    }

    // Lazily create the QProcess once and wire its handlers.
    if (compiler_ == nullptr) {
        compiler_ = new QProcess(this);
        compiler_->setProcessChannelMode(QProcess::MergedChannels);
        connect(compiler_, &QProcess::readyReadStandardOutput, this, [this] {
            if (compiler_ == nullptr) return;
            const QByteArray b = compiler_->readAllStandardOutput();
            // Probed decoding: MSVC speaks the local code page, MinGW UTF-8.
            const QString text = buildDecoder_.decode(b);
            if (!text.isEmpty()) {
                outputPanel_->appendBuildOutput(text);
                // A chunk can carry several log lines (and half of the next
                // one) — the splitter keeps the remainder for the next call.
                consumeCompilerLines(text, compilerLineTail_);
            }
        });
        connect(compiler_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            const int rc = (status == QProcess::NormalExit) ? code : -1;
            const QString tail = buildDecoder_.flush();
            if (!tail.isEmpty()) {
                outputPanel_->appendBuildOutput(tail);
            }
            consumeCompilerLines(tail, compilerLineTail_);
            if (!compilerLineTail_.isEmpty()) {
                processCompilerLogLine(compilerLineTail_);
                compilerLineTail_.clear();
            }
            outputPanel_->appendBuildOutput(
                rc == 0 ? tr("--- Build succeeded (exit 0) ---")
                        : tr("--- Build failed (exit %1) ---").arg(rc));
            // Failed compiles jump the panel to the parsed problem list.
            if (rc != 0 && !compilerProblems_.isEmpty()) {
                outputPanel_->showChannel(OutputPanel::ChannelProblemsCompiler);
            }
            updateRunActionStates();
            // "Compile & Run" only continues on a successful build.
            const bool runNow = runAfterBuild_;
            runAfterBuild_ = false;
            if (runNow) {
                if (rc == 0) {
                    startProgram(lastBuildExe_, QFileInfo(lastBuildExe_).absolutePath());
                } else {
                    outputPanel_->appendBuildOutput(
                        tr("Build failed — nothing was run."));
                }
            }
        });
        connect(compiler_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            outputPanel_->appendBuildOutput(
                tr("Compiler failed to start. Check that %1 exists.")
                    .arg(lastCompilerPath_.isEmpty()
                             ? QDir(kMingwBin).filePath(QStringLiteral("<gcc|g++|clang|clang++>.exe"))
                             : lastCompilerPath_));
            runAfterBuild_ = false;
            updateRunActionStates();
        });
    }

    if (compiler_->state() != QProcess::NotRunning) {
        outputPanel_->showChannel(OutputPanel::ChannelBuild);
        outputPanel_->appendBuildOutput(tr("A build is already running."));
        runAfterBuild_ = false;
        return;
    }

    // Each toolchain needs its own bin directory on the child's PATH: the
    // MinGW compilers and the binaries they produce both need the UCRT64
    // runtime DLLs, and cl.exe needs its own alongside it.
    env.insert(QStringLiteral("PATH"),
               toolDirs.join(QDir::listSeparator()) + QDir::listSeparator()
                   + env.value(QStringLiteral("PATH")));
    compiler_->setProcessEnvironment(env);
    compiler_->setWorkingDirectory(QFileInfo(srcPath).absolutePath());
    // Compiler log lines may carry paths relative to this directory.
    currentBuildWorkingDir_ = QFileInfo(srcPath).absolutePath();

    // A fresh build starts from an empty problem list; the editors' compiler
    // markers go with it (LSP squiggles are restored from diagnosticsByFile_).
    clearCompilerProblems();
    compilerLineTail_.clear();

    outputPanel_->showChannel(OutputPanel::ChannelBuild);
    // Name the toolset as well as the compiler: with more than one MSVC
    // installed, cl.exe alone does not say which one produced these errors.
    QString compilerBanner = QFileInfo(compilerExe).fileName();
    if (activeKitIsMsvc()) {
        compilerBanner += QStringLiteral(" [%1]").arg(msvcToolchain_.label);
    }
    outputPanel_->appendBuildOutput(
        tr("--- Compiling: %1 %2 ---")
            .arg(compilerBanner, args.join(QStringLiteral(" "))));
    lastBuildExe_ = outExe;
    lastCompilerPath_ = compilerExe;
    compiler_->start(compilerExe, args);
    updateRunActionStates();
}

void MainWindow::onCompileAndRun()
{
    // While a debug session is paused, F5 means "continue" — the same
    // convention Visual Studio and other IDEs use.
    if (debugger_ != nullptr && debugger_->isRunning()) {
        debugger_->cont();
        return;
    }
    // The run step happens in the compiler's finished handler.
    runAfterBuild_ = true;
    onCompileCurrentFile();
}

void MainWindow::onRunCurrentFile()
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr) {
        outputPanel_->showChannel(OutputPanel::ChannelProgram);
        outputPanel_->appendProgramOutput(tr("No active file to run."));
        return;
    }
    const BuildTarget target = resolveBuildTarget(ed);
    const QString exePath = target.exePath;
    outputPanel_->showChannel(OutputPanel::ChannelProgram);

    if (!QFileInfo::exists(exePath)) {
        // Nothing has been built yet — offer to build it instead of just
        // failing, which is what the user almost certainly wants.
        const QString label = target.tempSource
                ? tabWidget_->tabText(tabWidget_->indexOf(ed))
                : QFileInfo(exePath).fileName();
        outputPanel_->appendProgramOutput(
            tr("Nothing to run yet: %1 has not been compiled.").arg(exePath));
        const auto reply = QMessageBox::question(
            this, tr("Not Compiled"),
            tr("\"%1\" has not been compiled yet.\n\nCompile and run it now?")
                .arg(label),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (reply == QMessageBox::Yes) {
            onCompileAndRun();
        } else {
            outputPanel_->appendProgramOutput(
                tr("Hint: Tools > Compile & Run (F5) builds it first."));
        }
        return;
    }

    // The binary exists, but it may predate the current text. Running a stale
    // build is almost never what was meant, so ask instead of just noting it.
    const QFileInfo exeInfo(exePath);
    bool stale = ed->isModified();
    if (!stale && !target.tempSource) {
        stale = QFileInfo(target.srcPath).lastModified() > exeInfo.lastModified();
    }
    if (stale) {
        const auto reply = QMessageBox::question(
            this, tr("Source Changed"),
            tr("\"%1\" was built before the source was last changed.\n\n"
               "Rebuild it now?")
                .arg(exeInfo.fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (reply == QMessageBox::Yes) {
            onCompileAndRun();
            return;
        }
        outputPanel_->appendProgramOutput(
            tr("Running the existing %1 without rebuilding.")
                .arg(exeInfo.fileName()));
    }
    // A binary built while another run mode was selected may speak the
    // wrong encoding: console-backed modes must show the system code page,
    // so rebuild instead of printing mojibake into the console.
    const QString requiredCharset = requiredExecCharset();
    if (!requiredCharset.isEmpty()
        && lastBuildExecCharset_ != requiredCharset) {
        outputPanel_->appendProgramOutput(
            tr("Run output encoding changed — rebuilding with %1 string "
               "literals for the console.")
                .arg(requiredCharset));
        onCompileAndRun();
        return;
    }
    startProgram(exePath, QFileInfo(target.srcPath).absolutePath());
}

void MainWindow::onStopProgram()
{
    const bool programRunning = runner_ != nullptr
        && runner_->state() != QProcess::NotRunning;
    const bool terminalRunning = terminalRunner_ != nullptr
        && terminalRunner_->state() != QProcess::NotRunning;
    if (!programRunning && !terminalRunning) {
        return;
    }
    if (programRunning) {
        outputPanel_->showChannel(OutputPanel::ChannelProgram);
        outputPanel_->appendProgramOutput(tr("--- Stopping program ---"));
        runner_->kill();
    }
    if (terminalRunning) {
        // Show the terminal page before reporting, or the stop message lands
        // in a channel nobody is looking at. terminalChannel_ is filled in
        // when the embedded session opens its channel; -1 means no session
        // ever opened one, in which case there is nothing to switch to.
        if (terminalChannel_ >= 0) {
            outputPanel_->showChannel(
                static_cast<OutputPanel::Channel>(terminalChannel_));
        }
        outputPanel_->appendTerminalOutput(tr("--- Stopping terminal ---\n"));
        terminalRunner_->kill();
    }
}

QProcess* MainWindow::ensureProgramRunner()
{
    if (runner_ == nullptr) {
        runner_ = new QProcess(this);
        runner_->setProcessChannelMode(QProcess::MergedChannels);
        connect(runner_, &QProcess::readyReadStandardOutput, this, [this] {
            if (runner_ == nullptr) return;
            // Probed decoding: an MSVC-built program speaks the local code
            // page, an MSYS2/MinGW-built one speaks UTF-8.
            const QString text = runDecoder_.decode(
                runner_->readAllStandardOutput());
            if (!text.isEmpty()) {
                outputPanel_->appendProgramOutput(text);
            }
        });
        // The program's stdin stays open while it runs: the input field in
        // the Program Output channel feeds it, so scanf()/getline() work.
        // Embedded terminal modes share this runner — the field drives the
        // shell the same way, so it enables for every captured run.
        connect(runner_, &QProcess::started, this, [this] {
            outputPanel_->setProgramInputEnabled(true);
        });
        connect(runner_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError e) {
            if (runner_ == nullptr) return;
            if (e == QProcess::FailedToStart) {
                outputPanel_->appendProgramOutput(
                    tr("Failed to start the program. It probably needs the "
                       "MSYS2 runtime DLLs next to it (or on PATH)."));
                outputPanel_->setProgramInputEnabled(false);
            }
            updateRunActionStates();
        });
        connect(runner_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            const QString tail = runDecoder_.flush();
            if (!tail.isEmpty()) {
                outputPanel_->appendProgramOutput(tail);
            }
            outputPanel_->appendProgramOutput(
                status == QProcess::NormalExit
                    ? tr("--- Program exited with code %1 ---").arg(code)
                    : tr("--- Program was killed ---"));
            outputPanel_->setProgramInputEnabled(false);
            updateRunActionStates();
        });
    }
    return runner_;
}

QProcess* MainWindow::ensureTerminalRunner()
{
    if (terminalRunner_ == nullptr) {
        terminalRunner_ = new QProcess(this);
        terminalRunner_->setProcessChannelMode(QProcess::MergedChannels);
        connect(terminalRunner_, &QProcess::readyReadStandardOutput, this,
                [this] {
            if (terminalRunner_ == nullptr) return;
            // Raw: no decode probes, no line reassembly. The bytes are only
            // converted from the console's code page (what cmd writes, and
            // what programs built for console modes emit) and appended as
            // they arrived — a trailing "D:\>" prompt without a newline
            // stays visible, exactly like in a real console.
            outputPanel_->appendTerminalOutput(
                QString::fromLocal8Bit(
                    terminalRunner_->readAllStandardOutput()));
        });
        connect(terminalRunner_, &QProcess::started, this, [this] {
            outputPanel_->setTerminalInputEnabled(true);
        });
        connect(terminalRunner_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError e) {
            if (terminalRunner_ == nullptr) return;
            if (e == QProcess::FailedToStart) {
                outputPanel_->appendTerminalOutput(
                    tr("Failed to start the terminal.\n"));
                outputPanel_->setTerminalInputEnabled(false);
            }
            updateRunActionStates();
        });
        connect(terminalRunner_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            outputPanel_->appendTerminalOutput(
                status == QProcess::NormalExit
                    ? tr("--- Terminal exited with code %1 ---\n").arg(code)
                    : tr("--- Terminal was killed ---\n"));
            outputPanel_->setTerminalInputEnabled(false);
            updateRunActionStates();
        });
    }
    return terminalRunner_;
}

void MainWindow::startProgram(const QString& exePath, const QString& workDir)
{
    if (exePath.isEmpty() || !QFileInfo::exists(exePath)) {
        outputPanel_->appendProgramOutput(
            tr("Cannot run: %1 does not exist.").arg(exePath));
        return;
    }
    // The settings decide where the program runs: captured in the IDE, in
    // its own console via ConsolePauser.exe, or inside a terminal from the
    // editable list (embedded: captured, non-embedded: its own console).
    if (programOutputMode_ == QLatin1String("consolePauser")) {
        startWithConsolePauser(exePath, workDir);
        return;
    }
    if (programOutputMode_ != QLatin1String("program")) {
        startInTerminal(exePath, workDir);
        return;
    }
    QProcess* runner = ensureProgramRunner();

    if (runner->state() != QProcess::NotRunning) {
        outputPanel_->showChannel(OutputPanel::ChannelProgram);
        outputPanel_->appendProgramOutput(tr("A program is already running."));
        return;
    }

    // The binary links against the toolchain's runtime DLLs (libstdc++-6,
    // libgcc_s, libwinpthread), so its bin directory must be on PATH — the
    // active kit's compiler directory when there is one.
    QString runtimeBinDir = kMingwBin;
    if (const auto kit = activeKit(); kit.has_value() && !isMsvcKit(*kit)) {
        const QString dir = QFileInfo(kit->cppCompiler).absolutePath();
        if (!dir.isEmpty()) {
            runtimeBinDir = dir;
        }
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"),
               runtimeBinDir + QDir::listSeparator()
                   + env.value(QStringLiteral("PATH")));
    runner_->setProcessEnvironment(env);
    runner_->setWorkingDirectory(workDir);

    outputPanel_->showChannel(OutputPanel::ChannelProgram);
    outputPanel_->appendProgramOutput(
        tr("--- Running: %1 ---").arg(QFileInfo(exePath).absoluteFilePath()));
    runner_->start(exePath, {});
    updateRunActionStates();
}
void MainWindow::startTerminalProgramWatch(const QString& exePath)
{
    terminalProgramImage_ = QFileInfo(exePath).fileName().toLower();
    terminalProgramSeen_ = false;
    // Assume alive during the grace window so the run actions do not
    // flicker; the first poll corrects this if the program never appears.
    terminalProgramRunning_ = true;
    terminalProgramLaunch_.start();
    if (terminalProgramTimer_ == nullptr) {
        terminalProgramTimer_ = new QTimer(this);
        terminalProgramTimer_->setInterval(300);
        connect(terminalProgramTimer_, &QTimer::timeout,
                this, &MainWindow::pollTerminalProgram);
    }
    terminalProgramTimer_->start();
}

void MainWindow::pollTerminalProgram()
{
    if (terminalRunner_ == nullptr
        || terminalRunner_->state() == QProcess::NotRunning
        || terminalProgramImage_.isEmpty()) {
        terminalProgramTimer_->stop();
        terminalProgramRunning_ = false;
        updateRunActionStates();
        return;
    }
    const bool alive = shellHasChildImage(
        static_cast<unsigned long>(terminalRunner_->processId()),
        terminalProgramImage_);
    if (alive) {
        terminalProgramSeen_ = true;
        terminalProgramRunning_ = true;
    } else if (terminalProgramSeen_
               || terminalProgramLaunch_.elapsed() > 2000) {
        // Observed once and gone, or never showed up within the grace
        // window (a slow shell may still be resolving the command — do not
        // confuse that with an exit).
        terminalProgramRunning_ = false;
    }
    updateRunActionStates();
}

bool MainWindow::shellHasChildImage(unsigned long shellPid,
                                    const QString& imageLower)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ParentProcessID == shellPid
                && QString::fromWCharArray(entry.szExeFile).toLower()
                       == imageLower) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

bool MainWindow::startDetachedInNewConsole(const QString& program,
                                           const QStringList& args,
                                           const QString& workDir,
                                           const QProcessEnvironment& env)
{
    // `start ""` — the empty quoted token is the window title placeholder;
    // without it start would mistake a quoted program path for the title.
    QProcess launcher;
    launcher.setProgram(QStringLiteral("cmd.exe"));
    QStringList cmdArgs{QStringLiteral("/c"), QStringLiteral("start"),
                        QString(), QDir::toNativeSeparators(program)};
    cmdArgs.append(args);
    launcher.setArguments(cmdArgs);
    launcher.setWorkingDirectory(workDir);
    launcher.setProcessEnvironment(env);
    return launcher.startDetached();
}

void MainWindow::startWithConsolePauser(const QString& exePath,
                                        const QString& workDir)
{
    outputPanel_->showChannel(OutputPanel::ChannelProgram);
    const QString pauser = resolveConsolePauser(exePath);
    if (pauser.isEmpty()) {
        outputPanel_->appendProgramOutput(
            tr("Console Pauser is selected, but ConsolePauser.exe was not "
               "found next to the program or next to the IDE."));
        return;
    }

    // Same PATH handling as the embedded runner: even in its own console the
    // program still needs its toolchain's runtime DLLs to load.
    QString runtimeBinDir = kMingwBin;
    if (const auto kit = activeKit(); kit.has_value() && !isMsvcKit(*kit)) {
        const QString dir = QFileInfo(kit->cppCompiler).absolutePath();
        if (!dir.isEmpty()) {
            runtimeBinDir = dir;
        }
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"),
               runtimeBinDir + QDir::listSeparator()
                   + env.value(QStringLiteral("PATH")));

    // ConsolePauser.exe <filename> <parameters> — we never pass extra
    // parameters, so the program itself is the only argument. Launched via
    // `cmd /c start`: the console window is always brand new, never the
    // terminal the IDE itself was started from, and the IDE captures
    // nothing. The window stays up after the program exits (that is the
    // pause the tool adds). The environment (PATH with the toolchain's bin
    // directory) travels through cmd into the child.
    if (!startDetachedInNewConsole(pauser, {exePath}, workDir, env)) {
        outputPanel_->appendProgramOutput(
            tr("Failed to start %1.").arg(QDir::toNativeSeparators(pauser)));
        return;
    }
    outputPanel_->appendProgramOutput(
        tr("--- Running in an external console (Console Pauser): %1 ---")
            .arg(QFileInfo(exePath).absoluteFilePath()));
    // Nothing is captured, so there is nothing to stop from the IDE.
    updateRunActionStates();
}

QString MainWindow::resolveConsolePauser(const QString& exePath) const
{
    const QString name = QStringLiteral("ConsolePauser.exe");
    const QString targetDir = QFileInfo(exePath).absolutePath();
    const QStringList candidates = {
        QDir(targetDir).filePath(name),
        QDir(QCoreApplication::applicationDirPath()).filePath(name),
        QString::fromLatin1(kConsolePauserFallback),
    };
    QString found;
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            found = candidate;
            break;
        }
    }
    if (found.isEmpty()) {
        return {};
    }
    // Console Pauser is a helper of the program being run: keep a copy next
    // to it, so the pairing survives the IDE being moved or reinstalled.
    if (QFileInfo(found).absolutePath() != targetDir) {
        const QString dest = QDir(targetDir).filePath(name);
        if (QFile::copy(found, dest)) {
            found = dest;
        }
    }
    return found;
}

void MainWindow::startInTerminal(const QString& exePath, const QString& workDir)
{
    outputPanel_->showChannel(OutputPanel::ChannelProgram);

    // The selected terminal from the editable list. The command template is
    // launched as-is (it starts the shell, e.g. "cmd.exe /k"); the program's
    // absolute path is then fed to it as a line of user input.
    const QStringList tokens = QProcess::splitCommand(runTerminalCommand_);
    if (tokens.isEmpty()) {
        outputPanel_->appendProgramOutput(
            tr("Terminal \"%1\" has no command configured.")
                .arg(runTerminalName_));
        return;
    }
    QString program = QStandardPaths::findExecutable(tokens.first());
    if (program.isEmpty()) {
        program = tokens.first();
    }
    QStringList args = tokens.mid(1);

    // The program needs its toolchain's runtime DLLs wherever it ends up
    // running, so the active kit's compiler directory goes on PATH first.
    QString runtimeBinDir = kMingwBin;
    if (const auto kit = activeKit(); kit.has_value() && !isMsvcKit(*kit)) {
        const QString dir = QFileInfo(kit->cppCompiler).absolutePath();
        if (!dir.isEmpty()) {
            runtimeBinDir = dir;
        }
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"),
               runtimeBinDir + QDir::listSeparator()
                   + env.value(QStringLiteral("PATH")));

    const QString terminalLabel =
        runTerminalName_.isEmpty() ? program : runTerminalName_;
    if (runTerminalEmbedded_) {
        // Embedded, Windows Terminal style: every session opens its own
        // channel in the output drop-down, named after the selected
        // terminal. The shell's bytes land there raw — no decoding probes,
        // no line reassembly — so an unfinished prompt such as "D:\>" shows
        // up exactly the way a real console prints it, including the one
        // after the program exits.
        const bool shellUp = terminalRunner_ != nullptr
            && terminalRunner_->state() != QProcess::NotRunning;
        // Only a live *program* blocks a run: the shell (cmd /k, pwsh …)
        // intentionally outlives the program, and typing the program path
        // again into the same session is the natural rerun.
        if (shellUp && terminalProgramRunning_) {
            outputPanel_->appendTerminalOutput(
                tr("A program is already running in the terminal.\n"));
            return;
        }
        if (!shellUp) {
            terminalChannel_ = outputPanel_->openTerminalChannel(terminalLabel);
        } else {
            // Reusing the session: keep the previous output visible.
            terminalChannel_ = outputPanel_->openTerminalChannel(terminalLabel, false);
        }
        QProcess* runner = ensureTerminalRunner();
        // A PowerShell-style template ends in -Command/-c, which needs a
        // value; launch without it and use the call operator when typing.
        bool needsCallOperator = false;
        if (!args.isEmpty()
            && (args.last().compare(QLatin1String("-Command"),
                                    Qt::CaseInsensitive) == 0
                || args.last().compare(QLatin1String("-c"),
                                       Qt::CaseInsensitive) == 0)) {
            args.removeLast();
            needsCallOperator = true;
        }
        const QString firstInput = needsCallOperator
            ? QStringLiteral("& '%1'").arg(exePath)
            : exePath;
        runner->setProcessEnvironment(env);
        runner->setWorkingDirectory(workDir);
        outputPanel_->appendTerminalOutput(
            tr("--- Running in %1: %2 ---\n")
                .arg(terminalLabel, QFileInfo(exePath).absoluteFilePath()));
        startTerminalProgramWatch(exePath);
        if (shellUp) {
            // Session reuse: `started` already fired once, so type the
            // program path directly instead of waiting for the signal.
            runner->write(firstInput.toLocal8Bit() + QByteArray("\r\n"));
        } else {
            // One-shot: once the shell is up, type the program path into it.
            QObject::connect(runner, &QProcess::started, this,
                             [this, runner, firstInput] {
                                 if (runner->state() == QProcess::Running) {
                                     runner->write(firstInput.toLocal8Bit()
                                                   + QByteArray("\r\n"));
                                 }
                             },
                             static_cast<Qt::ConnectionType>(Qt::AutoConnection
                                                             | Qt::SingleShotConnection));
            runner->start(program, args);
        }
        updateRunActionStates();
        return;
    }

    // Non-embedded: the terminal opens its own console window and owns the
    // program's stdio; the IDE captures nothing. There the program path is
    // passed as a command-line argument (nothing can be typed into it from
    // the IDE), with the call operator for PowerShell-style templates. The
    // `cmd /c start` route keeps the window off the IDE's own terminal.
    QStringList externalArgs = args;
    const QString lastToken =
        externalArgs.isEmpty() ? QString() : externalArgs.last();
    if (lastToken.compare(QLatin1String("-Command"), Qt::CaseInsensitive) == 0
        || lastToken.compare(QLatin1String("-c"), Qt::CaseInsensitive) == 0) {
        externalArgs << QStringLiteral("& '%1'").arg(exePath);
    } else {
        externalArgs << exePath;
    }
    if (!startDetachedInNewConsole(program, externalArgs, workDir, env)) {
        outputPanel_->appendProgramOutput(
            tr("Failed to start %1.").arg(QDir::toNativeSeparators(program)));
        return;
    }
    outputPanel_->appendProgramOutput(
        tr("--- Running in %1 (external console): %2 ---")
            .arg(terminalLabel, QFileInfo(exePath).absoluteFilePath()));
}

void MainWindow::updateRunActionStates()
{
    const bool building = compiler_ != nullptr
                          && compiler_->state() != QProcess::NotRunning;
    const bool runnerAlive = runner_ != nullptr
                             && runner_->state() != QProcess::NotRunning;
    const bool terminalAlive = terminalRunner_ != nullptr
                               && terminalRunner_->state()
                                      != QProcess::NotRunning;
    // Terminal modes: the shell (cmd /k, pwsh …) intentionally outlives the
    // program it ran, so what blocks a rerun is the program's own lifetime
    // — watched by the Toolhelp32 poll — not the shell's.
    const bool runningForRerun =
        runnerAlive || (terminalAlive && terminalProgramRunning_);
    const bool running = runnerAlive || terminalAlive;
    const bool debugging = debugger_ != nullptr && debugger_->isRunning();
    if (actCompile_)    actCompile_->setEnabled(!building);
    if (actCompileRun_) actCompileRun_->setEnabled(!building && !runningForRerun);
    if (actRun_)        actRun_->setEnabled(!building && !runningForRerun);
    if (actStopRun_)    actStopRun_->setEnabled(running);
    if (actDebugStart_) actDebugStart_->setEnabled(!debugging && !running);
    if (actDebugStop_)  actDebugStop_->setEnabled(debugging);
    if (actDebugContinue_) actDebugContinue_->setEnabled(debugging);
    if (actDebugStepOver_) actDebugStepOver_->setEnabled(debugging);
    if (actDebugStepInto_) actDebugStepInto_->setEnabled(debugging);
    if (actDebugStepOut_)  actDebugStepOut_->setEnabled(debugging);
}

// ---------------------------------------------------------------------------
// Debugging
// ---------------------------------------------------------------------------

void MainWindow::onToggleBreakpoint()
{
    CodeEditor* ed = currentEditor();
    if (ed == nullptr) {
        return;
    }
    const int line = ed->textCursor().blockNumber();
    const QString path = QDir::fromNativeSeparators(ed->lintPath());
    const QString key = path + QLatin1Char(':') + QString::number(line);
    if (breakpointKeys_.contains(key)) {
        breakpointKeys_.remove(key);
        if (debugger_ != nullptr) {
            debugger_->removeBreakpoint({path, line});
        }
    } else {
        breakpointKeys_.insert(key);
        if (debugger_ != nullptr) {
            debugger_->addBreakpoint({path, line});
        }
    }
    syncBreakpointMarkers();
}

void MainWindow::syncBreakpointMarkers()
{
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i));
        if (ed == nullptr) {
            continue;
        }
        const QString path = QDir::fromNativeSeparators(ed->lintPath());
        const QString prefix = path + QLatin1Char(':');
        QSet<int> lines;
        for (const QString& key : std::as_const(breakpointKeys_)) {
            if (key.startsWith(prefix)) {
                lines.insert(key.mid(prefix.size()).toInt());
            }
        }
        ed->setBreakpointLines(lines);
    }
}

void MainWindow::onStartDebugging()
{
    if (debugger_ == nullptr || debugger_->isRunning()) {
        return;
    }
    const QString exe = lastProjectExe_.isEmpty() ? lastBuildExe_
                                                  : lastProjectExe_;
    if (exe.isEmpty() || !QFileInfo::exists(exe)) {
        outputPanel_->appendDebugOutput(
            tr("Nothing to debug yet — compile first (F7)."));
        outputPanel_->showChannel(OutputPanel::ChannelDebug);
        return;
    }
    const QString debugger = resolveDebuggerPath();
    if (debugger.isEmpty()) {
        outputPanel_->appendDebugOutput(
            tr("No debugger found. Set one in the kit (debugger field) or "
               "install the Windows SDK debuggers (cdb.exe) for MSVC kits."));
        outputPanel_->showChannel(OutputPanel::ChannelDebug);
        return;
    }
    debugger_->setDebuggerPath(debugger);

    QList<LightCpp::App::DebugBreakpoint> breakpoints;
    breakpoints.reserve(breakpointKeys_.size());
    for (const QString& key : std::as_const(breakpointKeys_)) {
        const qsizetype sep = key.lastIndexOf(QLatin1Char(':'));
        if (sep > 0) {
            breakpoints.append({key.left(sep), key.mid(sep + 1).toInt()});
        }
    }
    debugger_->start(exe, QFileInfo(exe).absolutePath(), breakpoints);
    updateRunActionStates();
}

void MainWindow::onStopDebugging()
{
    if (debugger_ != nullptr) {
        debugger_->stop();
    }
    updateRunActionStates();
}

void MainWindow::onDebuggerStopped()
{
    updateRunActionStates();
}

void MainWindow::onDebugPaused(const QString& file, int line)
{
    outputPanel_->appendDebugOutput(
        file.isEmpty()
            ? tr("Target paused.")
            : tr("Target paused at %1:%2").arg(file).arg(line + 1));
    if (!file.isEmpty() && line >= 0) {
        jumpToLocation(file, line, 0);
    }
    // Pause/step states re-enable the stepping actions implicitly — they are
    // enabled for the whole session and the debugger itself ignores stepping
    // while the target runs.
}

QString MainWindow::resolveDebuggerPath() const
{
    // 1) The active kit's debugger field.
    if (const auto kit = activeKit(); kit.has_value()
        && !kit->debugger.isEmpty()) {
        if (QFileInfo::exists(kit->debugger)) {
            return kit->debugger;
        }
    }
    // 2) The active kit's compiler directory (MSYS2: gdb.exe lives next to
    // g++.exe).
    if (const auto kit = activeKit(); kit.has_value()
        && !kit->cppCompiler.isEmpty()) {
        const QString candidate =
            QFileInfo(kit->cppCompiler).absolutePath()
            + QLatin1Char('/') + QStringLiteral("gdb.exe");
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    // 3) MSVC kits: the Windows SDK debuggers (cdb.exe), read from the same
    // KitsRoot10 registry value the MSVC toolchain detection uses. cdb
    // debugs anything VS2012 (toolset v11.0) or newer produces.
    QSettings kitsRoot(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots"),
        QSettings::NativeFormat);
    const QString kitsDir = kitsRoot.value(QStringLiteral("KitsRoot10")).toString();
    if (!kitsDir.isEmpty()) {
        const QString candidate =
            QDir(kitsDir).filePath(QStringLiteral("Debuggers/x64/cdb.exe"));
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

void MainWindow::onMsvcDetected(const LightCpp::App::Msvc::Toolchain& toolchain)
{
    // A build may have waited for the answer and applied it already; the
    // queued copy of the same result then must not log or restart anything
    // a second time.
    if (msvcDetectionApplied_) {
        msvcToolchain_ = toolchain;
        return;
    }
    msvcDetectionApplied_ = true;
    msvcToolchain_ = toolchain;

    // Say what was found (or why nothing was): this runs in the background
    // now, so unlike the old inline detect() the outcome would otherwise
    // never reach the user until a build failed.
    outputPanel_->appendBuildOutput(
        toolchain.isValid()
            ? tr("MSVC toolchain: %1").arg(toolchain.label)
            : tr("No MSVC toolchain: %1").arg(toolchain.problem));

    // clangd needs the MSVC headers to parse against; when the server is
    // already up, applyToolchainHeaders() restarts it, which is exactly the
    // path a late-arriving toolchain needs.
    applyToolchainHeaders();
}

void MainWindow::applyToolchainHeaders()
{
    // Only MSVC needs saying: clangd's own defaults already are the MSYS2 GCC
    // headers, which is what both MinGW toolchains compile against.
    LightCpp::App::ToolchainHeaders headers;
    if (activeKitIsMsvc() && msvcToolchain_.isValid()) {
        headers = LightCpp::App::Msvc::clangdHeaders(msvcToolchain_);
    }
    if (headers == lsp_->toolchainHeaders()) {
        return;  // GCC and Clang both mean "leave clangd alone"
    }
    lsp_->setToolchainHeaders(headers);

    // The libclang engine parses in-process: give it the same include
    // paths as a compile_flags.txt would carry (-target, -isystem, extra
    // flags). Cheap to keep up to date here — libclang re-reads the args
    // on every request.
    if (libClang_ != nullptr) {
        QStringList args;
        if (!headers.target.isEmpty()) {
            args << QStringLiteral("-target") << headers.target;
        }
        for (const QString& dir : headers.includeDirs) {
            args << QStringLiteral("-isystem") << dir;
        }
        args.append(headers.extraFlags);
        libClang_->setCompilerArgs(args);
    }

    // clangd reads its include paths once, when it starts, so a server that is
    // already up keeps answering from the old ones until it is restarted.
    if (!lsp_->isRunning()) {
        return;
    }
    lsp_->stop();
    lsp_->start();
    appendLanguageServerLog(
        headers.isEmpty()
            ? tr("Restarting the language server with its own headers")
            : tr("Restarting the language server for %1").arg(msvcToolchain_.label));
}

// ---------------------------------------------------------------------------
// Code-completion backends
// ---------------------------------------------------------------------------

void MainWindow::requestCompletionFor(CodeEditor* ed, int line, int column,
                                      const QString& triggerChar)
{
    if (ed == nullptr || !clangdCompletionEnabled_) {
        return;
    }
    const QString lintPath = ed->lintPath();
    if (lintPath.isEmpty() || !isCppLikeFile(lintPath)) {
        return;
    }
    // Remember the request so a failure can be retried at the same spot.
    lastCompletion_ = PendingCompletion{ed, lintPath, line, column, triggerChar};
    completionAttempts_ = 0;

    if (completionBackend_ == QStringLiteral("libclang")) {
        if (libClang_ == nullptr || !libClang_->isLoaded()) {
            // Never loaded (path missing): fall through to clangd rather
            // than pretending completion is unavailable.
            appendLanguageServerLog(
                tr("libclang unavailable; completion request went to the "
                   "language server"));
        } else {
            deliverLibClangCompletion();
            return;
        }
    }

    // --- language-server backends (clangd / ccls) -------------------------
    if (!lsp_->isRunning()) {
        // The server is down. One retry budget entry: bring it up and
        // re-issue. If it cannot be started, the bounded retry loop ends
        // with the error in the log — and not with a hung editor. When the
        // user turned syntax check off, respect that and just drop the
        // request (completion has always required the server to be up).
        if (actSyntaxCheck_ != nullptr && actSyntaxCheck_->isChecked()) {
            scheduleCompletionRetry();
        }
        return;
    }
    // Push the current text first: the lint didChange is debounced
    // (300ms), so without this the completion request would arrive while
    // the server still has stale content and return nothing (or results
    // for the wrong prefix).
    lsp_->changeDocument(lintPath, ed->toPlainText());
    const int reqId = lsp_->requestCompletion(lintPath, line, column,
                                              triggerChar);
    if (reqId > 0) {
        latestCompletionId_ = reqId;
        pendingCompletions_[reqId] = ed;
    }
}

void MainWindow::deliverLibClangCompletion()
{
    CodeEditor* ed = lastCompletion_.editor.data();
    if (ed == nullptr || libClang_ == nullptr) {
        return;
    }
    // The parse reads every included header, so it goes to the completer's
    // worker thread; completed() routes the answer back to the popup. Running
    // it inline froze the whole IDE whenever the disk was busy.
    libclangToken_ = ++libclangTokenCounter_;
    libClang_->completeAsync({lastCompletion_.lintPath, ed->toPlainText(),
                               lastCompletion_.line, lastCompletion_.column},
                              libclangToken_);
}

void MainWindow::onLibClangCompleted(
    quint64 token, const QList<LightCpp::App::LspCompletionItem>& items,
    const QString& error)
{
    // A later keystroke already asked again: this answer describes text the
    // caret has moved past, so it must not touch the popup.
    if (token != libclangToken_) {
        return;
    }
    CodeEditor* ed = lastCompletion_.editor.data();
    if (ed == nullptr) {
        return;
    }
    if (items.isEmpty() && !error.isEmpty()) {
        appendLanguageServerLog(error);
        if (scheduleCompletionRetry()) {
            return;  // the retry timer will call back into this
        }
    }
    ed->presentPopup(items);
}

bool MainWindow::scheduleCompletionRetry()
{
    if (completionRetryCount_ <= 0
        || completionAttempts_ >= completionRetryCount_) {
        return false;  // budget spent — let the empty result stand
    }
    ++completionAttempts_;
    // A QPointer, not a raw pointer: if the tab closes within the retry
    // delay a raw pointer would dangle (and never read as nullptr).
    const QPointer<CodeEditor> ed = lastCompletion_.editor.data();
    if (ed == nullptr) {
        return false;
    }
    QTimer::singleShot(400, this, [this, ed] {
        if (ed.isNull()) {
            return;  // tab closed in the meantime
        }
        CodeEditor* editor = ed.data();
        if (completionBackend_ == QStringLiteral("libclang")) {
            deliverLibClangCompletion();
            return;
        }
        if (!lsp_->isRunning()) {
            lsp_->start();
        }
        if (!lsp_->isRunning()) {
            return;  // start() failed again; serverFailed already logged it
        }
        lsp_->changeDocument(lastCompletion_.lintPath, editor->toPlainText());
        const int reqId = lsp_->requestCompletion(
            lastCompletion_.lintPath, lastCompletion_.line,
            lastCompletion_.column, lastCompletion_.triggerChar);
        if (reqId > 0) {
            latestCompletionId_ = reqId;
            pendingCompletions_[reqId] = editor;
        }
    });
    return true;
}

QString MainWindow::resolveCompletionServerPath(const QString& backend) const
{
    // clangd keeps its existing resolution: whatever defaults.clangdPath
    // names (or the built-in fallback the settings store computes).
    const QString clangdPath = settings_->clangdPath();
    if (backend != QStringLiteral("ccls")) {
        return clangdPath;
    }
    const QString exeName = QStringLiteral("ccls.exe");
    QStringList candidates;
    // ccls usually sits in the same toolchain directory as clangd.
    if (!clangdPath.isEmpty()) {
        candidates << QFileInfo(clangdPath).absolutePath()
                          + QLatin1Char('/') + exeName;
    }
    candidates << QCoreApplication::applicationDirPath()
                      + QLatin1Char('/') + exeName;
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    const QString onPath = QStandardPaths::findExecutable(exeName);
    return onPath.isEmpty() ? exeName : onPath;
}

QString MainWindow::resolveLibClangPath() const
{
    QStringList dirs;
    const QString clangdPath = settings_->clangdPath();
    if (!clangdPath.isEmpty()) {
        dirs << QFileInfo(clangdPath).absolutePath();
    }
    dirs << QCoreApplication::applicationDirPath();
    for (const QString& dir : dirs) {
        const QString candidate = dir + QLatin1Char('/') + QStringLiteral("libclang.dll");
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
void MainWindow::loadSettings()
{
    // Compiled-in locations back the settings up: with no config files at
    // all, the IDE behaves exactly as it did before they existed.
    settings_->setFallbackToolPaths(LightCpp::App::kClangdPath,
                                    LightCpp::App::kClangFormatPath,
                                    LightCpp::App::kGitPath);

    // First run: write the starter configuration next to the executable so
    // the settings are discoverable and editable. Files that already exist
    // are never touched, so this is also a self-repair for a deleted file —
    // but never an override of an edited one.
    const QString baseDir =
        LightCpp::Settings::SettingsStore::baseConfigDirectory();
    const auto result =
        LightCpp::App::DefaultSettings::ensureDefaultConfigFiles(baseDir);
    for (const QString& path : result.written) {
        outputPanel_->appendBuildOutput(
            tr("Settings: wrote starter config %1").arg(path));
    }

    settings_->reload();
    for (const QString& error : settings_->lastErrors()) {
        outputPanel_->appendBuildOutput(tr("Settings: %1").arg(error));
    }
}

void MainWindow::applyPreferences()
{
    const LightCpp::Settings::Preferences prefs = settings_->preferences();

    // --- fonts ---------------------------------------------------------
    // The toolbar font is the application font: menus, toolbars and every
    // widget without its own font follow it, while the editor and the
    // output panel override it with their own settings below.
    if (prefs.toolbarFont.isValid()) {
        QFont font(prefs.toolbarFont.family);
        font.setPointSizeF(prefs.toolbarFont.size);
        QApplication::setFont(font);
    } else {
        // "(built-in default)": putting the captured application font back
        // is the only way a previously chosen toolbar font un-applies —
        // QApplication keeps the last setFont otherwise.
        QApplication::setFont(defaultAppFont_);
    }
    if (prefs.editorFont.isValid()) {
        QFont font(prefs.editorFont.family);
        font.setPointSizeF(prefs.editorFont.size);
        font.setStyleHint(QFont::Monospace);
        editorFont_ = font;
        for (int i = 0; i < tabWidget_->count(); ++i) {
            if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
                ed->setEditorFont(font);
            }
        }
    } else {
        // An empty QFont has no explicit settings, so setFont() falls back
        // to the inherited application font — the built-in default. Without
        // this branch the previously chosen editor font stayed until restart.
        editorFont_ = QFont();
        for (int i = 0; i < tabWidget_->count(); ++i) {
            if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
                ed->setEditorFont(editorFont_);
            }
        }
    }
    if (prefs.outputFont.isValid()) {
        QFont font(prefs.outputFont.family);
        font.setPointSizeF(prefs.outputFont.size);
        outputPanel_->setOutputFont(font);
    } else {
        outputPanel_->setOutputFont(QFont());
    }

    // --- files ----------------------------------------------------------
    const auto parsedEncoding =
        LightCpp::Text::fromName(prefs.defaultEncoding.toUtf8());
    defaultEncodingName_ = parsedEncoding.has_value()
        ? QString::fromLatin1(LightCpp::Text::toName(*parsedEncoding))
        : QStringLiteral("UTF-8");
    lineEndingForSaving_ = settings_->lineEndingForSaving();

    // files.longLineMode: "scroll" (one row per line, sideways scroll) or
    // "wrap" (break at the editor edge). Applied to every open buffer, not
    // just new ones, so the setting takes effect without a restart.
    const bool wrapLongLines = prefs.longLineMode == QLatin1String("wrap");
    longLinesWrap_ = wrapLongLines;
    editorsShowWhitespaces_ = prefs.showWhitespaces;
    editorsShowIndentGuides_ = prefs.showIndentGuides;
    for (int i = 0; i < tabWidget_->count(); ++i) {
        if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
            ed->setLongLinesWrap(wrapLongLines);
            ed->setShowWhitespaces(prefs.showWhitespaces);
            ed->setShowIndentGuides(prefs.showIndentGuides);
        }
    }

    // --- performance ------------------------------------------------------
    largeFileHighlightMB_ = prefs.largeFile.syntaxHighlightDisabledAboveMB;
    largeFileIndexMB_ = prefs.largeFile.indexDisabledAboveMB;
    largeFileParseMB_ = prefs.largeFile.parseDisabledAboveMB;

    // --- auto save ---------------------------------------------------------
    if (prefs.autoSave && prefs.autoSaveSeconds > 0) {
        autoSaveTimer_->start(qBound(5, prefs.autoSaveSeconds, 3600) * 1000);
    } else {
        autoSaveTimer_->stop();
    }

    // --- language service: clangd ----------------------------------------
    clangdEnabledByDefault_ = prefs.clangd.enabledByDefault;
    clangdCompletionEnabled_ = prefs.clangd.completion;
    clangdDiagnosticsEnabled_ = prefs.clangd.diagnostics;
    clangdNavigationEnabled_ = prefs.clangd.indexing;
    // A kit's own clangdFallbackStyle wins over the global preference.
    QString clangdStyle = prefs.clangd.fallbackStyle;
    if (const auto kit = activeKit();
        kit.has_value() && !kit->clangdFallbackStyle.isEmpty()) {
        clangdStyle = kit->clangdFallbackStyle;
    }
    lsp_->setFallbackStyle(clangdStyle);
    lsp_->setServerPath(settings_->clangdPath());

    // --- run ----------------------------------------------------------------
    // run.output: "consolePauser" runs the program in its own console via
    // ConsolePauser.exe, a terminal id selects an entry from the editable
    // terminal list, and anything else (and the default) is "program",
    // captured in the embedded Program Output channel. Unknown ids fall
    // back to "program".
    programOutputMode_ = QStringLiteral("program");
    runTerminalName_.clear();
    runTerminalCommand_.clear();
    runTerminalEmbedded_ = true;
    if (prefs.programOutput == QLatin1String("consolePauser")) {
        programOutputMode_ = prefs.programOutput;
    } else if (prefs.programOutput != QLatin1String("program")) {
        for (const auto& terminal : prefs.terminals) {
            if (terminal.id == prefs.programOutput
                && !terminal.command.trimmed().isEmpty()) {
                programOutputMode_ = terminal.id;
                runTerminalName_ = terminal.name;
                runTerminalCommand_ = terminal.command;
                runTerminalEmbedded_ = terminal.embedded;
                break;
            }
        }
    }
    // Only the captured Program Output mode uses the Program channel's
    // stdin field. Embedded terminals got their own channel with its own
    // input row (titled after the selected terminal); Console Pauser and
    // non-embedded terminals (their own console window) capture nothing.
    outputPanel_->setProgramInputVisible(
        programOutputMode_ == QLatin1String("program"));

    // --- language service: completion backend -----------------------------
    // Captured before the new value lands, not after: comparing the setting
    // with itself is how the "backend changed" receipt below never fired.
    const QString previousBackend = completionBackend_;
    completionBackend_ = prefs.completion.backend.isEmpty()
        ? QStringLiteral("clangd") : prefs.completion.backend;
    completionRetryCount_ = qMax(0, prefs.completion.retryCount);
    completionAttempts_ = 0;
    if (completionBackend_ == QStringLiteral("libclang")) {
        if (libClang_ != nullptr && !libClang_->isLoaded()) {
            const QString libPath = resolveLibClangPath();
            if (!libPath.isEmpty()) {
                libClang_->load(libPath);
            } else {
                appendLanguageServerLog(
                    tr("libclang.dll was not found next to clangd or the "
                       "executable; completion stays on clangd"));
                completionBackend_ = QStringLiteral("clangd");
            }
        }
    } else {
        // clangd (the default) or ccls — both are language servers.
        if (completionBackend_ != QStringLiteral("clangd")
            && completionBackend_ != QStringLiteral("ccls")) {
            completionBackend_ = QStringLiteral("clangd");
        }
        const QString serverPath = resolveCompletionServerPath(completionBackend_);
        // Compare BEFORE overwriting: the running process is still the old
        // server, and that difference is what says a restart is needed.
        const bool serverChanged = lsp_->serverPath() != serverPath;
        if (serverChanged) {
            lsp_->setServerPath(serverPath);
        }
        // A language-server backend is useless while the process is down —
        // most visibly when coming back from libclang with syntax check off,
        // which left completion silently answering nothing. Bring it up (and
        // restart it, so the switch takes effect at once when it is already
        // running the previous server).
        if (actSyntaxCheck_ != nullptr && actSyntaxCheck_->isChecked()) {
            if (lsp_->isRunning() && serverChanged) {
                lsp_->stop();
            }
            if (!lsp_->isRunning()) {
                lsp_->start();
            }
        }
        if (serverChanged) {
            appendServerProcessLog(
                tr("Language server: %1")
                    .arg(QDir::toNativeSeparators(serverPath)));
        }
    }

    if (completionBackend_ != previousBackend) {
        appendLanguageServerLog(
            tr("Completion backend: %1").arg(completionBackend_));
    }

    // --- language service: clang-format ----------------------------------
    clangFormatEnabled_ = prefs.clangFormat.enabled;
    formatOnSave_ = prefs.clangFormat.formatOnSave;
    clangFormatFallbackStyle_ = resolvedClangFormatFallbackStyle();
    actFormat_->setEnabled(clangFormatEnabled_);
    actFormatFolder_->setEnabled(clangFormatEnabled_);
    actFormatProject_->setEnabled(clangFormatEnabled_);
    if (formatter_ != nullptr) {
        formatter_->setToolPath(settings_->clangFormatPath());
        formatter_->setFallbackStyle(clangFormatFallbackStyle_);
    }
    if (batchFormatter_ != nullptr) {
        batchFormatter_->setToolPath(settings_->clangFormatPath());
        batchFormatter_->setFallbackStyle(clangFormatFallbackStyle_);
    }

    // --- tool paths --------------------------------------------------------
    git_->setToolPath(resolvedGitPath());

    // --- shortcuts ----------------------------------------------------------
    // An empty setting means "no shortcut" for clangd's toggle (it had none
    // before) and "keep the built-ins" for clang-format (it always had two).
    actSyntaxCheck_->setShortcut(QKeySequence(prefs.shortcutClangd));
    if (prefs.shortcutClangFormat.isEmpty()) {
        actFormat_->setShortcuts({QKeySequence(QStringLiteral("Shift+Alt+F")),
                                  QKeySequence(QStringLiteral("Ctrl+Shift+I"))});
    } else {
        actFormat_->setShortcut(QKeySequence(prefs.shortcutClangFormat));
    }
}

QString MainWindow::resolvedClangFormatFallbackStyle() const
{
    // Precedence: the active kit's style, then the clangFormat service
    // style, then the global formatting style. "file" (or empty) means the
    // built-in default applies — the formatter resolves that itself.
    QString style;
    if (const auto kit = activeKit(); kit.has_value()) {
        style = kit->clangFormatStyle;
    }
    const LightCpp::Settings::Preferences prefs = settings_->preferences();
    if (style.isEmpty()) {
        style = prefs.clangFormat.style;
    }
    if (style.isEmpty()) {
        style = prefs.globalStyle;
    }
    return style;
}

QString MainWindow::resolvedGitPath() const
{
    // The global default first; an active kit's own git.path fills in when
    // no default is configured (a per-kit git is a fallback, not an
    // override — one repository should not change git binaries behind the
    // user's back).
    QString path = settings_->gitPath();
    if (path.isEmpty()) {
        if (const auto kit = activeKit(); kit.has_value()) {
            path = kit->gitPath;
        }
    }
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return LightCpp::App::GitClient::defaultToolPath();
    }
    return path;
}

void MainWindow::onReloadSettings()
{
    settings_->reload();
    applyPreferences();
    syncKitUi();
    applyToolchainHeaders();
    outputPanel_->appendBuildOutput(
        tr("Settings reloaded from %1")
            .arg(QDir::toNativeSeparators(
                LightCpp::Settings::SettingsStore::baseConfigDirectory())));
    for (const QString& error : settings_->lastErrors()) {
        outputPanel_->appendBuildOutput(tr("Settings: %1").arg(error));
    }
}

void MainWindow::onSettings()
{
    LightCpp::App::SettingsDialog dialog(this);
    dialog.setBaseDirectory(
        LightCpp::Settings::SettingsStore::baseConfigDirectory());
    const QString overlay = overlayConfigDirectory();
    if (!overlay.isEmpty()) {
        dialog.setOverlayDirectory(overlay);
    }
    // Apply writes the files but leaves the dialog open — without this the
    // changes would sit on disk inert until the next OK or restart.
    connect(&dialog, &LightCpp::App::SettingsDialog::settingsApplied, this,
            &MainWindow::onReloadSettings);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    // The dialog wrote config files: re-read + re-apply, which is exactly
    // what Reload Settings does (and reports the same way).
    onReloadSettings();
}

void MainWindow::syncSettingsOverlays()
{
    // The workspace's own config directory overlays the executable's —
    // VSCode-style folder settings, but keyed the same way as the base.
    const QString overlay = overlayConfigDirectory();
    QStringList overlayDirs;
    if (!overlay.isEmpty()) {
        overlayDirs.append(overlay);
    }
    settings_->setOverlayDirectories(overlayDirs);
    settings_->reload();
    applyPreferences();
    syncKitUi();
    applyToolchainHeaders();
    if (!overlayDirs.isEmpty()) {
        outputPanel_->appendBuildOutput(
            tr("Settings overlay: %1").arg(overlayDirs.join(QStringLiteral(", "))));
    }
    for (const QString& error : settings_->lastErrors()) {
        outputPanel_->appendBuildOutput(tr("Settings: %1").arg(error));
    }
}

QString MainWindow::overlayConfigDirectory() const
{
    const QString root = projectRootDirectory();
    if (root.isEmpty()) {
        return {};
    }
    return QDir(root).filePath(QStringLiteral("config"));
}

bool MainWindow::fileExceeds(const QString& path, int limitMB) const
{
    if (limitMB <= 0) {
        return false;  // no limit configured
    }
    return QFileInfo(path).size() > qint64(limitMB) * 1024 * 1024;
}

void MainWindow::noteLargeFile(const QString& path, const QString& feature)
{
    // Once per file and feature per session — this runs on hot paths
    // (opening, every debounced lint), so the log must not fill up either.
    const QString key = QFileInfo(path).absoluteFilePath() + QLatin1Char('|')
                        + feature;
    if (largeFileNoted_.contains(key)) {
        return;
    }
    largeFileNoted_.insert(key);
    appendLanguageServerLog(
        tr("Large file: %1 — %2 disabled by settings.")
            .arg(QFileInfo(path).fileName(), feature));
}
