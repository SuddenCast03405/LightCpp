// MainWindow — IDE shell implementation.

#include "LightCpp/App/MainWindow.hpp"
#include "LightCpp/App/BatchFormatter.hpp"
#include "LightCpp/App/CodeEditor.hpp"
#include "LightCpp/App/CodeFormatter.hpp"
#include "LightCpp/App/DefaultSettings.hpp"
#include "LightCpp/App/FileExplorer.hpp"
#include "LightCpp/App/GitClient.hpp"
#include "LightCpp/App/OutputPanel.hpp"
#include "LightCpp/App/LanguageServerClient.hpp"
#include "LightCpp/App/MsvcToolchain.hpp"
#include "LightCpp/App/SettingsDialog.hpp"
#include "LightCpp/App/ToolchainPaths.hpp"

#include "LightCpp/Text/Encoding.hpp"
#include "LightCpp/Text/TextFile.hpp"

#include "LightCpp/Project/FolderContents.hpp"

#include <algorithm>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QFileDialog>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QUrl>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMetaType>
#include <QMessageBox>
#include <QPair>
#include <QProcess>
#include <QProcessEnvironment>
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
#include <QVBoxLayout>

// Required so QVariant can carry a TextEncoding in QComboBox item data.
Q_DECLARE_METATYPE(LightCpp::Text::TextEncoding)

namespace {

// The tool paths live in ToolchainPaths.hpp so the compiler, the language
// server and the formatter cannot disagree about where the toolchain is.
using LightCpp::App::kClangdPath;
using LightCpp::App::kMingwBin;

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
    //   +---------------------------+
    //   | Explorer | Editors        |   <- horizontal splitter
    //   +---------------------------+
    //   | Output                    |   <- vertical splitter
    //   +---------------------------+
    tabWidget_->setTabsClosable(true);
    tabWidget_->setMovable(true);
    tabWidget_->setDocumentMode(true);

    auto* mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->setChildrenCollapsible(false);  // panels can't be dragged shut
    mainSplitter->addWidget(fileExplorer_);
    mainSplitter->addWidget(tabWidget_);
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
    // The server's own log (stderr + window/logMessage) goes to its own
    // channel, so it never drowns out compiler output.
    connect(lsp_, &LightCpp::App::LanguageServerClient::serverLog,
            outputPanel_, &OutputPanel::appendServerLog);
    connect(lsp_, &LightCpp::App::LanguageServerClient::serverStarted, [this] {
        outputPanel_->appendServerLog(tr("clangd started"));
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
                    ed->setDiagnostics(diags);
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
            outputPanel_->appendServerLog(
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
            outputPanel_->appendServerLog(
                tr("No definition found at that position."));
            return;
        }
        jumpToLocation(location.filePath, location.line, location.column);
    });
    connect(lsp_, &LightCpp::App::LanguageServerClient::serverFailed,
            this, [this](const QString& message) {
        outputPanel_->appendServerLog(tr("language server error: %1").arg(message));
        // Surface the reason instead of leaving the user with a silent pane.
        outputPanel_->showChannel(OutputPanel::ChannelServer);
        actSyntaxCheck_->setChecked(false);
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
    connect(tabWidget_, &QTabWidget::currentChanged, [this](int) {
        if (auto* ed = currentEditor()) {
            statusLanguage_->setText(languageFromSuffix(ed->filePath()));
            statusFile_->setText(ed->filePath().isEmpty()
                                 ? QStringLiteral("Untitled") : ed->filePath());
            updateEncodingCombo(ed);
            onCursorPositionChanged();
        } else {
            statusLanguage_->clear();
            statusFile_->clear();
            statusCursor_->clear();
        }
    });

    resize(1280, 800);
    onNewFile();  // start with an empty editor
    updateProjectActions();  // nothing is open yet, so most of Project is greyed out

    // Git runs on demand: nothing polls the working tree behind the user's
    // back, so a repository changing underneath the IDE costs nothing.
    git_ = new LightCpp::App::GitClient(this);
    connect(git_, &LightCpp::App::GitClient::statusReady, this,
            &MainWindow::onGitStatusReady);
    connect(git_, &LightCpp::App::GitClient::commandFinished, this,
            &MainWindow::onGitCommandFinished);
    syncGitWorkingDirectory();

    // Locate MSVC once. cl.exe needs INCLUDE/LIB built by hand, so this walks
    // the installed toolsets and the Windows SDK up front rather than failing
    // mid-build; a kit that names cl.exe reports the outcome either way.
    msvcToolchain_ = LightCpp::App::Msvc::detect();

    // Load the config (writing the starter files on first run) *before*
    // anything consumes it: fonts, gates and the Kit combo below all read
    // the loaded state, and applyPreferences() also points the language
    // server at the configured clangd.
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

MainWindow::~MainWindow() = default;

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
                    event->ignore();
                    return;
                }
            }
        }
    }
    event->accept();
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
    connect(actToggleExplorer_, &QAction::toggled,
            fileExplorer_, &QWidget::setVisible);

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
    actOpenProject_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
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
    actRunProject_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F5")));
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
}

