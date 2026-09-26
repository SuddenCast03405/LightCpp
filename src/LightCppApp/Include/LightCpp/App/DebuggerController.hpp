// DebuggerController — drives an external debugger over its console
// interface, one process per debug session:
//
//   GDB   — gdb --interpreter=mi2: GDB/MI framed commands (-break-insert,
//           -exec-run, -exec-next ...), *stopped events parsed for the
//           breakpoint's file:line. Any GDB 12+ works.
//   LLDB  — lldb with classic commands (breakpoint set / run / n / s /
//           finish / quit); the (lldb) prompt delimits the output. Any
//           LLDB 15+ works.
//   cdb   — the Windows debuggers console (cdb.exe, ships with the Windows
//           SDK debuggers and with Visual Studio): .lines, bp `file:line`,
//           g / p / t / gu / q. This is what debugs MSVC-built binaries;
//           anything VS2012 (toolset v11.0) or newer produces is fine.
//
// The controller is deliberately thin: it maps menu actions to commands,
// parses just enough output to know "paused here" / "resumed" / "exited",
// and forwards every console line to the log signal. Variable inspection,
// watch windows and friends are future work; until then the raw command
// line writeCommand() keeps the full debugger available to power users.

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace LightCpp::App {

/// One source breakpoint, 0-based line like every editor coordinate here.
struct DebugBreakpoint {
    QString filePath;
    int line = 0;
};

class DebuggerController : public QObject {
    Q_OBJECT
public:
    enum class Backend {
        Unknown,
        Gdb,   ///< GDB/MI (gdb --interpreter=mi2)
        Lldb,  ///< LLDB command line
        Cdb,   ///< cdb.exe (Windows debuggers / MSVC)
    };

    explicit DebuggerController(QObject* parent = nullptr);
    ~DebuggerController() override;

    /// The debugger executable to launch on the next start(). The backend is
    /// derived from the file name (gdb/lldb/cdb).
    void setDebuggerPath(const QString& path);
    [[nodiscard]] QString debuggerPath() const { return debuggerPath_; }
    [[nodiscard]] Backend backend() const { return backend_; }

    /// Human-readable name for logs ("GDB (mi2)", "LLDB", "cdb").
    [[nodiscard]] QString backendName() const;

    /// True when the exe name maps to a supported backend.
    [[nodiscard]] static bool isSupportedDebugger(const QString& exePath);

    /// Launch the debugger on `exePath` with `breakpoints` already planted.
    /// Returns false (and logs why) when the debugger path is missing.
    bool start(const QString& exePath, const QString& workDir,
               const QList<DebugBreakpoint>& breakpoints);

    /// Kill the session: disconnect, kill the process, wait. Safe to call
    /// when nothing is running.
    void stop();

    [[nodiscard]] bool isRunning() const;

    /// Add/remove one breakpoint while the session is live (no-op otherwise).
    void addBreakpoint(const DebugBreakpoint& bp);
    void removeBreakpoint(const DebugBreakpoint& bp);

    // --- execution control ------------------------------------------------
    void cont();      ///< resume after a pause
    void stepOver();  ///< next line, over calls
    void stepInto();  ///< next line, into calls
    void stepOut();   ///< run to the caller

    /// Send a raw console command (GDB/MI when the backend is GDB).
    void writeCommand(const QString& command);

signals:
    /// Every console line the debugger produced (before parsing).
    void logLine(const QString& line);
    /// The debuggee's own stdout, when the backend can separate it.
    void targetOutput(const QString& text);
    /// The session is live and the breakpoints are planted.
    void started();
    /// The debugger process ended.
    void stopped();
    /// The target paused: breakpoint hit or signal. `file` may be empty when
    /// the backend did not report a position; line is 0-based.
    void paused(const QString& file, int line);
    /// The target is executing again.
    void resumed();

private:
    void onReadyRead();
    void handleLine(const QString& line);

    // --- per-backend output parsing ---------------------------------------
    void handleGdbLine(const QString& line);
    void handleLldbLine(const QString& line);
    void handleCdbLine(const QString& line);

    // --- per-backend command building -------------------------------------
    QStringList startCommands(const QList<DebugBreakpoint>& breakpoints) const;
    QString contCommand() const;
    QString stepOverCommand() const;
    QString stepIntoCommand() const;
    QString stepOutCommand() const;
    QString quitCommand() const;
    QString addBreakpointCommand(const DebugBreakpoint& bp) const;
    QString removeBreakpointCommand(const DebugBreakpoint& bp) const;

    /// MI/console quoting for a file:line location.
    QString breakpointLocation(const DebugBreakpoint& bp) const;

    QProcess* process_ = nullptr;
    QString debuggerPath_;
    Backend backend_ = Backend::Unknown;
    QString exePath_;
    bool running_ = false;       // start() succeeded, stop() not yet
    bool paused_ = false;        // target currently stopped
    QStringList pendingCommands_;         // sent once the process is up
    QByteArray outputBuffer_;             // partial line accumulation
    /// cdb prints the breakpoint-hit banner first and the source line
    /// (file:line) several lines later — remember to look for it.
    bool pendingCdbBreakpoint_ = false;
};

}  // namespace LightCpp::App
