// DebuggerController — implementation. See the header for the contract.

#include "LightCpp/App/DebuggerController.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>

namespace {

using LightCpp::App::DebuggerController;
using LightCpp::App::DebugBreakpoint;

/// The file name fragment each backend is recognised by.
DebuggerController::Backend backendForPath(const QString& exePath)
{
    const QString name = QFileInfo(exePath).fileName().toLower();
    if (name.startsWith(QLatin1String("gdb"))) {
        return DebuggerController::Backend::Gdb;
    }
    if (name.startsWith(QLatin1String("lldb")) || name == QLatin1String("lldb-mi")) {
        return DebuggerController::Backend::Lldb;
    }
    if (name == QLatin1String("cdb.exe") || name == QLatin1String("cdb")
        || name.startsWith(QLatin1String("cdb"))) {
        return DebuggerController::Backend::Cdb;
    }
    return DebuggerController::Backend::Unknown;
}

/// MI string values escape backslashes and quotes; this undoes the common
/// subset GDB actually emits for file paths.
QString unescapeMi(const QString& value)
{
    QString out;
    out.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        const QChar ch = value.at(i);
        if (ch != QLatin1Char('\\') || i + 1 >= value.size()) {
            out.append(ch);
            continue;
        }
        const QChar next = value.at(++i);
        switch (next.unicode()) {
        case '\\': out.append(QLatin1Char('\\')); break;
        case '"':  out.append(QLatin1Char('"')); break;
        case 'n':  out.append(QLatin1Char('\n')); break;
        case 't':  out.append(QLatin1Char('\t')); break;
        default:   out.append(QLatin1Char('\\')).append(next); break;
        }
    }
    return out;
}

/// Normalise a debugger-reported path for comparison with editor paths:
/// forward slashes, lowercased drive letter matches whatever QFileInfo does.
QString normalizePath(const QString& path)
{
    QString normalized = QDir::fromNativeSeparators(path);
    // LLDB can report a Windows drive path even when a parser is exercised
    // off-host (for example by a cross-platform test fixture). Normalize that
    // spelling too, without changing legal backslashes in ordinary POSIX paths.
    if (normalized.size() >= 3
        && normalized.at(1) == QLatin1Char(':')
        && normalized.at(2) == QLatin1Char('\\')) {
        normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    }
    return normalized;
}

/// Drops repeated file:line entries, keeping first-seen order. The plant batch
/// and the set remembered for re-arming are both built from this list, so a
/// location the caller listed twice can never be planted twice and re-armed
/// once.
QList<DebugBreakpoint> uniqueLocations(const QList<DebugBreakpoint>& in)
{
    QList<DebugBreakpoint> out;
    out.reserve(in.size());
    QSet<QString> seen;
    for (const DebugBreakpoint& bp : in) {
        const QString key = LightCpp::App::breakpointLocation(bp);
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        out.append(bp);
    }
    return out;
}

}  // namespace

