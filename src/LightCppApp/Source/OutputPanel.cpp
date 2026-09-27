// OutputPanel — switchable output area (clangd / build / program / problems).

#include "LightCpp/App/OutputPanel.hpp"

#include <QColor>
#include <QComboBox>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {

/// Item-data roles on the Problems tree rows.
constexpr int kRolePath = Qt::UserRole;
constexpr int kRoleLine = Qt::UserRole + 1;
constexpr int kRoleColumn = Qt::UserRole + 2;

/// A read-only log view, shared by the text channels.
QPlainTextEdit* makeLogView(QWidget* parent)
{
    auto* view = new QPlainTextEdit(parent);
    view->setReadOnly(true);
    view->setMaximumBlockCount(10000);
    view->setFont(QFont(QStringLiteral("Cascadia Code"), 9));
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    return view;
}

QString severityWord(int severity)
{
    switch (severity) {
    case 1:  return QStringLiteral("error");
    case 2:  return QStringLiteral("warning");
    case 3:  return QStringLiteral("info");
    default: return QStringLiteral("hint");
    }
}

/// Same palette as the editor's squiggles: red errors, amber warnings.
QColor severityColor(int severity)
{
    switch (severity) {
    case 1:  return QColor(Qt::red);
    case 2:  return QColor(0xE0, 0xA0, 0x00);
    default: return QColor(0x80, 0x80, 0x80);
    }
}

QString problemsLabel(const QString& base, int errors, int warnings)
{
    QStringList parts;
    if (errors > 0) {
        parts << OutputPanel::tr("%n error(s)", nullptr, errors);
    }
    if (warnings > 0) {
        parts << OutputPanel::tr("%n warning(s)", nullptr, warnings);
    }
    if (parts.isEmpty()) {
        return base;
    }
    return QStringLiteral("%1 (%2)")
        .arg(base, parts.join(QStringLiteral(", ")));
}

}  // namespace

