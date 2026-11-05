// DebugViewsPanel — the inspection docks of a debug session.
//
// Seven views in one widget: locals, watches, call stack, threads, registers,
// memory, disassembly — plus the breakpoint list with its property editor.
// The panel owns no protocol: DebugViewsModel issues the commands and
// publishes snapshots, the panel renders them and asks for a re-read when the
// user picks a frame, a thread, or a new memory address.
//
// Two decisions worth stating:
//
//   * The memory and disassembly views are *positioned* by the user, not
//     auto-refreshed on every step. A memory dump that jumps to a new address
//     whenever the code stops is unreadable; the panel keeps the address until
//     asked otherwise, and offers "follow the stack pointer" as an explicit
//     command.
//   * The breakpoint editor edits the model, not the debugger. Applying a
//     change disarms and re-arms through the controller, so the model is
//     always what the debugger has actually been told.

#pragma once

#include <QWidget>

#include "LightCpp/App/Debug/DebugBreakpoints.hpp"
#include "LightCpp/App/Debug/DebugViewsModel.hpp"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace LightCpp::App {
class DebuggerController;
}

class DebugViewsPanel : public QWidget {
    Q_OBJECT
public:
    DebugViewsPanel(LightCpp::App::DebuggerController* controller,
                    LightCpp::App::Debug::DebugViewsModel* model,
                    LightCpp::App::Debug::BreakpointModel* breakpoints,
                    QWidget* parent = nullptr);

    /// The session started: clear the stale snapshots and ask for the first
    /// answers.
    void sessionStarted();
    /// The target stopped: refresh the cheap views.
    void sessionStoppedAt(const QString& file, int line);
    /// The session ended.
    void sessionEnded();

    /// Repaint the breakpoint list (the gutter's F9 path writes the model
    /// directly).
    void refreshBreakpoints();

signals:
    /// The user picked a frame or thread that has a source position.
    void locationActivated(const QString& file, int line);
    /// The user asked for a breakpoint edit that the panel could not apply.
    void message(const QString& text);

private:
    void buildUi();
    QWidget* buildLocalsPage();
    QWidget* buildWatchPage();
    QWidget* buildStackPage();
    QWidget* buildThreadsPage();
    QWidget* buildRegistersPage();
    QWidget* buildMemoryPage();
    QWidget* buildDisassemblyPage();
    QWidget* buildBreakpointsPage();

    void onStackChanged();
    void onThreadsChanged();
    void onLocalsChanged();
    void onRegistersChanged();
    void onMemoryChanged();
    void onDisassemblyChanged();
    void onWatchesChanged();

    void onStackActivated(QTreeWidgetItem* item, int column);
    void onThreadActivated(QTreeWidgetItem* item, int column);
    void onWatchActivated(QTreeWidgetItem* item, int column);
    void onBreakpointActivated(QTreeWidgetItem* item, int column);
    void onBreakpointCommanded();
    void onEditBreakpoint();
    void onDeleteBreakpoint();
    void onNewBreakpoint();
    void onFollowStackPointer();
    void onReadMemory();

    LightCpp::App::DebuggerController* controller_ = nullptr;
    LightCpp::App::Debug::DebugViewsModel* model_ = nullptr;
    LightCpp::App::Debug::BreakpointModel* breakpoints_ = nullptr;

    QTabWidget* tabs_ = nullptr;
    QTreeWidget* localsTree_ = nullptr;
    QTreeWidget* watchTree_ = nullptr;
    QLineEdit* watchInput_ = nullptr;
    QTreeWidget* stackTree_ = nullptr;
    QTreeWidget* threadsTree_ = nullptr;
    QTreeWidget* registersTree_ = nullptr;
    QLineEdit* memoryAddress_ = nullptr;
    QSpinBox* memoryLength_ = nullptr;
    QTableWidget* memoryTable_ = nullptr;
    QTableWidget* disassemblyTable_ = nullptr;
    QTreeWidget* breakpointTree_ = nullptr;
    QPlainTextEdit* evaluateOutput_ = nullptr;
    QLineEdit* evaluateInput_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* followStackButton_ = nullptr;

    /// The (file, line) of each call-stack row, so activating one can jump.
    QStringList stackFiles_;
    QList<int> stackLines_;
    /// Model ids parallel to the breakpoint rows.
    QList<int> breakpointIds_;
};