namespace LightCpp::App {

DebuggerController::DebuggerController(QObject* parent) : QObject(parent) {}

DebuggerController::~DebuggerController()
{
    stop();
}

void DebuggerController::setDebuggerPath(const QString& path)
{
    if (debuggerPath_ == path) {
        return;
    }
    debuggerPath_ = path;
    backend_ = backendForPath(path);
}

QString DebuggerController::backendName() const
{
    switch (backend_) {
    case Backend::Gdb:   return QStringLiteral("GDB/MI");
    case Backend::Lldb:  return QStringLiteral("LLDB");
    case Backend::Cdb:   return QStringLiteral("cdb");
    default:             return QStringLiteral("unknown");
    }
}

bool DebuggerController::isSupportedDebugger(const QString& exePath)
{
    return backendForPath(exePath) != Backend::Unknown;
}

bool DebuggerController::start(const QString& exePath, const QString& workDir,
                               const QList<DebugBreakpoint>& breakpoints)
{
    if (isRunning()) {
        return true;  // one session at a time
    }
    if (debuggerPath_.isEmpty()
        || !QFileInfo::exists(debuggerPath_)
        || backend_ == Backend::Unknown) {
        emit logLine(tr("debugger not usable: %1")
                         .arg(QDir::toNativeSeparators(debuggerPath_)));
        return false;
    }
    if (!QFileInfo::exists(exePath)) {
        emit logLine(tr("debug target not found: %1")
                         .arg(QDir::toNativeSeparators(exePath)));
        return false;
    }

    exePath_ = exePath;
    paused_ = false;
    startFailed_ = false;
    const QList<DebugBreakpoint> toPlant = uniqueLocations(breakpoints);
    pendingCommands_ = startCommands(toPlant);

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    // The debugger usually lives in a toolchain directory whose DLLs must be
    // resolvable (MSYS2 gdb needs its siblings).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString debuggerDir = QFileInfo(debuggerPath_).absolutePath();
    env.insert(QStringLiteral("PATH"),
               debuggerDir + QDir::listSeparator() + env.value(QStringLiteral("PATH")));
    process_->setProcessEnvironment(env);
    if (!workDir.isEmpty()) {
        process_->setWorkingDirectory(workDir);
    }

    connect(process_, &QProcess::readyRead,
            this, &DebuggerController::onReadyRead);
    connect(process_, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError error) {
        // A process from an earlier session must never touch this one.
        if (process_ == nullptr || process_ != sender()) {
            return;
        }
        if (error == QProcess::FailedToStart) {
            emit logLine(tr("failed to start debugger: %1").arg(debuggerPath_));
            // Delivered from *inside* start(): tearing the process down here
            // would free process_ while start() is still on the stack (and
            // stop() would then be reached with a null member on the very
            // next line of start()). Record the failure and let start() —
            // which regains control immediately after the signal returns —
            // do the cleanup.
            startFailed_ = true;
            running_ = false;
        }
    });
    connect(process_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (process_ == nullptr || process_ != sender()) {
            return;  // a session that was already replaced or discarded
        }
        process_->deleteLater();
        process_ = nullptr;
        running_ = false;
        paused_ = false;
        emit stopped();
    });

    running_ = true;
    emit logLine(tr("--- %1 debugging %2 ---")
                     .arg(backendName(), QDir::toNativeSeparators(exePath)));

    QStringList arguments;
    switch (backend_) {
    case Backend::Gdb:
        arguments << QStringLiteral("--interpreter=mi2")
                  << QStringLiteral("--quiet") << exePath;
        break;
    case Backend::Lldb:
        arguments << exePath;
        // LLDB's Windows backend needs the debugger process to own a console
        // when it creates the debuggee; launched from a GUI process (no
        // console at all) CreateProcess fails with "The parameter is
        // incorrect". Grant a console but keep its window hidden.
#ifdef Q_OS_WIN
        process_->setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* args) {
                args->flags |= 0x08000000;  // CREATE_NO_WINDOW
            });
#endif
        break;
    case Backend::Cdb:
        // -lines: source-line breakpoints; -y: symbol path from _NT_SYMBOL_PATH
        // (left alone — local builds are debugged without a symbol server).
        arguments << QStringLiteral("-lines") << exePath;
        break;
    default:
        break;
    }

    process_->start(debuggerPath_, arguments);
    if (startFailed_ || process_->state() == QProcess::NotRunning) {
        // Nothing came up. Leaving process_ (and the pending command batch)
        // behind used to poison the next session: the dead QProcess stayed
        // connected and the stale handlers acted on whatever process the
        // next start() installed.
        return abortStart();
    }

    // The initial command batch (breakpoints + run). QProcess buffers writes
    // made before the process actually starts, so sending now is safe.
    for (const QString& command : std::as_const(pendingCommands_)) {
        process_->write((command + QLatin1Char('\n')).toUtf8());
    }
    pendingCommands_.clear();
    // From here on these are the breakpoints the session believes are armed.
    // cdb and LLDB can only clear all of them, so this list is what a single
    // removal re-arms afterwards.
    armed_ = toPlant;
    emit started();
    return true;
}

void DebuggerController::stop()
{
    if (process_ == nullptr) {
        return;
    }
    // Same teardown discipline as GitClient: disconnect our handlers first,
    // then quit/kill synchronously so no lambda can touch a dead pointer.
    const bool wasRunning = running_;
    process_->disconnect(this);
    if (process_->state() != QProcess::NotRunning) {
        process_->write((quitCommand() + QLatin1Char('\n')).toUtf8());
        process_->waitForFinished(2000);
    }
    discardProcess();
    running_ = false;
    paused_ = false;
    armed_.clear();  // the debugger took the breakpoints with it
    emit logLine(tr("--- debug session ended ---"));
    // The finished handler above is disconnected, so a manual stop would
    // otherwise never announce its own end — anything keyed on stopped()
    // (action states, the console input field) would stay live.
    if (wasRunning) {
        emit stopped();
    }
}

