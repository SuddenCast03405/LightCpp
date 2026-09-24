// MainWindow — IDE shell implementation.

#include "LightCpp/App/MainWindow.hpp"
#include "LightCpp/App/CodeEditor.hpp"
#include "LightCpp/App/CodeFormatter.hpp"
#include "LightCpp/App/FileExplorer.hpp"
#include "LightCpp/App/OutputPanel.hpp"
#include "LightCpp/App/LanguageServerClient.hpp"
#include "LightCpp/App/MsvcToolchain.hpp"
#include "LightCpp/App/ToolchainPaths.hpp"

#include "LightCpp/Text/Encoding.hpp"
#include "LightCpp/Text/TextFile.hpp"

#include <algorithm>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMetaType>
#include <QMessageBox>
#include <QPair>
#include <QProcess>
#include <QProcessEnvironment>
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
    , tabWidget_(new QTabWidget(this))
    , fileExplorer_(new FileExplorer(this))
    , outputPanel_(new OutputPanel(this))
    , encodingCombo_(new QComboBox(this))
    , statusCursor_(new QLabel(this))
    , statusLanguage_(new QLabel(this))
    , statusFile_(new QLabel(this))
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

    // Locate MSVC once. cl.exe needs INCLUDE/LIB built by hand, so this walks
    // the installed toolsets and the Windows SDK up front rather than failing
    // mid-build; the menu entry reports the outcome either way.
    msvcToolchain_ = LightCpp::App::Msvc::detect();
    if (msvcToolchain_.isValid()) {
        actToolchainMsvc_->setStatusTip(
            tr("Use %1").arg(msvcToolchain_.label));
    } else {
        actToolchainMsvc_->setEnabled(false);
        actToolchainMsvc_->setStatusTip(msvcToolchain_.problem);
    }
    // Before the server starts, so it comes up already resolving includes the
    // way the selected toolchain's compiler does.
    applyToolchainHeaders();

    // Syntax checking is on by default — the Tools menu entry stays
    // checkable so it can still be switched off.
    actSyntaxCheck_->setChecked(true);
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
    connect(actOpenFolder_, &QAction::triggered, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Open Folder"));
        if (!dir.isEmpty()) {
            fileExplorer_->setRootPath(dir);
            outputPanel_->appendBuildOutput(tr("Opened folder: %1").arg(dir));
        }
    });

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

    // Toolchain choice: GCC / Clang / MSVC. C/C++ (g++ vs gcc etc.) is chosen
    // automatically from the file extension at compile time.
    auto* toolchainGroup = new QActionGroup(this);
    toolchainGroup->setExclusive(true);
    actToolchainGcc_ = new QAction(tr("&GCC (g++/gcc)"), this);
    actToolchainGcc_->setCheckable(true);
    actToolchainGcc_->setChecked(true);
    actToolchainGcc_->setStatusTip(tr("Use GCC from MSYS2 UCRT64"));
    actToolchainClang_ = new QAction(tr("&Clang (clang++/clang)"), this);
    actToolchainClang_->setCheckable(true);
    actToolchainClang_->setStatusTip(tr("Use Clang from MSYS2 UCRT64"));
    actToolchainMsvc_ = new QAction(tr("&MSVC (cl.exe)"), this);
    actToolchainMsvc_->setCheckable(true);
    toolchainGroup->addAction(actToolchainGcc_);
    toolchainGroup->addAction(actToolchainClang_);
    toolchainGroup->addAction(actToolchainMsvc_);
    // Picking a different compiler also changes which standard-library headers
    // the editor must resolve includes against, so the language server has to
    // be told (and restarted — clangd reads them once, at start-up).
    connect(toolchainGroup, &QActionGroup::triggered,
            this, [this](QAction*) { applyToolchainHeaders(); });

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
    editMenu->addSeparator();
    auto* foldingMenu = editMenu->addMenu(tr("&Folding"));
    foldingMenu->addAction(actFold_);
    foldingMenu->addAction(actUnfold_);
    foldingMenu->addSeparator();
    foldingMenu->addAction(actFoldAll_);
    foldingMenu->addAction(actUnfoldAll_);

    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->addAction(actSyntaxCheck_);
    toolsMenu->addSeparator();
    toolsMenu->addAction(actCompile_);
    toolsMenu->addAction(actCompileRun_);
    toolsMenu->addAction(actRun_);
    toolsMenu->addAction(actStopRun_);
    toolsMenu->addSeparator();
    auto* toolchainMenu = toolsMenu->addMenu(tr("&Toolchain"));
    toolchainMenu->addAction(actToolchainGcc_);
    toolchainMenu->addAction(actToolchainClang_);
    toolchainMenu->addAction(actToolchainMsvc_);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(actToggleExplorer_);
    viewMenu->addAction(actToggleOutput_);

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

    // Everything past the spacer goes to the far right, which is where a
    // build configuration belongs: it is a setting, not an action.
    auto* spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    tb->addWidget(createBuildConfigurationWidget());
}