void MainWindow::createMenuBar()
{
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(actNew_);
    fileMenu->addAction(actOpen_);
    fileMenu->addAction(actOpenFolder_);
    fileMenu->addSeparator();
    fileMenu->addAction(actSave_);
    fileMenu->addAction(actSaveAll_);
    fileMenu->addSeparator();
    fileMenu->addAction(actExit_);

    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(actFormat_);
    editMenu->addAction(actFormatFolder_);
    editMenu->addSeparator();
    auto* foldingMenu = editMenu->addMenu(tr("&Folding"));
    foldingMenu->addAction(actFold_);
    foldingMenu->addAction(actUnfold_);
    foldingMenu->addSeparator();
    foldingMenu->addAction(actFoldAll_);
    foldingMenu->addAction(actUnfoldAll_);
    editMenu->addSeparator();
    editMenu->addAction(actSettings_);

    auto* projectMenu = menuBar()->addMenu(tr("&Project"));
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
    toolsMenu->addAction(actSyntaxCheck_);
    toolsMenu->addAction(actReloadSettings_);
    toolsMenu->addAction(actOpenConfigDir_);
    toolsMenu->addSeparator();
    toolsMenu->addAction(actCompile_);
    toolsMenu->addAction(actCompileRun_);
    toolsMenu->addAction(actRun_);
    toolsMenu->addAction(actStopRun_);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(actToggleExplorer_);
    viewMenu->addAction(actToggleOutput_);

    auto* gitMenu = menuBar()->addMenu(tr("&Git"));
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
    // cl.exe is the MSVC front end; anything else is treated as GNU-style.
    return kit.cppCompiler.endsWith(QLatin1String("cl.exe"),
                                    Qt::CaseInsensitive);
}

bool MainWindow::activeKitIsMsvc() const
{
    const auto kit = activeKit();
    return kit.has_value() && isMsvcKit(*kit);
}