void DebuggerController::discardProcess()
{
    if (process_ == nullptr) {
        return;
    }
    process_->disconnect(this);
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(2000);
    }
    process_->deleteLater();
    process_ = nullptr;
}

bool DebuggerController::abortStart()
{
    // start() had already announced the session and armed the process, so
    // both have to be taken back. stopped() is emitted for the same reason
    // the old code did: listeners were told a session was under way.
    discardProcess();
    running_ = false;
    paused_ = false;
    pendingCommands_.clear();
    armed_.clear();  // nothing was planted if the debugger never came up
    emit logLine(tr("debugger did not start"));
    emit stopped();
    return false;
}

bool DebuggerController::isRunning() const
{
    return running_ && process_ != nullptr
           && process_->state() != QProcess::NotRunning;
}

void DebuggerController::addBreakpoint(const DebugBreakpoint& bp)
{
    if (!isRunning()) {
        return;
    }
    const QString key = breakpointLocation(bp);
    for (const DebugBreakpoint& armed : std::as_const(armed_)) {
        if (breakpointLocation(armed) == key) {
            // Already planted. cdb answers "breakpoint N redefined" and LLDB
            // would quietly create a twin numbering, so refuse up front.
            return;
        }
    }
    writeCommand(addBreakpointCommand(backend_, bp));
    armed_.append(bp);
}

void DebuggerController::removeBreakpoint(const DebugBreakpoint& bp)
{
    if (!isRunning()) {
        return;
    }
    const QString key = breakpointLocation(bp);
    for (qsizetype i = 0; i < armed_.size(); ++i) {
        if (breakpointLocation(armed_.at(i)) != key) {
            continue;
        }
        armed_.removeAt(i);
        for (const QString& command :
             removeBreakpointCommands(backend_, bp, armed_)) {
            writeCommand(command);
        }
        return;
    }
    // Not one of ours: there is nothing to take back. Writing a delete anyway
    // is exactly how "remove one breakpoint" used to become "remove them all".
}

void DebuggerController::cont()
{
    if (isRunning() && paused_) {
        writeCommand(contCommand());
        paused_ = false;
        emit resumed();
    }
}

void DebuggerController::stepOver() { if (isRunning() && paused_) { writeCommand(stepOverCommand()); paused_ = false; emit resumed(); } }
void DebuggerController::stepInto() { if (isRunning() && paused_) { writeCommand(stepIntoCommand()); paused_ = false; emit resumed(); } }
void DebuggerController::stepOut()  { if (isRunning() && paused_) { writeCommand(stepOutCommand());  paused_ = false; emit resumed(); } }

void DebuggerController::writeCommand(const QString& command)
{
    if (process_ != nullptr
        && process_->state() != QProcess::NotRunning) {
        process_->write((command + QLatin1Char('\n')).toUtf8());
    }
}

// ---------------------------------------------------------------------------
// Per-backend command dialects
// ---------------------------------------------------------------------------

QString breakpointLocation(const DebugBreakpoint& bp)
{
    // Forward slashes for the Unix-ish backends, native separators for cdb;
    // all of them accept forward slashes, so keep one form.
    return QDir::fromNativeSeparators(bp.filePath) + QLatin1Char(':')
           + QString::number(bp.line + 1);
}

QString addBreakpointCommand(DebuggerController::Backend backend,
                             const DebugBreakpoint& bp)
{
    switch (backend) {
    case DebuggerController::Backend::Gdb:
        return QStringLiteral("-break-insert \"%1\"").arg(breakpointLocation(bp));
    case DebuggerController::Backend::Cdb:
        return QStringLiteral("bp `%1`").arg(breakpointLocation(bp));
    case DebuggerController::Backend::Lldb:
        // LLDB sets by file *name* and line number, not by path:line.
        return QStringLiteral("breakpoint set --file \"%1\" --line %2")
            .arg(QFileInfo(bp.filePath).fileName())
            .arg(bp.line + 1);
    default:
        return {};
    }
}

