// OutputPanel — switchable output area (clangd log / build log / problems).

#include "LightCpp/App/OutputPanel.hpp"

#include <QColor>
#include <QComboBox>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
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

/// A read-only log view, shared by the two text channels.
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

QString problemsLabel(int errors, int warnings)
{
    QStringList parts;
    if (errors > 0) {
        parts << OutputPanel::tr("%n error(s)", nullptr, errors);
    }
    if (warnings > 0) {
        parts << OutputPanel::tr("%n warning(s)", nullptr, warnings);
    }
    if (parts.isEmpty()) {
        return OutputPanel::tr("Problems");
    }
    return OutputPanel::tr("Problems (%1)").arg(parts.join(QStringLiteral(", ")));
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
    , problems_(new QTreeWidget(this))
{
    // --- header row: title, channel drop-down, clear ----------------------
    QFont headerFont = header_->font();
    headerFont.setBold(true);
    headerFont.setPointSizeF(headerFont.pointSizeF() * 0.9);
    header_->setFont(headerFont);
    header_->setContentsMargins(6, 2, 6, 2);

    // The drop-down switches which view is on screen; each channel keeps its
    // own buffer, so switching never mixes clangd chatter into build output.
    channelCombo_->addItem(tr("Clangd Log"));   // ChannelServer
    channelCombo_->addItem(tr("Build Log"));    // ChannelBuild
    channelCombo_->addItem(tr("Problems"));     // ChannelProblems
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
    stack_->addWidget(serverLog_);   // ChannelServer
    stack_->addWidget(buildLog_);    // ChannelBuild
    stack_->addWidget(problems_);    // ChannelProblems

    problems_->setColumnCount(2);
    problems_->setHeaderLabels({tr("Problem"), tr("Location")});
    problems_->setRootIsDecorated(true);
    problems_->setUniformRowHeights(true);
    problems_->setAllColumnsShowFocus(true);
    problems_->setSelectionMode(QAbstractItemView::SingleSelection);
    problems_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    problems_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    // Single click jumps, like the problems list in other IDEs.
    connect(problems_, &QTreeWidget::itemClicked, this,
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

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(headerRow);
    layout->addWidget(stack_, 1);

    setObjectName(QStringLiteral("OutputPanel"));

    setProblems({});                    // show the empty state
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

void OutputPanel::clearOutput()
{
    switch (stack_->currentIndex()) {
    case ChannelServer: serverLog_->clear(); break;
    case ChannelBuild:  buildLog_->clear(); break;
    default:            setProblems({}); break;
    }
}

// ---------------------------------------------------------------------------
// Problems
// ---------------------------------------------------------------------------
void OutputPanel::setProblems(const QList<ProblemEntry>& problems)
{
    rebuildProblemsTree(problems);

    int errors = 0;
    int warnings = 0;
    for (const ProblemEntry& p : problems) {
        if (p.severity == 1) ++errors;
        else if (p.severity == 2) ++warnings;
    }
    // The counts live in the drop-down label so problems stay visible even
    // while another channel is on screen.
    const QString label = problemsLabel(errors, warnings);
    if (channelCombo_->itemText(ChannelProblems) != label) {
        QSignalBlocker blocker(channelCombo_);
        channelCombo_->setItemText(ChannelProblems, label);
    }
}

void OutputPanel::rebuildProblemsTree(const QList<ProblemEntry>& problems)
{
    problems_->setUpdatesEnabled(false);
    problems_->clear();

    if (problems.isEmpty()) {
        auto* none = new QTreeWidgetItem(problems_);
        none->setText(0, tr("No problems detected."));
        none->setFirstColumnSpanned(true);
        none->setFlags(Qt::NoItemFlags);  // not selectable, not clickable
        problems_->setUpdatesEnabled(true);
        return;
    }

    // Group by file; the incoming list is sorted by path/line, so both the
    // group order and the rows within a group stay stable.
    QHash<QString, QTreeWidgetItem*> groups;
    for (const ProblemEntry& p : problems) {
        QTreeWidgetItem*& group = groups[p.filePath];
        if (group == nullptr) {
            group = new QTreeWidgetItem(problems_);
            group->setText(0, QFileInfo(p.filePath).fileName());
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
        const QString tip = tr("%1 at %2:%3:%4")
                                .arg(severityWord(p.severity), p.filePath)
                                .arg(p.line + 1)
                                .arg(p.column + 1);
        item->setToolTip(0, tip);
        item->setToolTip(1, tip);
        item->setData(0, kRolePath, p.filePath);
        item->setData(0, kRoleLine, p.line);
        item->setData(0, kRoleColumn, p.column);
    }

    problems_->expandAll();
    problems_->setUpdatesEnabled(true);
}
