// MainWindow — IDE shell implementation.

#include "LightCpp/App/MainWindow.hpp"
#include "LightCpp/App/CodeEditor.hpp"
#include "LightCpp/App/FileExplorer.hpp"
#include "LightCpp/App/OutputPanel.hpp"
#include "LightCpp/App/LanguageServerClient.hpp"

#include "LightCpp/Text/Encoding.hpp"
#include "LightCpp/Text/TextFile.hpp"

#include <algorithm>

#include <QAction>
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

/// Path to the language server used for syntax checking.
/// TEMPORARY: hardcoded per project phase; will become a setting later.
const QString kClangdPath = QStringLiteral("E:/msys64/ucrt64/bin/clangd.exe");

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

    // --- central tab widget ---
    tabWidget_->setTabsClosable(true);
    tabWidget_->setMovable(true);
    tabWidget_->setDocumentMode(true);
    setCentralWidget(tabWidget_);

    // --- docks ---
    addDockWidget(Qt::LeftDockWidgetArea, fileExplorer_);
    addDockWidget(Qt::BottomDockWidgetArea, outputPanel_);
    fileExplorer_->setMinimumWidth(200);
    fileExplorer_->setMaximumWidth(400);
    outputPanel_->setMaximumHeight(200);

    createActions();
    createMenuBar();
    createToolBar();
    createStatusBar();

    // --- syntax check wiring ---
    lintDebounceTimer_->setSingleShot(true);
    lintDebounceTimer_->setInterval(kLintDebounceMs);
    connect(lintDebounceTimer_, &QTimer::timeout,
            this, &MainWindow::onLintTimerTimeout);
    connect(lsp_, &LightCpp::App::LanguageServerClient::serverStarted, [this] {
        outputPanel_->appendOutput(tr("Syntax check: clangd started"));
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
            outputPanel_->appendOutput(
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
    connect(lsp_, &LightCpp::App::LanguageServerClient::serverFailed,
            this, [this](const QString& message) {
        outputPanel_->appendOutput(tr("Syntax check error: %1").arg(message));
        actSyntaxCheck_->setChecked(false);
    });

    // --- connections ---
    connect(tabWidget_, &QTabWidget::tabCloseRequested,
            this, &MainWindow::onCloseTab);
    connect(fileExplorer_, &FileExplorer::fileActivated,
            this, &MainWindow::onFileActivated);
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
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event)
{
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
            outputPanel_->appendOutput(tr("Opened folder: %1").arg(dir));
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

    actSyntaxCheck_ = new QAction(tr("&Syntax Check"), this);
    actSyntaxCheck_->setCheckable(true);
    actSyntaxCheck_->setStatusTip(tr("Run clangd in the background and underline "
                                      "errors and warnings as you type"));
    connect(actSyntaxCheck_, &QAction::toggled,
            this, &MainWindow::onSyntaxCheckToggled);

    actAbout_ = new QAction(tr("&About LightC++..."), this);
    connect(actAbout_, &QAction::triggered, this, &MainWindow::onAbout);
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

    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->addAction(actSyntaxCheck_);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(fileExplorer_->toggleViewAction());
    viewMenu->addAction(outputPanel_->toggleViewAction());

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
    // decides the parse mode. The file itself never touches the disk.
    ed->setLintPath(QStringLiteral("%1/lightcpp-untitled/%2.cpp")
                        .arg(QDir::tempPath(), title));
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
        outputPanel_->appendOutput(tr("Saved: %1").arg(path));
        // Re-sync the language server: the buffer moved from its virtual
        // (untitled) path to the real location, and the extension may have
        // changed the parse mode.
        if (lsp_->isRunning()) {
            const QString oldLint = ed->lintPath();
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
        outputPanel_->appendOutput(
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
    outputPanel_->appendOutput(
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
        outputPanel_->appendOutput(
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
    outputPanel_->appendOutput(
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
        outputPanel_->appendOutput(
            tr("Starting language server: %1").arg(kClangdPath));
    } else {
        lsp_->stop();
        outputPanel_->appendOutput(tr("Language server stopped"));
        // Clear all diagnostic markers and any pending completion popups.
        pendingCompletions_.clear();
        for (int i = 0; i < tabWidget_->count(); ++i) {
            if (auto* ed = qobject_cast<CodeEditor*>(tabWidget_->widget(i))) {
                ed->setDiagnostics({});
                ed->dismissPopup();
            }
        }
    }
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