QStringList removeBreakpointCommands(DebuggerController::Backend backend,
                                     const DebugBreakpoint& target,
                                     const QList<DebugBreakpoint>& stillArmed)
{
    switch (backend) {
    case DebuggerController::Backend::Gdb:
        // GDB's console has delete-by-location, so one command is enough and
        // no other breakpoint is touched.
        return {QStringLiteral("clear %1").arg(breakpointLocation(target))};
    case DebuggerController::Backend::Cdb:
    case DebuggerController::Backend::Lldb: {
        // Neither backend can delete one breakpoint from a location, and
        // neither can be told which id belongs to it:
        //   cdb  — `bc` only takes ids, and a source-line breakpoint's id is
        //          never printed: `bp` says nothing when it succeeds, `bl`
        //          shows the symbol (`msvcprog!main+0x4`) and `.bpcmds` shows
        //          an address, so the location cannot be mapped back to an id.
        //   lldb — `breakpoint delete <file>.<line>` looks like an id but is
        //          not: it answers "0 breakpoints deleted" and leaves the
        //          breakpoint armed, which is what this code used to send.
        // So: clear everything, then re-arm what is still supposed to be
        // there. Getting `stillArmed` wrong here silently drops breakpoints.
        QStringList commands;
        commands << (backend == DebuggerController::Backend::Cdb
                         ? QStringLiteral("bc *")
                         : QStringLiteral("breakpoint delete -f"));
        for (const DebugBreakpoint& bp : stillArmed) {
            commands << addBreakpointCommand(backend, bp);
        }
        return commands;
    }
    default:
        return {};
    }
}

QStringList DebuggerController::startCommands(
    const QList<DebugBreakpoint>& breakpoints) const
{
    QStringList commands;
    switch (backend_) {
    case Backend::Gdb:
        // Negotiate the charset before anything else: the rest of this batch
        // carries file paths as UTF-8 bytes, and GDB otherwise decodes its
        // input in the host locale charset (GBK on zh-CN Windows), so a
        // Chinese path would never match and the breakpoint never plants.
        // With UTF-8 on both sides paths round-trip and the console stream
        // decodes as UTF-8 (see onReadyRead). QProcess queues these lines in
        // order and GDB executes them one at a time, so the setting is in
        // effect for every command below.
        commands << QStringLiteral("-gdb-set charset UTF-8");
        for (const DebugBreakpoint& bp : breakpoints) {
            commands << addBreakpointCommand(Backend::Gdb, bp);
        }
        commands << QStringLiteral("-exec-run");
        break;
    case Backend::Lldb:
        for (const DebugBreakpoint& bp : breakpoints) {
            commands << addBreakpointCommand(Backend::Lldb, bp);
        }
        // "run" would give the debuggee LLDB's own stdio handles, and LLDB's
        // Windows launcher fails with "The parameter is incorrect" when those
        // handles are pipes/files — always the case when launched from a GUI
        // process. Redirecting the debuggee's stdio explicitly (NUL device)
        // avoids the inherited-handle path entirely; the target's own output
        // is not captured per-target then, which matches every other backend
        // showing the console stream in the Debug channel instead.
        commands << QStringLiteral("process launch -i NUL -o NUL -e NUL");
        break;
    case Backend::Cdb:
        // Source-line display must be ON for position reports; the debugger
        // was started with -lines and .lines is a *toggle*, so re-issuing
        // ".lines" would switch it back off — ".lines -e" is idempotent.
        // `bp `file:line`` plants each breakpoint.
        commands << QStringLiteral(".lines -e");
        for (const DebugBreakpoint& bp : breakpoints) {
            commands << addBreakpointCommand(Backend::Cdb, bp);
        }
        commands << QStringLiteral("g");
        break;
    default:
        break;
    }
    return commands;
}

QString DebuggerController::contCommand() const
{
    switch (backend_) {
    case Backend::Gdb:  return QStringLiteral("-exec-continue");
    case Backend::Cdb:  return QStringLiteral("g");
    default:            return QStringLiteral("continue");
    }
}

QString DebuggerController::stepOverCommand() const
{
    switch (backend_) {
    case Backend::Gdb:  return QStringLiteral("-exec-next");
    case Backend::Cdb:  return QStringLiteral("p");
    default:            return QStringLiteral("thread step-over");
    }
}