QWidget* MainWindow::createBuildConfigurationWidget()
{
    auto* container = new QWidget(this);
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 4, 0);
    layout->setSpacing(6);

    layout->addWidget(new QLabel(tr("Configuration:"), container));

    configCombo_ = new QComboBox(container);
    // The names are the CMAKE_BUILD_TYPE spellings, so what the toolbar says
    // is exactly what gets passed to CMake and printed in the build log.
    for (const LightCpp::Project::BuildConfiguration config :
         {LightCpp::Project::BuildConfiguration::Debug,
          LightCpp::Project::BuildConfiguration::Release}) {
        configCombo_->addItem(LightCpp::Project::toString(config),
                              QVariant(int(config)));
    }
    // Debug is the default because that is what a single file was always
    // compiled with; Release stays one click away and never surprises anyone
    // who is mid-debug.
    configCombo_->setCurrentIndex(
        configCombo_->findData(int(LightCpp::Project::BuildConfiguration::Debug)));
    configCombo_->setToolTip(
        tr("Flags used when compiling.\n"
           "Debug adds debug info and leaves optimisation off;\n"
           "Release optimises and defines NDEBUG."));
    connect(configCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        outputPanel_->appendBuildOutput(tr("Build configuration: %1")
                                            .arg(LightCpp::Project::toString(
                                                buildConfiguration())));
    });
    layout->addWidget(configCombo_);
    return container;
}

LightCpp::Project::BuildConfiguration MainWindow::buildConfiguration() const
{
    if (configCombo_ == nullptr) {
        return LightCpp::Project::BuildConfiguration::Debug;
    }
    // Parsed back through the same function CMake settings are read with, so
    // an unexpected label degrades to the default instead of crashing.
    return LightCpp::Project::buildConfigurationFromName(configCombo_->currentText())
        .value_or(LightCpp::Project::BuildConfiguration::Debug);
}

void MainWindow::createStatusBar()
{
    statusCursor_->setText(tr("Ln 1, Col 1"));
    statusLanguage_->setText(tr("C++"));
    statusFile_->setText(tr("Untitled"));

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
        f.write(ed->toUtf8());
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
    connect(editor, &QPlainTextEdit::cursorPositionChanged,
            this, &MainWindow::onCursorPositionChanged);
    connect(editor, &QPlainTextEdit::textChanged, [this, editor]() {
        const int idx = tabWidget_->indexOf(editor);
        if (idx >= 0) {
            QString t = tabWidget_->tabText(idx);
            // Add/remove the modification dot.
            if (editor->isModified() && !t.endsWith(u'\u25CF')) {
                t += QStringLiteral(" \u25CF");
            } else if (!editor->isModified() && t.endsWith(u'\u25CF')) {
                t = t.left(t.length() - 2);
            }
            tabWidget_->setTabText(idx, t);
        }
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
    auto* editor = createEditorTab(QFileInfo(path).fileName());
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

    // Push the freshly opened document to the language server.
    if (lsp_->isRunning() && isCppLikeFile(path)) {
        lsp_->openDocument(path, languageIdForFile(path),
                            QString::fromUtf8(content.utf8));
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
            tr("Starting language server: %1").arg(kClangdPath));
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
            ed->setModified(false);
            const int idx = tabWidget_->indexOf(ed);
            if (idx >= 0) {
                QString t = tabWidget_->tabText(idx);
                if (t.endsWith(u'\u25CF')) t = t.left(t.length() - 2);
                tabWidget_->setTabText(idx, t);
            }
        } else {
            outputPanel_->showChannel(OutputPanel::ChannelBuild);
            outputPanel_->appendBuildOutput(tr("Cannot save %1 before compile").arg(srcPath));
            runAfterBuild_ = false;
            return;
        }
    }

    // Pick the compiler from the selected toolchain + file extension.
    // Headers compile as C++ by default.
    const QString suf = QFileInfo(srcPath).suffix().toLower();
    const bool isC = (suf == "c");
    const QString outExe = target.exePath;

    QString compilerExe;
    QStringList args;
    QStringList toolDirs;  // prepended to the child's PATH
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    if (actToolchainMsvc_->isChecked()) {
        if (!msvcToolchain_.isValid()) {
            outputPanel_->showChannel(OutputPanel::ChannelBuild);
            outputPanel_->appendBuildOutput(
                tr("No MSVC toolchain available: %1").arg(msvcToolchain_.problem));
            runAfterBuild_ = false;
            return;
        }
        compilerExe = msvcToolchain_.clPath;
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
        // Debug is /Od alone, without /Zi: this path is for compiling and
        // running, not for attaching a debugger, and the linker would drop a
        // multi-megabyte .pdb next to the source. Release adds /O2 /DNDEBUG.
        args += LightCpp::Project::flagsFor(buildConfiguration(),
                                            LightCpp::Project::CompilerFamily::Msvc);
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
        const bool useClang = actToolchainClang_->isChecked();
        const QString compilerName = useClang
            ? (isC ? QStringLiteral("clang.exe")    : QStringLiteral("clang++.exe"))
            : (isC ? QStringLiteral("gcc.exe")      : QStringLiteral("g++.exe"));
        compilerExe = QDir(kMingwBin).filePath(compilerName);
        toolDirs << kMingwBin;
        // -g for Debug, -O2 -DNDEBUG for Release; both are in the same place
        // the project builds read their flags from.
        args = {QStringLiteral("-Wall"), QStringLiteral("-Wextra")};
        args += LightCpp::Project::flagsFor(buildConfiguration(),
                                            LightCpp::Project::CompilerFamily::Gnu);
        args << srcPath << QStringLiteral("-o") << outExe;
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
    if (actToolchainMsvc_->isChecked()) {
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
    // libgcc_s, libwinpthread), so its bin directory must be on PATH.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"),
               kMingwBin + QDir::listSeparator() + env.value(QStringLiteral("PATH")));
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
    if (actToolchainMsvc_->isChecked() && msvcToolchain_.isValid()) {
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