OutputPanel::OutputPanel(QWidget* parent)
    : QWidget(parent)
    , header_(new QLabel(tr("OUTPUT"), this))
    , channelCombo_(new QComboBox(this))
    , clearButton_(new QToolButton(this))
    , stack_(new QStackedWidget(this))
    , serverLog_(makeLogView(this))
    , buildLog_(makeLogView(this))
    , programOutput_(makeLogView(this))
    , gitLog_(makeLogView(this))
    , debugLog_(makeLogView(this))
    , pluginLog_(makeLogView(this))
    , problems_(new QTreeWidget(this))
    , compilerProblems_(new QTreeWidget(this))
{
    // --- header row: title, channel drop-down, clear ----------------------
    QFont headerFont = header_->font();
    headerFont.setBold(true);
    headerFont.setPointSizeF(headerFont.pointSizeF() * 0.9);
    header_->setFont(headerFont);
    header_->setContentsMargins(6, 2, 6, 2);

    // The drop-down switches which view is on screen; each channel keeps its
    // own buffer, so switching never mixes clangd chatter into build output.
    channelCombo_->addItem(tr("Clangd Log"));       // ChannelServer
    channelCombo_->addItem(tr("Build Log"));        // ChannelBuild
    channelCombo_->addItem(tr("Program Output"));   // ChannelProgram
    channelCombo_->addItem(tr("Git"));              // ChannelGit
    channelCombo_->addItem(tr("Problems (LSP Server)"));       // ChannelProblems
    channelCombo_->addItem(tr("Problems (Compiler)"));         // ChannelProblemsCompiler
    channelCombo_->addItem(tr("Debugger"));         // ChannelDebug
    channelCombo_->addItem(tr("Plugins"));          // ChannelPlugins
    channelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    channelCombo_->setToolTip(tr("Choose what the output area shows"));
    connect(channelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) { showChannel(static_cast<Channel>(index)); });

    clearButton_->setText(tr("Clear"));
    clearButton_->setAutoRaise(true);
    clearButton_->setToolTip(tr("Clear the channel on screen"));
    clearButton_->setFocusPolicy(Qt::NoFocus);
    connect(clearButton_, &QToolButton::clicked, this, &OutputPanel::clearOutput);

    auto* headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 4, 0);
    headerRow->setSpacing(4);
    headerRow->addWidget(header_);
    headerRow->addStretch(1);
    headerRow->addWidget(channelCombo_);
    headerRow->addWidget(clearButton_);

    // --- stacked views ---------------------------------------------------
    serverLog_->setPlaceholderText(tr("No language server output yet."));
    buildLog_->setPlaceholderText(tr("No build output yet."));
    programOutput_->setPlaceholderText(tr("No program output yet."));
    gitLog_->setPlaceholderText(tr("No git output yet."));
    debugLog_->setPlaceholderText(
        tr("No debug session yet. Build the program, then Debug > Start Debugging."));
    stack_->addWidget(serverLog_);      // ChannelServer
    stack_->addWidget(buildLog_);       // ChannelBuild

    // The Program and Debugger channels are logs plus a one-line input row:
    // a program that asks for input must be able to receive it, and a live
    // debugger must accept raw console commands. Both pages are built by the
    // same helper; the input starts disabled and the owner toggles it.
    auto* programPage = makeInputLogPage(
        programOutput_, &programInput_,
        tr("Type program input here, press Enter to send (enabled while a "
           "program runs)"),
        [this](const QString& text) { emit programInputSubmitted(text); },
        &programInputRow_);
    stack_->addWidget(programPage);     // ChannelProgram

    stack_->addWidget(gitLog_);         // ChannelGit
    stack_->addWidget(problems_);       // ChannelProblems
    stack_->addWidget(compilerProblems_);  // ChannelProblemsCompiler
    auto* debugPage = makeInputLogPage(
        debugLog_, &debuggerInput_,
        tr("Type a debugger command here, press Enter to send (enabled while "
           "a session runs)"),
        [this](const QString& text) { emit debuggerInputSubmitted(text); });
    stack_->addWidget(debugPage);       // ChannelDebug

    pluginLog_->setPlaceholderText(tr("No plugin output yet."));
    stack_->addWidget(pluginLog_);      // ChannelPlugins

    // Both Problems channels share the same tree shape and click behaviour:
    // single click jumps, like the problem lists in other IDEs.
    for (QTreeWidget* tree : {problems_, compilerProblems_}) {
        tree->setColumnCount(3);
        tree->setHeaderLabels({tr("Problem"), tr("Location"), tr("File")});
        tree->setRootIsDecorated(true);
        tree->setUniformRowHeights(true);
        tree->setAllColumnsShowFocus(true);
        tree->setSelectionMode(QAbstractItemView::SingleSelection);
        tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        connect(tree, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem* item, int) {
            if (item == nullptr) {
                return;
            }
            const QString path = item->data(0, kRolePath).toString();
            if (path.isEmpty()) {
                return;  // placeholder row ("No problems detected.")
            }
            emit problemActivated(path,
                                  item->data(0, kRoleLine).toInt(),
                                  item->data(0, kRoleColumn).toInt());
        });
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(headerRow);
    layout->addWidget(stack_, 1);

    setObjectName(QStringLiteral("OutputPanel"));

    setProblems({});                    // show the empty states
    setCompilerProblems({});
    showChannel(ChannelBuild);          // and start on the build log
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------
void OutputPanel::appendServerLog(const QString& text)
{
    serverLog_->appendPlainText(text);
}

void OutputPanel::appendBuildOutput(const QString& text)
{
    buildLog_->appendPlainText(text);
}

void OutputPanel::appendProgramOutput(const QString& text)
{
    programOutput_->appendPlainText(text);
}

void OutputPanel::appendGitOutput(const QString& text)
{
    gitLog_->appendPlainText(text);
}

void OutputPanel::appendDebugOutput(const QString& text)
{
    debugLog_->appendPlainText(text);
}

void OutputPanel::appendPluginOutput(const QString& text)
{
    pluginLog_->appendPlainText(text);
}

int OutputPanel::openTerminalChannel(const QString& title, bool resetBuffer)
{
    if (terminalLog_ == nullptr) {
        // First terminal session: build the page (log + input row) behind a
        // new drop-down entry at the end of the fixed channels.
        terminalLog_ = makeLogView(this);
        terminalLog_->setPlaceholderText(tr("No terminal session yet."));
        auto* page = makeInputLogPage(
            terminalLog_, &terminalInput_,
            tr("Type a shell command here, press Enter to send (enabled while "
               "the terminal runs)"),
            [this](const QString& text) { emit terminalInputSubmitted(text); },
            &terminalInputRow_);
        terminalChannel_ = stack_->addWidget(page);
        channelCombo_->addItem(title);
    } else {
        // A later session reuses the channel: fresh title, and a fresh
        // buffer unless the caller is reusing a still-live session whose
        // previous output must stay visible.
        if (resetBuffer) {
            terminalLog_->clear();
        }
        if (channelCombo_->itemText(terminalChannel_) != title) {
            channelCombo_->setItemText(terminalChannel_, title);
        }
    }
    if (terminalInput_ != nullptr) {
        terminalInput_->setEnabled(true);
    }
    showChannel(static_cast<Channel>(terminalChannel_));
    return terminalChannel_;
}