QString DebuggerController::stepIntoCommand() const
{
    switch (backend_) {
    case Backend::Gdb:  return QStringLiteral("-exec-step");
    case Backend::Cdb:  return QStringLiteral("t");
    default:            return QStringLiteral("thread step-in");
    }
}

QString DebuggerController::stepOutCommand() const
{
    switch (backend_) {
    case Backend::Gdb:  return QStringLiteral("-exec-finish");
    case Backend::Cdb:  return QStringLiteral("gu");
    default:            return QStringLiteral("thread step-out");
    }
}

QString DebuggerController::quitCommand() const
{
    switch (backend_) {
    case Backend::Gdb:  return QStringLiteral("-gdb-exit");
    case Backend::Cdb:  return QStringLiteral("q");
    default:            return QStringLiteral("quit");
    }
}

// ---------------------------------------------------------------------------
// Output handling
// ---------------------------------------------------------------------------

void DebuggerController::onReadyRead()
{
    if (process_ == nullptr) {
        return;
    }
    outputBuffer_ += process_->readAll();
    // Decode in the charset the backend actually speaks: GDB negotiated
    // UTF-8 at startup (-gdb-set charset UTF-8 in startCommands) and LLDB
    // emits UTF-8 natively, so their streams must not go through
    // fromLocal8Bit — on a zh-CN Windows that would garble every Chinese
    // path and break the frame->editor match. cdb reports in the console
    // codepage, which is exactly what fromLocal8Bit decodes.
    const bool utf8Output =
        backend_ == Backend::Gdb || backend_ == Backend::Lldb;
    const auto decode = [utf8Output](const QByteArray& bytes) {
        return utf8Output ? QString::fromUtf8(bytes)
                          : QString::fromLocal8Bit(bytes);
    };
    // Drain every complete line; one chunk can carry several MI records and
    // gdb stays quiet after "*running", so leftovers would never be seen.
    for (;;) {
        const qsizetype newline = outputBuffer_.indexOf(u'\n');
        if (newline < 0) {
            break;
        }
        QString line = decode(outputBuffer_.left(newline));
        outputBuffer_.remove(0, newline + 1);
        handleLine(line.remove(QLatin1Char('\r')));
    }
    // No complete line yet — but a prompt (cdb's `0:000>`, lldb's `(lldb)`)
    // ends a chunk without a newline; flush it as a line.
    const QString pending = decode(outputBuffer_);
    if (pending.contains(QLatin1String("0:000>"))
        || pending.contains(QLatin1String("(lldb)"))) {
        outputBuffer_.clear();
        handleLine(pending);
    }
}

void DebuggerController::handleLine(const QString& line)
{
    emit logLine(line);
    switch (backend_) {
    case Backend::Gdb:  handleGdbLine(line); break;
    case Backend::Lldb: handleLldbLine(line); break;
    case Backend::Cdb:  handleCdbLine(line); break;
    default:            break;
    }
}

void DebuggerController::handleGdbLine(const QString& line)
{
    // Console stream output: ~"text"
    if (line.startsWith(QLatin1String("~\""))) {
        const int end = line.lastIndexOf(QLatin1Char('"'));
        if (end > 2) {
            emit logLine(unescapeMi(line.mid(2, end - 2)));
        }
        return;
    }
    // Target (program) output: @"text" or ~"text" depending on tty setup.
    if (line.startsWith(QLatin1String("@\""))) {
        const int end = line.lastIndexOf(QLatin1Char('"'));
        if (end > 2) {
            emit targetOutput(unescapeMi(line.mid(2, end - 2)));
        }
        return;
    }
    // *stopped,reason="breakpoint-hit",...,frame={...,file="E:/x/f.cpp",line="12",...}
    if (line.startsWith(QLatin1String("*stopped"))) {
        // The program ending normally also arrives as *stopped — but there
        // is no frame to jump to and "paused" would be the wrong state.
        if (line.contains(QLatin1String("reason=\"exited-normally\""))
            || line.contains(QLatin1String("reason=\"exited\""))) {
            paused_ = false;
            emit logLine(tr("target exited"));
            return;
        }
        paused_ = true;
        static const QRegularExpression filePattern(
            QStringLiteral("file=\"([^\"]*)\""));
        static const QRegularExpression linePattern(
            QStringLiteral("line=\"(\\d+)\""));
        const auto fileMatch = filePattern.match(line);
        const auto lineMatch = linePattern.match(line);
        QString file;
        int lineNumber = -1;
        if (fileMatch.hasMatch()) {
            file = normalizePath(unescapeMi(fileMatch.captured(1)));
        }
        if (lineMatch.hasMatch()) {
            lineNumber = lineMatch.captured(1).toInt() - 1;  // MI is 1-based
        }
        emit paused(file, lineNumber);
        return;
    }
    if (line.startsWith(QLatin1String("^error"))) {
        emit logLine(tr("debugger command failed: %1").arg(line));
        return;
    }
    if (line.startsWith(QLatin1String("*running"))) {
        paused_ = false;
        emit resumed();
    }
}

