// DebugViewsModel — the six inspection views of a debug session.
//
// DebuggerController owns the process and the stepping state machine; this
// class owns *inspection*: call stack, threads, locals/arguments, watches,
// registers, memory and disassembly. It taps the same byte stream, issues its
// own queries, and publishes snapshots the dock widgets render.
//
// Why separate: the controller's job ends at "the target stopped at file:line".
// Everything a user *reads* while stopped is a request/answer pair with its
// own correlation, its own dialect per backend, and its own parsing — mixing
// that into the 900-line controller would make both untestable. Here the
// parser is fed captured transcripts in the test suite, no debugger needed.
//
// Correlation:
//   GDB   — every query carries a unique token; the answer is the `^done`
//           record bearing it. Tokens are allocated by the controller so the
//           numbering cannot collide with its own breakpoint commands.
//   LLDB  — the `(lldb)` prompt ends an answer; lines are accumulated and
//           parsed as a block.
//   cdb   — the `0:000>` prompt does the same.
//
// Refresh policy: on every stop the model re-queries the cheap views (stack,
// threads, locals, registers). Memory and disassembly are user-positioned and
// are only re-read when the user asks or when the session stops somewhere new
// and the view is open — that decision belongs to the dock, not here.

#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include "LightCpp/App/Debug/DebugViews.hpp"
#include "LightCpp/App/Debug/GdbMi.hpp"

#include <functional>

namespace LightCpp::App::Debug {

/// One watch expression and its last answer.
struct WatchItem {
    QString expression;
    QString value;
    QString type;
    bool hasChildren = false;
    QList<VariableInfo> children;
    bool valid = false;   ///< false until the first answer, or after an error
};

class DebugViewsModel : public QObject {
    Q_OBJECT
public:
    explicit DebugViewsModel(QObject* parent = nullptr);

    /// Which dialect the current session speaks.
    void setBackend(Backend backend);
    [[nodiscard]] Backend backend() const noexcept { return backend_; }

    /// How commands leave the model. The controller wires this to
    /// QProcess::write; the tests wire it to a recorder.
    void setCommandSink(std::function<void(const QString&)> sink);
    /// Unique MI token for the next query (GDB only). Defaults to a private
    /// counter starting at 1 for standalone use in tests.
    void setTokenAllocator(std::function<quint64()> allocator);

    /// Feed one decoded line of debugger output.
    void observeLine(const QString& line);

    // --- requests ---------------------------------------------------------
    void requestCallStack(int maxFrames = 40);
    void requestThreads();
    void requestLocals();
    void requestRegisters();
    void requestMemory(const QString& address, int byteCount);
    void requestDisassembly(const QString& address, int instructionCount);
    /// Evaluate `expression` in the current frame (debug.view.evaluate).
    void evaluate(const QString& expression);
    /// Add/replace a watch expression (debug.view.watch).
    void addWatch(const QString& expression);
    void removeWatch(const QString& expression);
    /// Expand one aggregate child level (debug.symbol.stl / smartptr / enum).
    void expandVariable(const QString& rootExpression, const QString& name);
    /// Select a frame: changes which locals evaluate, then re-reads them.
    void selectFrame(int level);
    /// Select a thread.
    void selectThread(int id);
    /// Attach / remote / core: the three non-"run" ways into a session
    /// (debug.attach.*). Returned as the console commands the controller
    /// should send right after the debugger is up.
    [[nodiscard]] QStringList attachCommands(int pid) const;
    [[nodiscard]] QStringList remoteCommands(const QString& endpoint) const;
    [[nodiscard]] QStringList coreCommands(const QString& coreFile) const;

    /// Everything that is cheap to refresh, in one call.
    void refreshOnStop();
    /// Forget every snapshot (session ended).
    void clear();

    // --- snapshots --------------------------------------------------------
    [[nodiscard]] const QList<FrameInfo>& frames() const noexcept { return frames_; }
    [[nodiscard]] const QList<ThreadInfo>& threads() const noexcept { return threads_; }
    [[nodiscard]] const QList<VariableInfo>& locals() const noexcept { return locals_; }
    [[nodiscard]] const QList<RegisterInfo>& registers() const noexcept { return registers_; }
    [[nodiscard]] const QList<MemoryRow>& memory() const noexcept { return memory_; }
    [[nodiscard]] const QList<DisassemblyLine>& disassembly() const noexcept
    {
        return disassembly_;
    }
    [[nodiscard]] const QList<WatchItem>& watches() const noexcept { return watches_; }
    [[nodiscard]] int selectedFrame() const noexcept { return selectedFrame_; }
    [[nodiscard]] int selectedThread() const noexcept { return selectedThread_; }

signals:
    void callStackChanged();
    void threadsChanged();
    void localsChanged();
    void registersChanged();
    void memoryChanged();
    void disassemblyChanged();
    void watchesChanged();
    void evaluationFinished(const QString& expression, const QString& value,
                            bool ok);

private:
    enum class Pending {
        None,
        Stack,
        Threads,
        Locals,
        RegisterNames,
        RegisterValues,
        Memory,
        Disassembly,
        Evaluate,
        WatchCreate,
        WatchChildren,
        FrameSelect,
        ThreadSelect,
    };

    struct PendingQuery {
        Pending kind = Pending::None;
        quint64 token = 0;
        QString argument;   ///< the expression / address the answer belongs to
        QStringList lines;  ///< LLDB and cdb accumulate here
    };

    void send(Pending kind, const QString& command, const QString& argument);
    bool handleTokenAnswer(const MiRecord& record);
    void flushTextAnswer(Pending kind, const QStringList& lines,
                         const QString& argument);
    void finishTextAnswerIfPrompt(const QString& line);

    static QString frameCommand(Backend backend, int maxFrames);
    static QString threadsCommand(Backend backend);
    static QString localsCommand(Backend backend);
    static QString registersCommand(Backend backend, bool names);
    static QString memoryCommand(Backend backend, const QString& address, int count);
    static QString disassemblyCommand(Backend backend, const QString& address, int count);
    static QString evaluateCommand(Backend backend, const QString& expression);
    static QString watchCommand(Backend backend, const QString& expression);
    static QString watchChildrenCommand(Backend backend, const QString& expression);
    static QString selectFrameCommand(Backend backend, int level);
    static QString selectThreadCommand(Backend backend, int id);

    Backend backend_ = Backend::Unknown;
    std::function<void(const QString&)> sink_;
    std::function<quint64()> tokenAllocator_;
    quint64 privateToken_ = 1;

    QList<FrameInfo> frames_;
    QList<ThreadInfo> threads_;
    QList<VariableInfo> locals_;
    QList<RegisterInfo> registers_;
    QList<MemoryRow> memory_;
    QList<DisassemblyLine> disassembly_;
    QList<WatchItem> watches_;

    QStringList registerNames_;
    int selectedFrame_ = 0;
    int selectedThread_ = 0;

    PendingQuery pending_;
    /// LLDB/cdb have no tokens; one answer at a time is enough because the
    /// controller only ever asks for one view at a time.
    QList<PendingQuery> textQueue_;
};

}  // namespace LightCpp::App::Debug
