// DebugViewsPanel — implementation. See the header for the two decisions.

#include "LightCpp/App/DebugViewsPanel.hpp"

#include "LightCpp/App/DebuggerController.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace Debug = LightCpp::App::Debug;

namespace {

QTreeWidgetItem* addRow(QTreeWidget* tree, const QStringList& columns)
{
    auto* item = new QTreeWidgetItem(tree);
    for (int i = 0; i < columns.size() && i < tree->columnCount(); ++i) {
        item->setText(i, columns.at(i));
    }
    return item;
}

QTableWidget* makeTable(const QStringList& headers, QWidget* parent)
{
    auto* table = new QTableWidget(parent);
    table->setColumnCount(static_cast<int>(headers.size()));
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

void fillTable(QTableWidget* table, const QList<QStringList>& rows)
{
    table->setRowCount(0);
    table->setRowCount(static_cast<int>(rows.size()));
    for (int row = 0; row < rows.size(); ++row) {
        const QStringList& values = rows.at(row);
        for (int column = 0; column < values.size() && column < table->columnCount(); ++column) {
            table->setItem(row, column, new QTableWidgetItem(values.at(column)));
        }
    }
}

}  // namespace

DebugViewsPanel::DebugViewsPanel(LightCpp::App::DebuggerController* controller,
                                 LightCpp::App::Debug::DebugViewsModel* model,
                                 LightCpp::App::Debug::BreakpointModel* breakpoints,
                                 QWidget* parent)
    : QWidget(parent), controller_(controller), model_(model), breakpoints_(breakpoints)
{
    buildUi();

    connect(model_, &Debug::DebugViewsModel::callStackChanged,
            this, &DebugViewsPanel::onStackChanged);
    connect(model_, &Debug::DebugViewsModel::threadsChanged,
            this, &DebugViewsPanel::onThreadsChanged);
    connect(model_, &Debug::DebugViewsModel::localsChanged,
            this, &DebugViewsPanel::onLocalsChanged);
    connect(model_, &Debug::DebugViewsModel::registersChanged,
            this, &DebugViewsPanel::onRegistersChanged);
    connect(model_, &Debug::DebugViewsModel::memoryChanged,
            this, &DebugViewsPanel::onMemoryChanged);
    connect(model_, &Debug::DebugViewsModel::disassemblyChanged,
            this, &DebugViewsPanel::onDisassemblyChanged);
    connect(model_, &Debug::DebugViewsModel::watchesChanged,
            this, &DebugViewsPanel::onWatchesChanged);
    connect(model_, &Debug::DebugViewsModel::evaluationFinished,
            this, [this](const QString& expression, const QString& value, bool ok) {
                evaluateOutput_->appendPlainText(
                    ok ? QStringLiteral("%1 = %2").arg(expression, value)
                       : QStringLiteral("%1 = <not available>").arg(expression));
            });
}

void DebugViewsPanel::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->addTab(buildLocalsPage(), tr("Locals"));
    tabs_->addTab(buildWatchPage(), tr("Watch"));
    tabs_->addTab(buildStackPage(), tr("Stack"));
    tabs_->addTab(buildThreadsPage(), tr("Threads"));
    tabs_->addTab(buildRegistersPage(), tr("Registers"));
    tabs_->addTab(buildMemoryPage(), tr("Memory"));
    tabs_->addTab(buildDisassemblyPage(), tr("Code"));
    tabs_->addTab(buildBreakpointsPage(), tr("Breakpoints"));
    layout->addWidget(tabs_, 1);

    status_ = new QLabel(tr("No session."), this);
    status_->setWordWrap(true);
    layout->addWidget(status_);
}