bool parseLldbFrameLocation(const QString& outputLine,
                             QString& filePath, int& zeroBasedLine)
{
    // LLDB commonly prints file.cpp:line:column, but some versions omit the
    // column. Match the two numeric suffixes separately: a greedy path group
    // then preserves drive letters (C:\\...) and colons in POSIX filenames.
    static const QRegularExpression withColumn(
        QStringLiteral("at (.+):(\\d+):(\\d+)\\s*$"));
    static const QRegularExpression withoutColumn(
        QStringLiteral("at (.+):(\\d+)\\s*$"));

    QRegularExpressionMatch match = withColumn.match(outputLine);
    if (!match.hasMatch()) {
        match = withoutColumn.match(outputLine);
    }
    if (!match.hasMatch()) {
        return false;
    }

    bool ok = false;
    const int oneBasedLine = match.captured(2).toInt(&ok);
    if (!ok || oneBasedLine <= 0) {
        return false;
    }
    filePath = normalizePath(match.captured(1));
    zeroBasedLine = oneBasedLine - 1;
    return true;
}

void DebuggerController::handleLldbLine(const QString& line)
{
    // "Process 1234 stopped" + "stop reason = breakpoint 1.1", followed by
    // "frame #0: 0x... app`main at main.cpp:8[:column]".
    if (line.contains(QLatin1String("stop reason ="))) {
        paused_ = true;
        return;
    }
    if (line.contains(QLatin1String("frame #"))) {
        QString file;
        int lineNumber = -1;
        if (parseLldbFrameLocation(line, file, lineNumber)) {
            emit paused(file, lineNumber);
            return;
        }
    }
    if (line.contains(QLatin1String("Process "))
        && line.contains(QLatin1String("exited"))) {
        paused_ = false;
        emit logLine(tr("target exited"));
    }
}

void DebuggerController::handleCdbLine(const QString& line)
{
    // "Breakpoint 0 hit" — cdb announces the hit but not the position. Ask
    // for a stack trace: with source lines enabled its innermost frame is
    // "  ... _app!main+0x4 [E:\path\file.cpp @ 5]".
    static const QRegularExpression bpHit(
        QStringLiteral("Breakpoint\\s+\\d+\\s+hit"));
    if (bpHit.match(line).hasMatch()) {
        paused_ = true;
        pendingCdbBreakpoint_ = true;
        writeCommand(QStringLiteral("k"));
        return;
    }
    if (paused_ && pendingCdbBreakpoint_) {
        // The stack frame form from the `k` request above.
        static const QRegularExpression kFrame(
            QStringLiteral("\\[([A-Za-z]:\\\\[^@\\]]+?) @ (\\d+)\\]"));
        const auto kMatch = kFrame.match(line);
        if (kMatch.hasMatch()) {
            pendingCdbBreakpoint_ = false;
            emit paused(normalizePath(kMatch.captured(1)),
                        kMatch.captured(2).toInt() - 1);
            return;
        }
        // Fallback: some setups echo the source line directly as file:line.
        static const QRegularExpression srcLine(
            QStringLiteral("([A-Za-z]:\\\\[^:]+\\.(?:cpp|c|cc|cxx|h|hpp)):(\\d+)"));
        const auto match = srcLine.match(line);
        if (match.hasMatch()) {
            pendingCdbBreakpoint_ = false;
            emit paused(normalizePath(match.captured(1)),
                        match.captured(2).toInt() - 1);
        }
        return;
    }
    if (line.contains(QLatin1String(" exited, returning "))) {
        paused_ = false;
        emit logLine(tr("target exited"));
    }
}

}  // namespace LightCpp::App