void OutputPanel::appendTerminalOutput(const QString& text)
{
    if (terminalLog_ == nullptr) {
        return;
    }
    // Raw appending: insertPlainText at the end keeps an unfinished last
    // line open (a prompt without a newline), so what follows continues on
    // the same line — exactly how a console renders its prompt.
    terminalLog_->moveCursor(QTextCursor::End);
    terminalLog_->insertPlainText(text);
}

void OutputPanel::setTerminalInputEnabled(bool enabled)
{
    if (terminalInput_ != nullptr) {
        terminalInput_->setEnabled(enabled);
    }
}

void OutputPanel::setProgramInputEnabled(bool enabled)
{
    if (programInput_ != nullptr) {
        programInput_->setEnabled(enabled);
    }
}

void OutputPanel::setProgramInputVisible(bool visible)
{
    if (programInputRow_ != nullptr) {
        programInputRow_->setVisible(visible);
    }
}

void OutputPanel::setDebuggerInputEnabled(bool enabled)
{
    if (debuggerInput_ != nullptr) {
        debuggerInput_->setEnabled(enabled);
    }
}

void OutputPanel::showChannel(Channel channel)
{
    const int index = int(channel);
    if (index < 0 || index >= stack_->count()) {
        return;
    }
    stack_->setCurrentIndex(index);
    if (channelCombo_->currentIndex() != index) {
        QSignalBlocker blocker(channelCombo_);
        channelCombo_->setCurrentIndex(index);
    }
}

void OutputPanel::setOutputFont(const QFont& font)
{
    // The four log channels plus the Problems tree — everything the output
    // area renders. An empty family would reset the widgets to the
    // application font, which is why invalid fonts are rejected by the
    // caller (and defensively here).
    if (font.family().isEmpty() || font.pointSizeF() <= 0) {
        return;
    }
    serverLog_->setFont(font);
    buildLog_->setFont(font);
    programOutput_->setFont(font);
    gitLog_->setFont(font);
    debugLog_->setFont(font);
    pluginLog_->setFont(font);
    if (terminalLog_ != nullptr) {
        terminalLog_->setFont(font);
    }
    problems_->setFont(font);
    compilerProblems_->setFont(font);
}