QWidget* DebugViewsPanel::buildLocalsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    localsTree_ = new QTreeWidget(page);
    localsTree_->setColumnCount(3);
    localsTree_->setHeaderLabels({tr("Name"), tr("Value"), tr("Type")});
    localsTree_->setRootIsDecorated(true);
    localsTree_->header()->setStretchLastSection(true);
    layout->addWidget(localsTree_, 1);

    auto* evaluateRow = new QHBoxLayout();
    evaluateInput_ = new QLineEdit(page);
    evaluateInput_->setPlaceholderText(tr("Evaluate expression…"));
    auto* evaluateButton = new QPushButton(tr("Evaluate"), page);
    evaluateRow->addWidget(evaluateInput_, 1);
    evaluateRow->addWidget(evaluateButton);
    layout->addLayout(evaluateRow);

    evaluateOutput_ = new QPlainTextEdit(page);
    evaluateOutput_->setReadOnly(true);
    evaluateOutput_->setMaximumHeight(70);
    layout->addWidget(evaluateOutput_);

    const auto evaluate = [this] {
        const QString expression = evaluateInput_->text().trimmed();
        if (expression.isEmpty()) {
            return;
        }
        model_->evaluate(expression);
        evaluateInput_->clear();
    };
    connect(evaluateButton, &QPushButton::clicked, this, evaluate);
    connect(evaluateInput_, &QLineEdit::returnPressed, this, evaluate);
    return page;
}

QWidget* DebugViewsPanel::buildWatchPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    watchTree_ = new QTreeWidget(page);
    watchTree_->setColumnCount(3);
    watchTree_->setHeaderLabels({tr("Expression"), tr("Value"), tr("Type")});
    watchTree_->setRootIsDecorated(true);
    watchTree_->header()->setStretchLastSection(true);
    connect(watchTree_, &QTreeWidget::itemActivated,
            this, &DebugViewsPanel::onWatchActivated);
    connect(watchTree_, &QTreeWidget::itemClicked,
            this, &DebugViewsPanel::onWatchActivated);
    layout->addWidget(watchTree_, 1);

    auto* row = new QHBoxLayout();
    watchInput_ = new QLineEdit(page);
    watchInput_->setPlaceholderText(tr("Watch expression (e.g. *ptr, vec)"));
    auto* addButton = new QPushButton(tr("Add"), page);
    auto* removeButton = new QPushButton(tr("Remove"), page);
    row->addWidget(watchInput_, 1);
    row->addWidget(addButton);
    row->addWidget(removeButton);
    layout->addLayout(row);

    const auto add = [this] {
        const QString expression = watchInput_->text().trimmed();
        if (expression.isEmpty()) {
            return;
        }
        model_->addWatch(expression);
        watchInput_->clear();
    };
    connect(addButton, &QPushButton::clicked, this, add);
    connect(watchInput_, &QLineEdit::returnPressed, this, add);
    connect(removeButton, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem* item = watchTree_->currentItem();
        if (item == nullptr) {
            return;
        }
        while (item->parent() != nullptr) {
            item = item->parent();
        }
        model_->removeWatch(item->text(0));
    });
    return page;
}

QWidget* DebugViewsPanel::buildStackPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    stackTree_ = new QTreeWidget(page);
    stackTree_->setColumnCount(3);
    stackTree_->setHeaderLabels({tr("Function"), tr("Location"), tr("#")});
    stackTree_->setRootIsDecorated(false);
    stackTree_->header()->setStretchLastSection(false);
    stackTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(stackTree_, &QTreeWidget::itemActivated,
            this, &DebugViewsPanel::onStackActivated);
    connect(stackTree_, &QTreeWidget::itemClicked,
            this, &DebugViewsPanel::onStackActivated);
    layout->addWidget(stackTree_, 1);
    return page;
}

QWidget* DebugViewsPanel::buildThreadsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    threadsTree_ = new QTreeWidget(page);
    threadsTree_->setColumnCount(4);
    threadsTree_->setHeaderLabels({tr("Id"), tr("Name"), tr("State"), tr("Where")});
    threadsTree_->setRootIsDecorated(false);
    threadsTree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    connect(threadsTree_, &QTreeWidget::itemActivated,
            this, &DebugViewsPanel::onThreadActivated);
    layout->addWidget(threadsTree_, 1);
    return page;
}

QWidget* DebugViewsPanel::buildRegistersPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    registersTree_ = new QTreeWidget(page);
    registersTree_->setColumnCount(3);
    registersTree_->setHeaderLabels({tr("Register"), tr("Value"), tr("#")});
    registersTree_->setRootIsDecorated(false);
    registersTree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(registersTree_, 1);
    return page;
}