void MainWindow::createStatusBar()
{
    statusCursor_->setText(tr("Ln 1, Col 1"));
    statusLanguage_->setText(tr("C++"));
    statusFile_->setText(tr("Untitled"));

    // Branch / drift / dirty marker. Empty until a repository turns up, and
    // never a place to click: it reports, it does not act.
    statusGit_->setObjectName(QStringLiteral("statusGit"));
    statusGit_->setToolTip(tr("Git branch and working tree state"));

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

    statusBar()->addWidget(statusFile_, 3);
    statusBar()->addPermanentWidget(statusGit_);
    statusBar()->addPermanentWidget(statusLanguage_);
    statusBar()->addPermanentWidget(statusCursor_);
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

    if (QFile f(path); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QByteArray bytes = ed->toUtf8();
        // files.lineEnding: the document works in LF internally; the bytes
        // on disk get the configured ending (CRLF on Windows by default).
        if (lineEndingForSaving_ == QLatin1String("CRLF")) {
            bytes.replace('\n', "\r\n");
        }
        f.write(bytes);
        f.close();
        ed->setModified(false);
        const int idx = tabWidget_->indexOf(ed);
        if (idx >= 0) {
            tabWidget_->setTabText(idx, QFileInfo(path).fileName());
        }
        statusFile_->setText(path);
        statusLanguage_->setText(languageFromSuffix(path));
        outputPanel_->appendBuildOutput(tr("Saved: %1").arg(path));
        // Re-sync the language server: the buffer moved from its virtual
        // (untitled) path to the real location, and the extension may have
        // changed the parse mode.
        const QString oldLint = ed->lintPath();
        if (lsp_->isRunning()) {
            if (!oldLint.isEmpty() && QFileInfo(oldLint).absoluteFilePath()
                                       != QFileInfo(path).absoluteFilePath()) {
                lsp_->closeDocument(oldLint);
            }
            ed->setLintPath(path);
            if (isCppLikeFile(path)) {
                lsp_->openDocument(path, languageIdForFile(path),
                                   ed->toPlainText());
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
    } else {
        QMessageBox::warning(this, tr("Save Error"),
                             tr("Cannot write to %1").arg(path));
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

    connect(editor, &QPlainTextEdit::textChanged, [this, editor]() {
        // Debounce a didChange notification to the language server.
        scheduleLint(editor);
    });

    // Diagnostics flow: clangd -> editor (squiggles) -> problem list. The
    // editor normalises positions before reporting, so the list and the
    // underlines always agree.
    connect(editor, &CodeEditor::diagnosticsChanged,
            this, [this](const QString& path,
                          const QList<LightCpp::App::LspDiagnostic>& diagnostics) {
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

    // Forward manual / trigger-char completion requests from the editor
    // to clangd. The requestId is tracked in pendingCompletions_ so the
    // async response can be routed back to the right popup.
    QPointer<CodeEditor> edPtr(editor);
    connect(editor, &CodeEditor::completionRequested,
            this, [this, edPtr](int line, int column,
                                 const QString& triggerChar) {
        CodeEditor* ed = edPtr;
        if (ed == nullptr || !lsp_->isRunning()) {
            return;
        }
        const QString lintPath = ed->lintPath();
        if (lintPath.isEmpty() || !isCppLikeFile(lintPath)) {
            return;
        }
        // Push the current text first: the lint didChange is debounced
        // (300ms), so without this the completion request would arrive
        // while clangd still has stale content and return nothing (or
        // results for the wrong prefix).
        lsp_->changeDocument(lintPath, ed->toPlainText());
        const int reqId = lsp_->requestCompletion(
            lintPath, line, column, triggerChar);
        if (reqId > 0) {
            latestCompletionId_ = reqId;
            pendingCompletions_[reqId] = edPtr;
        }
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

    setEncodingInfo(editor, toName(content.encoding));
    statusFile_->setText(path);
    statusLanguage_->setText(languageFromSuffix(path));
    outputPanel_->appendBuildOutput(
        tr("Opened: %1  [%2, %3 bytes%4]")
            .arg(path)
            .arg(toName(content.encoding))
            .arg(content.byteSize)
            .arg(content.hadBom ? QStringLiteral(", BOM") : QString()));

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
    outputPanel_->appendBuildOutput(
        tr("Reloaded: %1  [%2]")
            .arg(ed->filePath())
            .arg(toName(encoding)));
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
        outputPanel_->appendServerLog(
            tr("Starting language server: %1").arg(lsp_->serverPath()));
    } else {
        lsp_->stop();
        outputPanel_->appendServerLog(tr("Language server stopped"));
        // Clear all diagnostic markers and any pending completion popups.
        pendingCompletions_.clear();
        pendingDefinitions_.clear();
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
            tr("No CMakeLists.txt or .dev project here — showing the folder "
               "on its own."));
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
    // One dialog for both formats. Which one it is can be told from the name,
    // and asking the user to say it would be asking them to make a distinction
    // the file already makes.
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Project"), QString(),
        tr("Projects (CMakeLists.txt *.dev);;CMake (CMakeLists.txt);;"
           "Dev-C++ (*.dev);;All files (*)"));
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
    devProject_.reset();
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
        if (const QString newest = LightCpp::Project::newestExecutable(tree);
            !newest.isEmpty()) {
            lastProjectExe_ = newest;
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
        devProject_.reset();
        projectKind_ = ProjectKind::CMake;
        projectPath_ = info.absoluteFilePath();
        lastProjectExe_.clear();
        outputPanel_->appendBuildOutput(
            tr("CMake project opened: %1").arg(project.sourceDir));
        outputPanel_->appendBuildOutput(
            tr("Build trees: %1/<Debug|Release> (selected by the toolbar)")
                .arg(LightCpp::Project::buildDirectory(project, buildConfiguration())));
    } else if (info.suffix().compare(QStringLiteral("dev"), Qt::CaseInsensitive) == 0) {
        QString error;
        auto project = LightCpp::Project::loadDevProject(info.absoluteFilePath(), &error);
        if (!project.has_value()) {
            outputPanel_->appendBuildOutput(
                tr("Cannot read %1: %2").arg(path, error));
            return;
        }
        projectPath_ = info.absoluteFilePath();
        lastProjectExe_ = project->outputPath();
        devProject_ = project;
        cmakeProject_.reset();
        projectKind_ = ProjectKind::DevCpp;
        outputPanel_->appendBuildOutput(
            tr("Dev-C++ project opened: %1 [%2, %3 unit(s)]")
                .arg(project->name.isEmpty() ? info.fileName() : project->name,
                     project->targetLabel())
                .arg(project->units.size()));
        outputPanel_->appendBuildOutput(
            tr("Output: %1").arg(lastProjectExe_));
    } else {
        outputPanel_->appendBuildOutput(
            tr("Unrecognised project file: %1\n"
               "Open a CMakeLists.txt or a Dev-C++ .dev file.")
                .arg(info.fileName()));
        return;
    }

    // The Explorer follows the project rather than the descriptor: a CMake
    // project found by climbing lives at its source dir, and showing the file
    // that was named would hide the tree the user is about to work in.
    if (showInExplorer) {
        fileExplorer_->setRootPath(projectRootDirectory());
    }

    setWindowTitle(QStringLiteral("LightC++ — %1").arg(projectDisplayName()));
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
    if (projectKind_ == ProjectKind::DevCpp && devProject_.has_value()) {
        return devProject_->projectDir;
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
    if (devProject_.has_value() && !devProject_->name.isEmpty()) {
        return devProject_->name;
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

    if (projectKind_ == ProjectKind::DevCpp && devProject_.has_value()) {
        const LightCpp::Project::DevCppProject& project = *devProject_;
        if (project.useCustomMakefile) {
            // The kit's make when it names one, else the MSYS2 default.
            QString make = QDir(kMingwBin).filePath(QStringLiteral("mingw32-make.exe"));
            QStringList extraMakeArgs;
            if (const auto kit = activeKit(); kit.has_value()) {
                const QString kitMake =
                    kit->buildTools.value(QStringLiteral("gnuMake"));
                if (!kitMake.isEmpty() && QFileInfo::exists(kitMake)) {
                    make = kitMake;
                }
                extraMakeArgs =
                    settings_
                        ->effectiveBuildConfig(*kit, currentBuildConfigurationName())
                        .buildToolArgs.value(QStringLiteral("gnuMake"));
            }
            return {ProjectStep{
                make, LightCpp::Project::makeArguments(project) + extraMakeArgs,
                project.projectDir,
                tr("Running the project's own makefile: %1")
                    .arg(project.customMakefile)}};
        }
        // The kit's C++ compiler drives the whole unit list, like the
        // MSYS2 g++ did; the Dev-C++ model only supplies the files.
        QString gxx = QDir(kMingwBin).filePath(QStringLiteral("g++.exe"));
        if (const auto kit = activeKit(); kit.has_value()
            && !kit->cppCompiler.isEmpty()
            && !isMsvcKit(*kit)) {
            gxx = kit->cppCompiler;
        }
        return {ProjectStep{
            gxx,
            LightCpp::Project::gnuBuildArguments(project, config, gxx,
                                                 project.outputPath()),
            project.projectDir,
            // The number handed to the compiler, not the number of rows in the
            // file: units can be switched off, and the log should describe what
            // was actually built.
            tr("Compiling %1 unit(s) [%2]")
                .arg(project.buildUnitPaths().size())
                .arg(LightCpp::Project::toString(config)),
        }};
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

    if (projectProcess_ == nullptr) {
        projectProcess_ = new QProcess(this);
        // One stream: CMake writes warnings to stderr and progress to stdout,
        // and interleaving them in the order they happened reads better than
        // two separate halves of the log.
        projectProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(projectProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            if (projectProcess_ == nullptr) return;
            outputPanel_->appendBuildOutput(
                QString::fromLocal8Bit(projectProcess_->readAllStandardOutput()));
        });
        connect(projectProcess_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            if (projectProcess_ == nullptr) return;
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
    outputPanel_->showChannel(OutputPanel::ChannelBuild);
    outputPanel_->appendBuildOutput(
        tr("--- %1 ---").arg(step.label.isEmpty() ? step.program : step.label));
    outputPanel_->appendBuildOutput(
        QStringLiteral("> %1 %2").arg(step.program, step.arguments.join(QLatin1Char(' '))));

    // The toolchain's own bin directory has to be on PATH: cmake looks for the
    // compiler through it, and the binaries it produces need the runtime DLLs
    // that live there too.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"),
               kMingwBin + QDir::listSeparator() + env.value(QStringLiteral("PATH")));
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
            lastProjectExe_ = LightCpp::Project::newestExecutable(
                LightCpp::Project::buildDirectory(*cmakeProject_, buildConfiguration()));
        } else if (devProject_.has_value()) {
            lastProjectExe_ = devProject_->outputPath();
        }
        if (!lastProjectExe_.isEmpty()) {
            outputPanel_->appendBuildOutput(tr("Run Project will start: %1").arg(lastProjectExe_));
        }
    } else {
        outputPanel_->appendBuildOutput(
            tr("--- Project build failed (exit %1) ---").arg(exitCode));
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
    if (projectKind_ == ProjectKind::DevCpp && devProject_.has_value()) {
        return devProject_->projectDir;
    }
    if (CodeEditor* ed = currentEditor(); ed != nullptr && !ed->filePath().isEmpty()) {
        return QFileInfo(ed->filePath()).absolutePath();
    }
    return QDir::currentPath();
}

void MainWindow::syncGitWorkingDirectory()
{
    if (git_ == nullptr) {
        return;
    }
    const QString dir = gitWorkingDirectory();
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
        statusGit_->clear();
        statusGit_->setToolTip(tr("Not a git repository"));
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
        if (QFile f(srcPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(ed->toUtf8());
        } else {
            outputPanel_->showChannel(OutputPanel::ChannelBuild);
            outputPanel_->appendBuildOutput(tr("Cannot write temp file: %1").arg(srcPath));
            runAfterBuild_ = false;
            return;
        }
    } else if (ed->isModified()) {
        if (QFile f(srcPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(ed->toUtf8());
            // setModified(false) emits modificationChanged, which strips
            // the tab's modification dot (see createEditorTab).
            ed->setModified(false);
        } else {
            outputPanel_->showChannel(OutputPanel::ChannelBuild);
            outputPanel_->appendBuildOutput(tr("Cannot save %1 before compile").arg(srcPath));
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
        // built-in table: Debug is /Od alone (no /Zi — this path compiles and
        // runs, it does not debug), Release adds /O2 /DNDEBUG.
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
            outputPanel_->appendBuildOutput(QString::fromLocal8Bit(b));
        });
        connect(compiler_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            const int rc = (status == QProcess::NormalExit) ? code : -1;
            outputPanel_->appendBuildOutput(
                rc == 0 ? tr("--- Build succeeded (exit 0) ---")
                        : tr("--- Build failed (exit %1) ---").arg(rc));
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
    startProgram(exePath, QFileInfo(target.srcPath).absolutePath());
}

void MainWindow::onStopProgram()
{
    if (runner_ == nullptr || runner_->state() == QProcess::NotRunning) {
        return;
    }
    outputPanel_->showChannel(OutputPanel::ChannelProgram);
    outputPanel_->appendProgramOutput(tr("--- Stopping program ---"));
    runner_->kill();
}

void MainWindow::startProgram(const QString& exePath, const QString& workDir)
{
    if (exePath.isEmpty() || !QFileInfo::exists(exePath)) {
        outputPanel_->appendProgramOutput(
            tr("Cannot run: %1 does not exist.").arg(exePath));
        return;
    }
    if (runner_ == nullptr) {
        runner_ = new QProcess(this);
        runner_->setProcessChannelMode(QProcess::MergedChannels);
        connect(runner_, &QProcess::readyReadStandardOutput, this, [this] {
            if (runner_ == nullptr) return;
            const QByteArray b = runner_->readAllStandardOutput();
            outputPanel_->appendProgramOutput(QString::fromLocal8Bit(b));
        });
        // The program has no terminal, so hand it an immediate EOF on stdin:
        // otherwise a scanf()/getline() would block forever with no way to
        // feed it. Must run on started() — the channel isn't open before.
        connect(runner_, &QProcess::started,
                runner_, &QProcess::closeWriteChannel);
        connect(runner_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError e) {
            if (runner_ == nullptr) return;
            if (e == QProcess::FailedToStart) {
                outputPanel_->appendProgramOutput(
                    tr("Failed to start the program. It probably needs the "
                       "MSYS2 runtime DLLs next to it (or on PATH)."));
            }
            updateRunActionStates();
        });
        connect(runner_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            outputPanel_->appendProgramOutput(
                status == QProcess::NormalExit
                    ? tr("--- Program exited with code %1 ---").arg(code)
                    : tr("--- Program was killed ---"));
            updateRunActionStates();
        });
    }

    if (runner_->state() != QProcess::NotRunning) {
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

void MainWindow::updateRunActionStates()
{
    const bool building = compiler_ != nullptr
                          && compiler_->state() != QProcess::NotRunning;
    const bool running = runner_ != nullptr
                         && runner_->state() != QProcess::NotRunning;
    if (actCompile_)    actCompile_->setEnabled(!building);
    if (actCompileRun_) actCompileRun_->setEnabled(!building && !running);
    if (actRun_)        actRun_->setEnabled(!building && !running);
    if (actStopRun_)    actStopRun_->setEnabled(running);
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

    // clangd reads its include paths once, when it starts, so a server that is
    // already up keeps answering from the old ones until it is restarted.
    if (!lsp_->isRunning()) {
        return;
    }
    lsp_->stop();
    lsp_->start();
    outputPanel_->appendServerLog(
        headers.isEmpty()
            ? tr("Restarting the language server with its own headers")
            : tr("Restarting the language server for %1").arg(msvcToolchain_.label));
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
    }
    if (prefs.outputFont.isValid()) {
        QFont font(prefs.outputFont.family);
        font.setPointSizeF(prefs.outputFont.size);
        outputPanel_->setOutputFont(font);
    }

    // --- files ----------------------------------------------------------
    const auto parsedEncoding =
        LightCpp::Text::fromName(prefs.defaultEncoding.toUtf8());
    defaultEncodingName_ = parsedEncoding.has_value()
        ? QString::fromLatin1(LightCpp::Text::toName(*parsedEncoding))
        : QStringLiteral("UTF-8");
    lineEndingForSaving_ = settings_->lineEndingForSaving();

    // --- performance ------------------------------------------------------
    largeFileHighlightMB_ = prefs.largeFile.syntaxHighlightDisabledAboveMB;
    largeFileIndexMB_ = prefs.largeFile.indexDisabledAboveMB;
    largeFileParseMB_ = prefs.largeFile.parseDisabledAboveMB;

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
    outputPanel_->appendServerLog(
        tr("Large file: %1 — %2 disabled by settings.")
            .arg(QFileInfo(path).fileName(), feature));
}