void OutputPanel::clearOutput()
{
    switch (stack_->currentIndex()) {
    case ChannelServer:  serverLog_->clear(); break;
    case ChannelBuild:   buildLog_->clear(); break;
    case ChannelProgram: programOutput_->clear(); break;
    case ChannelGit:     gitLog_->clear(); break;
    case ChannelDebug:   debugLog_->clear(); break;
    case ChannelPlugins: pluginLog_->clear(); break;
    case ChannelProblems:        setProblems({}); break;
    case ChannelProblemsCompiler: setCompilerProblems({}); break;
    default:
        if (stack_->currentIndex() == terminalChannel_) {
            terminalLog_->clear();
        } else {
            setProblems({});
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Problems
// ---------------------------------------------------------------------------
void OutputPanel::setProblems(const QList<ProblemEntry>& problems)
{
    updateProblemsChannel(problems_, ChannelProblems,
                          tr("Problems (LSP Server)"), problems);
}

void OutputPanel::setCompilerProblems(const QList<ProblemEntry>& problems)
{
    updateProblemsChannel(compilerProblems_, ChannelProblemsCompiler,
                          tr("Problems (Compiler)"), problems);
}

void OutputPanel::updateProblemsChannel(QTreeWidget* tree, int comboIndex,
                                        const QString& baseLabel,
                                        const QList<ProblemEntry>& problems)
{
    rebuildProblemsTree(tree, problems);

    int errors = 0;
    int warnings = 0;
    for (const ProblemEntry& p : problems) {
        if (p.severity == 1) ++errors;
        else if (p.severity == 2) ++warnings;
    }
    // The counts live in the drop-down label so problems stay visible even
    // while another channel is on screen.
    const QString label = problemsLabel(baseLabel, errors, warnings);
    if (channelCombo_->itemText(comboIndex) != label) {
        QSignalBlocker blocker(channelCombo_);
        channelCombo_->setItemText(comboIndex, label);
    }
}

void OutputPanel::rebuildProblemsTree(QTreeWidget* tree,
                                      const QList<ProblemEntry>& problems)
{
    tree->setUpdatesEnabled(false);
    tree->clear();

    if (problems.isEmpty()) {
        auto* none = new QTreeWidgetItem(tree);
        none->setText(0, tr("No problems detected."));
        none->setFirstColumnSpanned(true);
        none->setFlags(Qt::NoItemFlags);  // not selectable, not clickable
        tree->setUpdatesEnabled(true);
        return;
    }

    // Group by file; the incoming list is sorted by path/line, so both the
    // group order and the rows within a group stay stable. An empty path is
    // a compiler-driver error with no source location — grouped under
    // "Build", and its rows do not navigate (the click handler ignores an
    // empty kRolePath).
    QHash<QString, QTreeWidgetItem*> groups;
    for (const ProblemEntry& p : problems) {
        QTreeWidgetItem*& group = groups[p.filePath];
        if (group == nullptr) {
            group = new QTreeWidgetItem(tree);
            group->setText(0, p.filePath.isEmpty()
                                  ? tr("Build")
                                  : QFileInfo(p.filePath).fileName());
            group->setToolTip(0, p.filePath);
            QFont bold = group->font(0);
            bold.setBold(true);
            group->setFont(0, bold);
            group->setData(0, kRolePath, p.filePath);
            group->setData(0, kRoleLine, 0);
            group->setData(0, kRoleColumn, 0);
        }

        auto* item = new QTreeWidgetItem(group);
        const QString suffix = p.code.isEmpty()
                ? QString()
                : QStringLiteral(" [%1]").arg(p.code);
        item->setText(0, QStringLiteral("%1: %2%3")
                             .arg(severityWord(p.severity), p.message, suffix));
        item->setForeground(0, severityColor(p.severity));
        item->setText(1, tr("Ln %1, Col %2").arg(p.line + 1).arg(p.column + 1));
        // The file column repeats what the group header says, but a row is
        // what gets copied or screenshotted — give it the full context there.
        item->setText(2, p.filePath.isEmpty() ? QString()
                                              : QFileInfo(p.filePath).fileName());
        const QString tip = tr("%1 at %2:%3:%4")
                                .arg(severityWord(p.severity), p.filePath)
                                .arg(p.line + 1)
                                .arg(p.column + 1);
        item->setToolTip(0, tip);
        item->setToolTip(1, tip);
        item->setToolTip(2, p.filePath);
        item->setData(0, kRolePath, p.filePath);
        item->setData(0, kRoleLine, p.line);
        item->setData(0, kRoleColumn, p.column);
    }

    tree->expandAll();
    tree->setUpdatesEnabled(true);
}

QWidget* OutputPanel::makeInputLogPage(QPlainTextEdit* log, QLineEdit** inputOut,
                                       const QString& placeholder,
                                       const std::function<void(const QString&)>& submit,
                                       QWidget** rowOut)
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(log, 1);

    auto* input = new QLineEdit(page);
    input->setEnabled(false);
    input->setPlaceholderText(placeholder);
    // `submit` is captured by value: the connection outlives this function,
    // and a by-reference capture would dangle the moment we return.
    const auto send = [input, submit] {
        const QString text = input->text();
        input->clear();
        submit(text);
    };
    connect(input, &QLineEdit::returnPressed, page, send);
    auto* sendButton = new QToolButton(page);
    sendButton->setText(tr("Send"));
    sendButton->setAutoRaise(true);
    connect(sendButton, &QToolButton::clicked, page, send);
    auto* sendRow = new QHBoxLayout;
    sendRow->setContentsMargins(4, 2, 4, 2);
    sendRow->setSpacing(4);
    sendRow->addWidget(input, 1);
    sendRow->addWidget(sendButton);
    // The row lives in its own container so the owner can hide the whole
    // thing (Send area is only meaningful for the captured mode).
    auto* rowWidget = new QWidget(page);
    rowWidget->setLayout(sendRow);
    layout->addWidget(rowWidget);

    *inputOut = input;
    if (rowOut != nullptr) {
        *rowOut = rowWidget;
    }
    return page;
}