QWidget* DebugViewsPanel::buildMemoryPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* row = new QHBoxLayout();
    memoryAddress_ = new QLineEdit(page);
    memoryAddress_->setPlaceholderText(tr("Address or expression, e.g. $rsp"));
    memoryLength_ = new QSpinBox(page);
    memoryLength_->setRange(16, 4096);
    memoryLength_->setSingleStep(16);
    memoryLength_->setValue(64);
    memoryLength_->setSuffix(tr(" bytes"));
    auto* readButton = new QPushButton(tr("Read"), page);
    followStackButton_ = new QPushButton(tr("At stack pointer"), page);
    row->addWidget(memoryAddress_, 1);
    row->addWidget(memoryLength_);
    row->addWidget(readButton);
    row->addWidget(followStackButton_);
    layout->addLayout(row);

    memoryTable_ = makeTable({tr("Address"), tr("Bytes"), tr("ASCII")}, page);
    layout->addWidget(memoryTable_, 1);

    connect(readButton, &QPushButton::clicked, this, &DebugViewsPanel::onReadMemory);
    connect(memoryAddress_, &QLineEdit::returnPressed, this, &DebugViewsPanel::onReadMemory);
    connect(followStackButton_, &QPushButton::clicked,
            this, &DebugViewsPanel::onFollowStackPointer);
    return page;
}

QWidget* DebugViewsPanel::buildDisassemblyPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    disassemblyTable_ = makeTable({tr("Address"), tr("Function"), tr("Instruction")}, page);
    layout->addWidget(disassemblyTable_, 1);

    auto* row = new QHBoxLayout();
    auto* atPc = new QPushButton(tr("At program counter"), page);
    auto* atFunction = new QPushButton(tr("At current function"), page);
    row->addWidget(atPc);
    row->addWidget(atFunction);
    row->addStretch(1);
    layout->addLayout(row);

    connect(atPc, &QPushButton::clicked, this, [this] {
        // $pc is spelled the same by gdb and lldb; cdb's equivalent is @eip,
        // which the model's evaluate path does not need — requestMemory
        // accepts any expression the backend understands.
        model_->requestMemory(QStringLiteral("$pc"), 64);
        model_->requestDisassembly(QStringLiteral("$pc"), 24);
        tabs_->setCurrentIndex(6);
    });
    connect(atFunction, &QPushButton::clicked, this, [this] {
        if (!model_->frames().isEmpty()) {
            const Debug::FrameInfo& frame = model_->frames().first();
            if (!frame.address.isEmpty()) {
                model_->requestDisassembly(frame.address, 24);
                tabs_->setCurrentIndex(6);
            }
        }
    });
    return page;
}

QWidget* DebugViewsPanel::buildBreakpointsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    breakpointTree_ = new QTreeWidget(page);
    breakpointTree_->setColumnCount(2);
    breakpointTree_->setHeaderLabels({tr("Breakpoint"), tr("State")});
    breakpointTree_->setRootIsDecorated(false);
    breakpointTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(breakpointTree_, &QTreeWidget::itemActivated,
            this, &DebugViewsPanel::onBreakpointActivated);
    layout->addWidget(breakpointTree_, 1);

    auto* row = new QHBoxLayout();
    auto* addButton = new QPushButton(tr("New…"), page);
    auto* editButton = new QPushButton(tr("Edit…"), page);
    auto* deleteButton = new QPushButton(tr("Delete"), page);
    row->addWidget(addButton);
    row->addWidget(editButton);
    row->addWidget(deleteButton);
    row->addStretch(1);
    layout->addLayout(row);

    connect(addButton, &QPushButton::clicked, this, &DebugViewsPanel::onNewBreakpoint);
    connect(editButton, &QPushButton::clicked, this, &DebugViewsPanel::onEditBreakpoint);
    connect(deleteButton, &QPushButton::clicked, this, &DebugViewsPanel::onDeleteBreakpoint);
    return page;
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

void DebugViewsPanel::sessionStarted()
{
    model_->clear();
    evaluateOutput_->clear();
    status_->setText(tr("Session running."));
    refreshBreakpoints();
}

void DebugViewsPanel::sessionStoppedAt(const QString& file, int line)
{
    model_->refreshOnStop();
    status_->setText(file.isEmpty()
                         ? tr("Paused.")
                         : tr("Paused at %1:%2").arg(file).arg(line + 1));
    refreshBreakpoints();
}

void DebugViewsPanel::sessionEnded()
{
    model_->clear();
    status_->setText(tr("Session ended."));
}

void DebugViewsPanel::refreshBreakpoints()
{
    breakpointTree_->clear();
    breakpointIds_.clear();
    for (const Debug::Breakpoint& bp : breakpoints_->items()) {
        const QString state = bp.backendNumber > 0
                                  ? tr("armed #%1").arg(bp.backendNumber)
                                  : (bp.enabled ? tr("pending") : tr("disabled"));
        auto* item = addRow(breakpointTree_, {bp.label(), state});
        item->setData(0, Qt::UserRole, bp.id);
        breakpointIds_ << bp.id;
    }
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

void DebugViewsPanel::onStackChanged()
{
    stackTree_->clear();
    stackFiles_.clear();
    stackLines_.clear();
    for (const Debug::FrameInfo& frame : model_->frames()) {
        QString where = frame.file.isEmpty()
                            ? frame.address
                            : QStringLiteral("%1:%2").arg(frame.file).arg(frame.line);
        auto* item = addRow(stackTree_, {frame.function.isEmpty()
                                             ? QStringLiteral("??")
                                             : frame.function,
                                         where, QString::number(frame.level)});
        item->setData(0, Qt::UserRole, frame.level);
        stackFiles_ << frame.file;
        stackLines_ << frame.line;
    }
    if (!stackTree_->topLevelItemCount() && status_ != nullptr) {
        status_->setText(tr("No frames (the target is not stopped)."));
    }
}

void DebugViewsPanel::onThreadsChanged()
{
    threadsTree_->clear();
    for (const Debug::ThreadInfo& thread : model_->threads()) {
        auto* item = addRow(threadsTree_,
                            {QString::number(thread.id), thread.name, thread.state,
                             thread.frameFullName});
        item->setData(0, Qt::UserRole, thread.id);
        if (thread.isCurrent) {
            QFont font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);
        }
    }
}

void DebugViewsPanel::onLocalsChanged()
{
    localsTree_->clear();
    for (const Debug::VariableInfo& variable : model_->locals()) {
        auto* item = addRow(localsTree_, {variable.name, variable.value, variable.type});
        if (variable.hasChildren) {
            // A placeholder row makes the expander visible without parsing
            // children the debugger has not been asked for yet.
            new QTreeWidgetItem(item, {QStringLiteral("…")});
        }
    }
    if (!localsTree_->topLevelItemCount()) {
        new QTreeWidgetItem(localsTree_, {tr("(no locals in this frame)")});
    }
}

void DebugViewsPanel::onRegistersChanged()
{
    registersTree_->clear();
    for (const Debug::RegisterInfo& reg : model_->registers()) {
        addRow(registersTree_, {reg.name.isEmpty() ? QStringLiteral("#%1").arg(reg.number)
                                                  : reg.name,
                                reg.value,
                                reg.number >= 0 ? QString::number(reg.number) : QString()});
    }
}

void DebugViewsPanel::onMemoryChanged()
{
    QList<QStringList> rows;
    for (const Debug::MemoryRow& row : model_->memory()) {
        QString bytes;
        QString ascii;
        for (int i = 0; i < row.bytes.size(); ++i) {
            const auto value = static_cast<unsigned char>(row.bytes.at(i));
            bytes += QStringLiteral("%1 ").arg(value, 2, 16, QLatin1Char('0'));
            ascii += (value >= 0x20 && value < 0x7f) ? QChar(value) : QLatin1Char('.');
            if ((i + 1) % 8 == 0) {
                bytes += QLatin1Char(' ');
            }
        }
        rows << QStringList{row.address, bytes.trimmed(), ascii};
    }
    fillTable(memoryTable_, rows);
}

void DebugViewsPanel::onDisassemblyChanged()
{
    QList<QStringList> rows;
    for (const Debug::DisassemblyLine& line : model_->disassembly()) {
        rows << QStringList{line.address, line.function, line.instruction};
    }
    fillTable(disassemblyTable_, rows);
}

void DebugViewsPanel::onWatchesChanged()
{
    watchTree_->clear();
    for (const Debug::WatchItem& watch : model_->watches()) {
        auto* item = addRow(watchTree_,
                            {watch.expression,
                             watch.valid ? watch.value : QStringLiteral("<not available>"),
                             watch.type});
        item->setData(0, Qt::UserRole, watch.expression);
        if (watch.hasChildren) {
            for (const Debug::VariableInfo& child : watch.children) {
                auto* childItem = new QTreeWidgetItem(item,
                                                      {child.name, child.value, child.type});
                childItem->setData(0, Qt::UserRole, watch.expression);
            }
            if (watch.children.isEmpty()) {
                new QTreeWidgetItem(item, {QStringLiteral("…")});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// User actions
// ---------------------------------------------------------------------------

void DebugViewsPanel::onStackActivated(QTreeWidgetItem* item, int)
{
    if (item == nullptr) {
        return;
    }
    const int level = item->data(0, Qt::UserRole).toInt();
    model_->selectFrame(level);
    const qsizetype index = stackTree_->indexOfTopLevelItem(item);
    if (index >= 0 && index < stackFiles_.size() && !stackFiles_.at(index).isEmpty()) {
        emit locationActivated(stackFiles_.at(index), stackLines_.at(index) - 1);
    }
}

void DebugViewsPanel::onThreadActivated(QTreeWidgetItem* item, int)
{
    if (item == nullptr) {
        return;
    }
    model_->selectThread(item->data(0, Qt::UserRole).toInt());
}

void DebugViewsPanel::onWatchActivated(QTreeWidgetItem* item, int)
{
    if (item == nullptr) {
        return;
    }
    const QString expression = item->data(0, Qt::UserRole).toString();
    if (expression.isEmpty()) {
        return;
    }
    // Expanding is a request, not a state: the answer fills the children and
    // onWatchesChanged() rebuilds the row with them.
    model_->expandVariable(expression, item->text(0));
}

void DebugViewsPanel::onBreakpointActivated(QTreeWidgetItem* item, int)
{
    if (item == nullptr) {
        return;
    }
    const int id = item->data(0, Qt::UserRole).toInt();
    const Debug::Breakpoint* bp = breakpoints_->find(id);
    if (bp == nullptr || bp->kind != Debug::BreakpointKind::Line || bp->file.isEmpty()) {
        return;
    }
    emit locationActivated(bp->file, bp->line);
}

void DebugViewsPanel::onReadMemory()
{
    const QString address = memoryAddress_->text().trimmed();
    if (address.isEmpty()) {
        return;
    }
    model_->requestMemory(address, memoryLength_->value());
}

void DebugViewsPanel::onFollowStackPointer()
{
    // The stack pointer's spelling is per backend; asking for it as an
    // expression would need the evaluate round trip, so the model is given
    // the backend's own token.
    const QString sp = controller_ != nullptr
                               && controller_->backend() == LightCpp::App::DebuggerController::Backend::Cdb
                           ? QStringLiteral("esp")
                           : QStringLiteral("$sp");
    memoryAddress_->setText(sp);
    model_->requestMemory(sp, memoryLength_->value());
}

void DebugViewsPanel::onNewBreakpoint()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New breakpoint"));
    auto* form = new QFormLayout(&dialog);

    auto* kind = new QComboBox(&dialog);
    kind->addItem(tr("Line (file:line)"), static_cast<int>(Debug::BreakpointKind::Line));
    kind->addItem(tr("Function"), static_cast<int>(Debug::BreakpointKind::Function));
    kind->addItem(tr("Address"), static_cast<int>(Debug::BreakpointKind::Address));
    kind->addItem(tr("Memory (watch)"), static_cast<int>(Debug::BreakpointKind::Memory));
    auto* location = new QLineEdit(&dialog);
    location->setPlaceholderText(tr("file.cpp:12 / main / 0x401000 / &counter"));
    auto* condition = new QLineEdit(&dialog);
    condition->setPlaceholderText(tr("i > 3 (empty = always)"));
    auto* ignore = new QSpinBox(&dialog);
    ignore->setRange(0, 1000000);
    auto* temporary = new QCheckBox(tr("Temporary (delete after first hit)"), &dialog);
    auto* logpoint = new QCheckBox(tr("Logpoint (print and continue)"), &dialog);
    auto* messageInput = new QLineEdit(&dialog);
    messageInput->setPlaceholderText(tr("Log message, e.g. i = {i}"));
    auto* watchSize = new QSpinBox(&dialog);
    watchSize->setRange(1, 8);
    watchSize->setValue(8);

    form->addRow(tr("Kind"), kind);
    form->addRow(tr("Location"), location);
    form->addRow(tr("Condition"), condition);
    form->addRow(tr("Ignore count"), ignore);
    form->addRow(QString(), temporary);
    form->addRow(QString(), logpoint);
    form->addRow(tr("Log message"), messageInput);
    form->addRow(tr("Watch size"), watchSize);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    Debug::Breakpoint bp;
    bp.kind = static_cast<Debug::BreakpointKind>(kind->currentData().toInt());
    bp.condition = condition->text().trimmed();
    bp.ignoreCount = ignore->value();
    bp.temporary = temporary->isChecked();
    bp.logpoint = logpoint->isChecked();
    const QString text = location->text().trimmed();
    if (bp.kind == Debug::BreakpointKind::Line) {
        const qsizetype colon = text.lastIndexOf(QLatin1Char(':'));
        if (colon <= 1) {
            QMessageBox::warning(this, tr("New breakpoint"),
                                 tr("A line breakpoint needs file:line."));
            return;
        }
        bp.file = text.left(colon);
        bp.line = text.mid(colon + 1).toInt() - 1;
        if (bp.line < 0) {
            bp.line = 0;
        }
    } else {
        bp.expression = text;
        if (bp.kind == Debug::BreakpointKind::Memory) {
            bp.memorySize = watchSize->value();
        }
        if (bp.logpoint && !messageInput->text().trimmed().isEmpty()) {
            // The message rides in `condition` for the logpoint formatter,
            // which keeps the model free of a second text field.
            bp.condition = messageInput->text();
        }
    }
    if (text.isEmpty()) {
        QMessageBox::warning(this, tr("New breakpoint"), tr("A location is required."));
        return;
    }
    breakpoints_->add(bp);
    refreshBreakpoints();
    emit message(tr("Breakpoint added: %1").arg(bp.label()));
    onBreakpointCommanded();
}

void DebugViewsPanel::onEditBreakpoint()
{
    QTreeWidgetItem* item = breakpointTree_->currentItem();
    if (item == nullptr) {
        return;
    }
    const int id = item->data(0, Qt::UserRole).toInt();
    const Debug::Breakpoint* existing = breakpoints_->find(id);
    if (existing == nullptr) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Edit breakpoint"));
    auto* form = new QFormLayout(&dialog);
    auto* location = new QLineEdit(existing->label(), &dialog);
    location->setReadOnly(true);
    auto* condition = new QLineEdit(existing->condition, &dialog);
    auto* ignore = new QSpinBox(&dialog);
    ignore->setRange(0, 1000000);
    ignore->setValue(existing->ignoreCount);
    auto* enabled = new QCheckBox(tr("Enabled"), &dialog);
    enabled->setChecked(existing->enabled);
    auto* temporary = new QCheckBox(tr("Temporary"), &dialog);
    temporary->setChecked(existing->temporary);
    auto* logpoint = new QCheckBox(tr("Logpoint"), &dialog);
    logpoint->setChecked(existing->logpoint);
    form->addRow(tr("Breakpoint"), location);
    form->addRow(tr("Condition"), condition);
    form->addRow(tr("Ignore count"), ignore);
    form->addRow(QString(), enabled);
    form->addRow(QString(), temporary);
    form->addRow(QString(), logpoint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    Debug::Breakpoint fields = *existing;
    fields.condition = condition->text().trimmed();
    fields.ignoreCount = ignore->value();
    fields.enabled = enabled->isChecked();
    fields.temporary = temporary->isChecked();
    fields.logpoint = logpoint->isChecked();
    breakpoints_->update(id, fields);
    refreshBreakpoints();
    onBreakpointCommanded();
}

void DebugViewsPanel::onDeleteBreakpoint()
{
    QTreeWidgetItem* item = breakpointTree_->currentItem();
    if (item == nullptr) {
        return;
    }
    const int id = item->data(0, Qt::UserRole).toInt();
    breakpoints_->remove(id);
    refreshBreakpoints();
    onBreakpointCommanded();
}

void DebugViewsPanel::onBreakpointCommanded()
{
    // The controller owns the live session; re-planting everything is the only
    // way a *changed* breakpoint (a new condition, a new kind) reaches the
    // debugger. It refuses when nothing is running, which is the right no-op:
    // the model is the session file either way.
    if (controller_ == nullptr || !controller_->isRunning()) {
        return;
    }
    controller_->clearAllBreakpoints();
    for (const Debug::Breakpoint& bp : breakpoints_->enabledItems()) {
        controller_->sendCommands(Debug::armCommands(controller_->backend(), bp));
    }
}
