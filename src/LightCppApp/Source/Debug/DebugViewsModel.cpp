// DebugViewsModel — implementation. See the header for the contract.

#include "LightCpp/App/Debug/DebugViewsModel.hpp"

#include <QRegularExpression>

namespace LightCpp::App::Debug {
namespace {

QString quoted(const QString& value)
{
    return QStringLiteral("\"%1\"")
        .arg(QString(value).replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
                 .replace(QLatin1Char('"'), QStringLiteral("\\\"")));
}

}  // namespace

DebugViewsModel::DebugViewsModel(QObject* parent) : QObject(parent) {}

void DebugViewsModel::setBackend(Backend backend)
{
    backend_ = backend;
    clear();
}

void DebugViewsModel::setCommandSink(std::function<void(const QString&)> sink)
{
    sink_ = std::move(sink);
}

void DebugViewsModel::setTokenAllocator(std::function<quint64()> allocator)
{
    tokenAllocator_ = std::move(allocator);
}

void DebugViewsModel::clear()
{
    frames_.clear();
    threads_.clear();
    locals_.clear();
    registers_.clear();
    memory_.clear();
    disassembly_.clear();
    for (WatchItem& watch : watches_) {
        watch.value.clear();
        watch.type.clear();
        watch.children.clear();
        watch.valid = false;
    }
    registerNames_.clear();
    selectedFrame_ = 0;
    selectedThread_ = 0;
    pending_ = PendingQuery{};
    textQueue_.clear();
    emit callStackChanged();
    emit threadsChanged();
    emit localsChanged();
    emit registersChanged();
    emit memoryChanged();
    emit disassemblyChanged();
    emit watchesChanged();
}

// ---------------------------------------------------------------------------
// Command dialects
// ---------------------------------------------------------------------------

QString DebugViewsModel::frameCommand(Backend backend, int maxFrames)
{
    switch (backend) {
    case Backend::Gdb:
        return QStringLiteral("-stack-list-frames 0 %1").arg(maxFrames);
    case Backend::Lldb:
        return QStringLiteral("thread backtrace -c %1").arg(maxFrames);
    case Backend::Cdb:
        return QStringLiteral("k");
    default:
        return {};
    }
}

QString DebugViewsModel::threadsCommand(Backend backend)
{
    switch (backend) {
    case Backend::Gdb:
        return QStringLiteral("-thread-info");
    case Backend::Lldb:
        return QStringLiteral("thread list");
    case Backend::Cdb:
        // `~*k` lists every thread with its innermost frame — the extra
        // output is what makes the thread list useful without a second query.
        return QStringLiteral("~*k");
    default:
        return {};
    }
}

QString DebugViewsModel::localsCommand(Backend backend)
{
    switch (backend) {
    case Backend::Gdb:
        // --all-values includes aggregates (needed for the expander);
        // --simple-values would omit the very types users expand.
        return QStringLiteral("-stack-list-variables --all-values");
    case Backend::Lldb:
        return QStringLiteral("frame variable");
    case Backend::Cdb:
        return QStringLiteral("dv /t /v");
    default:
        return {};
    }
}

QString DebugViewsModel::registersCommand(Backend backend, bool names)
{
    switch (backend) {
    case Backend::Gdb:
        return names ? QStringLiteral("-data-list-register-names")
                     : QStringLiteral("-data-list-register-values x");
    case Backend::Lldb:
        return names ? QString() : QStringLiteral("register read");
    case Backend::Cdb:
        return names ? QString() : QStringLiteral("r");
    default:
        return {};
    }
}

QString DebugViewsModel::memoryCommand(Backend backend, const QString& address,
                                       int count)
{
    switch (backend) {
    case Backend::Gdb:
        return QStringLiteral("-data-read-memory-bytes %1 %2").arg(address).arg(count);
    case Backend::Lldb:
        return QStringLiteral("memory read -c %1 %2").arg(count).arg(address);
    case Backend::Cdb:
        return QStringLiteral("db %1 L?%2").arg(address).arg(count);
    default:
        return {};
    }
}

QString DebugViewsModel::disassemblyCommand(Backend backend,
                                            const QString& address, int count)
{
    switch (backend) {
    case Backend::Gdb:
        // `-a` disassembles from an address; `-- 0` means "no source lines",
        // which is what a disassembly view shows.
        return QStringLiteral("-data-disassemble -a %1 -- 0").arg(address);
    case Backend::Lldb:
        return QStringLiteral("disassemble -s %1 -c %2").arg(address).arg(count);
    case Backend::Cdb:
        return QStringLiteral("u %1 L?%2").arg(address).arg(count);
    default:
        return {};
    }
}

QString DebugViewsModel::evaluateCommand(Backend backend, const QString& expression)
{
    switch (backend) {
    case Backend::Gdb:
        return QStringLiteral("-data-evaluate-expression %1").arg(quoted(expression));
    case Backend::Lldb:
        return QStringLiteral("expression -- %1").arg(expression);
    case Backend::Cdb:
        return QStringLiteral("? %1").arg(expression);
    default:
        return {};
    }
}

QString DebugViewsModel::watchCommand(Backend backend, const QString& expression)
{
    switch (backend) {
    case Backend::Gdb:
        // -var-create gives a stable handle whose children can be listed,
        // which is what makes STL/smart-pointer expansion possible at all.
        return QStringLiteral("-var-create - * %1").arg(quoted(expression));
    case Backend::Lldb:
        return QStringLiteral("frame variable %1").arg(expression);
    case Backend::Cdb:
        return QStringLiteral("dx %1").arg(expression);
    default:
        return {};
    }
}

QString DebugViewsModel::watchChildrenCommand(Backend backend,
                                              const QString& expression)
{
    switch (backend) {
    case Backend::Gdb:
        return QStringLiteral("-var-list-children --all-values %1").arg(quoted(expression));
    case Backend::Lldb:
        return QStringLiteral("frame variable %1").arg(expression);
    case Backend::Cdb:
        return QStringLiteral("dx %1").arg(expression);
    default:
        return {};
    }
}

QString DebugViewsModel::selectFrameCommand(Backend backend, int level)
{
    switch (backend) {
    case Backend::Gdb:
        return QStringLiteral("-stack-select-frame %1").arg(level);
    case Backend::Lldb:
        return QStringLiteral("frame select %1").arg(level);
    case Backend::Cdb:
        return QStringLiteral(".frame %1").arg(level);
    default:
        return {};
    }
}

QString DebugViewsModel::selectThreadCommand(Backend backend, int id)
{
    switch (backend) {
    case Backend::Gdb:
        return QStringLiteral("-thread-select %1").arg(id);
    case Backend::Lldb:
        return QStringLiteral("thread select %1").arg(id);
    case Backend::Cdb:
        // cdb's thread index in `~*k` is the argument of `~<n>s`.
        return QStringLiteral("~%1s").arg(id);
    default:
        return {};
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void DebugViewsModel::send(Pending kind, const QString& command,
                           const QString& argument)
{
    if (command.isEmpty() || !sink_) {
        return;
    }
    PendingQuery query;
    query.kind = kind;
    query.argument = argument;
    if (backend_ == Backend::Gdb) {
        const quint64 token = tokenAllocator_ ? tokenAllocator_()
                                              : privateToken_++;
        query.token = token;
        pending_ = query;
        sink_(QString::number(token) + command);
        return;
    }
    textQueue_.append(query);
    sink_(command);
}

void DebugViewsModel::requestCallStack(int maxFrames)
{
    send(Pending::Stack, frameCommand(backend_, maxFrames), QString());
}

void DebugViewsModel::requestThreads()
{
    send(Pending::Threads, threadsCommand(backend_), QString());
}

void DebugViewsModel::requestLocals()
{
    send(Pending::Locals, localsCommand(backend_), QString());
}

void DebugViewsModel::requestRegisters()
{
    if (backend_ == Backend::Gdb && registerNames_.isEmpty()) {
        // Names first: the values answer carries numbers only. The two
        // queries queue naturally because GDB answers in order.
        send(Pending::RegisterNames, registersCommand(backend_, true), QString());
        return;
    }
    send(Pending::RegisterValues, registersCommand(backend_, false), QString());
}

void DebugViewsModel::requestMemory(const QString& address, int byteCount)
{
    if (byteCount <= 0) {
        byteCount = 64;
    }
    send(Pending::Memory, memoryCommand(backend_, address, byteCount), address);
}

void DebugViewsModel::requestDisassembly(const QString& address, int instructionCount)
{
    if (instructionCount <= 0) {
        instructionCount = 20;
    }
    send(Pending::Disassembly,
         disassemblyCommand(backend_, address, instructionCount), address);
}

void DebugViewsModel::evaluate(const QString& expression)
{
    send(Pending::Evaluate, evaluateCommand(backend_, expression), expression);
}

void DebugViewsModel::addWatch(const QString& expression)
{
    for (WatchItem& watch : watches_) {
        if (watch.expression == expression) {
            watch.valid = false;
            break;
        }
    }
    bool known = false;
    for (const WatchItem& watch : std::as_const(watches_)) {
        known = known || watch.expression == expression;
    }
    if (!known) {
        WatchItem item;
        item.expression = expression;
        watches_.append(item);
    }
    emit watchesChanged();
    send(Pending::WatchCreate, watchCommand(backend_, expression), expression);
}

void DebugViewsModel::removeWatch(const QString& expression)
{
    for (qsizetype i = 0; i < watches_.size(); ++i) {
        if (watches_.at(i).expression == expression) {
            watches_.removeAt(i);
            emit watchesChanged();
            return;
        }
    }
}

void DebugViewsModel::expandVariable(const QString& rootExpression,
                                     const QString& name)
{
    Q_UNUSED(name);
    send(Pending::WatchChildren, watchChildrenCommand(backend_, rootExpression),
         rootExpression);
}

void DebugViewsModel::selectFrame(int level)
{
    selectedFrame_ = level;
    send(Pending::FrameSelect, selectFrameCommand(backend_, level), QString::number(level));
}

void DebugViewsModel::selectThread(int id)
{
    selectedThread_ = id;
    send(Pending::ThreadSelect, selectThreadCommand(backend_, id), QString::number(id));
}

QStringList DebugViewsModel::attachCommands(int pid) const
{
    switch (backend_) {
    case Backend::Gdb:
        // -target-attach is the MI form; `attach <pid>` works too but the MI
        // record is what the controller parses.
        return {QStringLiteral("-target-attach %1").arg(pid)};
    case Backend::Lldb:
        return {QStringLiteral("process attach --pid %1").arg(pid)};
    case Backend::Cdb:
        // cdb's -p flag is a launch option, not a command: the controller
        // starts it with `-p <pid>` (see attachArguments()).
        return {};
    default:
        return {};
    }
}

QStringList DebugViewsModel::remoteCommands(const QString& endpoint) const
{
    switch (backend_) {
    case Backend::Gdb:
        return {QStringLiteral("-target-select remote %1").arg(endpoint)};
    case Backend::Lldb:
        return {QStringLiteral("gdb-remote %1").arg(endpoint)};
    case Backend::Cdb:
        // cdb reaches a remote through the .server/.remote pair; the endpoint
        // is "<transport>:server=<host>,port=<port>".
        return {QStringLiteral(".remote %1").arg(endpoint)};
    default:
        return {};
    }
}

QStringList DebugViewsModel::coreCommands(const QString& coreFile) const
{
    const QString path = coreFile;
    switch (backend_) {
    case Backend::Gdb:
        return {QStringLiteral("-target-select core %1").arg(quoted(path))};
    case Backend::Lldb:
        return {QStringLiteral("target create --core %1").arg(quoted(path))};
    case Backend::Cdb:
        return {QStringLiteral(".opendump %1").arg(quoted(path))};
    default:
        return {};
    }
}

void DebugViewsModel::refreshOnStop()
{
    requestCallStack();
    requestThreads();
    requestLocals();
    requestRegisters();
    for (const WatchItem& watch : std::as_const(watches_)) {
        send(Pending::WatchCreate, watchCommand(backend_, watch.expression),
             watch.expression);
    }
}

// ---------------------------------------------------------------------------
// Answers
// ---------------------------------------------------------------------------

bool DebugViewsModel::handleTokenAnswer(const MiRecord& record)
{
    if (pending_.kind == Pending::None || pending_.token == 0) {
        return false;
    }
    bool ok = false;
    const quint64 token = record.token.toULongLong(&ok);
    if (!ok || token != pending_.token) {
        return false;
    }
    const bool succeeded = record.name == QLatin1String("done")
                           || record.name == QLatin1String("running");
    const Pending kind = pending_.kind;
    const QString argument = pending_.argument;
    pending_ = PendingQuery{};

    // A rejected query must not erase the view it was meant to refresh: an
    // empty stack reads as "no frames", which is a different claim from
    // "GDB would not answer".
    const bool isSnapshot = kind == Pending::Stack || kind == Pending::Threads
                            || kind == Pending::Locals
                            || kind == Pending::RegisterValues
                            || kind == Pending::Memory
                            || kind == Pending::Disassembly;
    if (!succeeded && isSnapshot) {
        return true;
    }

    switch (kind) {
    case Pending::Stack:
        frames_ = framesFromMi(record.results);
        emit callStackChanged();
        return true;
    case Pending::Threads:
        threads_ = parseGdbThreads(record.results);
        emit threadsChanged();
        return true;
    case Pending::Locals:
        locals_ = parseGdbVariables(record.results);
        emit localsChanged();
        return true;
    case Pending::RegisterNames: {
        const MiValue* names = record.results.find(QStringLiteral("register-names"));
        registerNames_.clear();
        if (names != nullptr) {
            for (const auto& entry : names->children()) {
                registerNames_ << entry.second.text();
            }
        }
        // The names answer is not a snapshot of its own: ask for the values
        // immediately, which is what the caller wanted.
        requestRegisters();
        return true;
    }
    case Pending::RegisterValues:
        registers_ = parseGdbRegisters(record.results, registerNames_);
        emit registersChanged();
        return true;
    case Pending::Memory:
        memory_ = parseGdbMemory(record.results);
        emit memoryChanged();
        return true;
    case Pending::Disassembly: {
        const MiValue* insns = record.results.find(QStringLiteral("asm_insns"));
        disassembly_ = insns != nullptr ? parseGdbDisassembly(*insns)
                                        : parseGdbDisassembly(record.results);
        emit disassemblyChanged();
        return true;
    }
    case Pending::FrameSelect:
    case Pending::ThreadSelect:
        // The selection moved: the locals belong to the new frame/thread.
        requestLocals();
        return true;
    case Pending::Evaluate: {
        const QString value = record.results.value(QStringLiteral("value"));
        emit evaluationFinished(argument, succeeded ? value : QString(), succeeded);
        return true;
    }
    case Pending::WatchCreate:
    case Pending::WatchChildren: {
        for (WatchItem& watch : watches_) {
            if (watch.expression != argument) {
                continue;
            }
            if (!succeeded) {
                watch.valid = false;
                watch.value = QStringLiteral("<not available>");
                break;
            }
            const QString name = record.results.value(QStringLiteral("name"));
            const QString numChild = record.results.value(QStringLiteral("numchild"));
            watch.valid = true;
            watch.type = record.results.value(QStringLiteral("type"));
            watch.value = record.results.value(QStringLiteral("value"));
            watch.hasChildren = numChild.toInt() > 0;
            if (kind == Pending::WatchChildren) {
                watch.children = parseGdbChildren(record.results);
            }
            if (watch.hasChildren && watch.value.isEmpty()) {
                watch.value = QStringLiteral("{...}");
            }
            Q_UNUSED(name);
            break;
        }
        emit watchesChanged();
        return true;
    }
    default:
        return false;
    }
}

void DebugViewsModel::flushTextAnswer(Pending kind, const QStringList& lines,
                                      const QString& argument)
{
    const QString text = lines.join(QLatin1Char('\n'));
    switch (kind) {
    case Pending::Stack:
        // LLDB and cdb both print file:line; the shared scanner is in
        // DebugViews. For cdb, `k` already carries the position per frame.
        if (backend_ == Backend::Lldb) {
            frames_.clear();
            static const QRegularExpression framePattern(
                QStringLiteral("^\\s*\\*?\\s*frame #(\\d+):"));
            for (const QString& line : lines) {
                const auto match = framePattern.match(line);
                if (!match.hasMatch()) {
                    continue;
                }
                FrameInfo info;
                info.level = match.captured(1).toInt();
                QString file;
                int oneBased = 0;
                if (parseFramePosition(line, Backend::Lldb, file, oneBased)) {
                    info.file = file;
                    info.line = oneBased;
                }
                // "app`main at a.cpp:8" — the symbol follows the address.
                static const QRegularExpression symbolPattern(
                    QStringLiteral("0x[0-9a-fA-F]+\\s+(\\S+?)(?:\\+0x[0-9a-fA-F]+)?(?:\\s+at\\s|\\s*$)"));
                if (const auto sym = symbolPattern.match(line); sym.hasMatch()) {
                    info.function = sym.captured(1);
                    const qsizetype bang = info.function.indexOf(QLatin1Char('`'));
                    if (bang >= 0) {
                        info.module = info.function.left(bang);
                        info.function = info.function.mid(bang + 1);
                    }
                }
                frames_.append(info);
            }
            emit callStackChanged();
            return;
        }
        if (backend_ == Backend::Cdb) {
            frames_.clear();
            static const QRegularExpression cdbFrame(
                QStringLiteral("^\\s*([0-9a-fA-F]{2})\\s+([0-9a-fA-F]{8,16})\\s+([0-9a-fA-F]{8,16})"));
            static const QRegularExpression cdbSymbol(
                QStringLiteral("^\\s*([0-9a-fA-F]{2})\\s+(\\S+)!(.+?)\\+0x[0-9a-fA-F]+"));
            for (const QString& line : lines) {
                const auto sym = cdbSymbol.match(line);
                if (!sym.hasMatch()) {
                    continue;
                }
                FrameInfo info;
                info.level = sym.captured(1).toInt(nullptr, 16);
                info.module = sym.captured(2);
                info.function = sym.captured(3);
                QString file;
                int oneBased = 0;
                if (parseFramePosition(line, Backend::Cdb, file, oneBased)) {
                    info.file = file;
                    info.line = oneBased;
                }
                frames_.append(info);
            }
            Q_UNUSED(cdbFrame);
            emit callStackChanged();
            return;
        }
        return;
    case Pending::Threads:
        threads_ = backend_ == Backend::Lldb ? parseLldbThreads(text)
                                             : parseCdbThreads(text);
        emit threadsChanged();
        return;
    case Pending::Locals:
        locals_ = backend_ == Backend::Lldb ? parseLldbVariables(text)
                                            : parseCdbVariables(text);
        emit localsChanged();
        return;
    case Pending::RegisterValues:
        registers_ = backend_ == Backend::Lldb ? parseLldbRegisters(text)
                                               : parseCdbRegisters(text);
        emit registersChanged();
        return;
    case Pending::Memory:
        memory_ = parseHexDump(text);
        emit memoryChanged();
        return;
    case Pending::Disassembly:
        disassembly_ = parseTextDisassembly(
            text, backend_ == Backend::Lldb ? Backend::Lldb : Backend::Cdb);
        emit disassemblyChanged();
        return;
    case Pending::Evaluate:
        // "expression -- x" prints "(int) $0 = 3" / "? x" prints
        // "Evaluate expression: 3 = 00000000`00000003".
        emit evaluationFinished(argument, text.trimmed(), !text.trimmed().isEmpty());
        return;
    case Pending::WatchCreate:
    case Pending::WatchChildren:
        for (WatchItem& watch : watches_) {
            if (watch.expression != argument) {
                continue;
            }
            if (backend_ == Backend::Lldb) {
                const QList<VariableInfo> parsed = parseLldbVariables(text);
                if (parsed.isEmpty()) {
                    watch.valid = false;
                } else {
                    watch.valid = true;
                    watch.type = parsed.first().type;
                    watch.value = parsed.first().value;
                    watch.hasChildren = parsed.first().hasChildren;
                }
            } else {
                const QList<VariableInfo> parsed = parseCdbVariables(text);
                if (parsed.isEmpty()) {
                    watch.valid = false;
                } else {
                    watch.valid = true;
                    watch.type = parsed.first().type;
                    watch.value = parsed.first().value;
                    watch.hasChildren = parsed.first().hasChildren;
                }
            }
            break;
        }
        emit watchesChanged();
        return;
    case Pending::RegisterNames:
        return;
    case Pending::FrameSelect:
    case Pending::ThreadSelect:
        // The selection changed; the caller follows up with a locals refresh.
        requestLocals();
        return;
    default:
        return;
    }
}

void DebugViewsModel::finishTextAnswerIfPrompt(const QString& line)
{
    const QString trimmed = line.trimmed();
    const bool isPrompt = trimmed.endsWith(QLatin1String("(lldb)"))
                          || trimmed.endsWith(QLatin1String("0:000>"))
                          || trimmed == QLatin1String("(gdb)");
    if (!isPrompt || textQueue_.isEmpty()) {
        return;
    }
    const PendingQuery query = textQueue_.takeFirst();
    flushTextAnswer(query.kind, query.lines, query.argument);
}

void DebugViewsModel::observeLine(const QString& line)
{
    if (backend_ == Backend::Gdb) {
        const MiRecord record = parseMiRecord(line);
        if (record.kind == MiRecord::Kind::Result
            || record.kind == MiRecord::Kind::ExecAsync) {
            if (handleTokenAnswer(record)) {
                return;
            }
            // A `^error` for one of our queries must release the slot, or the
            // next query's answer would be matched against the stale token.
            if (record.kind == MiRecord::Kind::Result
                && record.name == QLatin1String("error")
                && !record.token.isEmpty()) {
                bool ok = false;
                const quint64 token = record.token.toULongLong(&ok);
                if (ok && token == pending_.token) {
                    const QString argument = pending_.argument;
                    const Pending kind = pending_.kind;
                    pending_ = PendingQuery{};
                    if (kind == Pending::Evaluate) {
                        emit evaluationFinished(argument, QString(), false);
                    }
                }
            }
            return;
        }
        // `-var-list-children` and friends answer in a single record; console
        // and target records carry no view data.
        return;
    }

    // LLDB / cdb: accumulate the answer in progress, end it at the prompt.
    if (!textQueue_.isEmpty()) {
        textQueue_.last().lines.append(line);
    }
    finishTextAnswerIfPrompt(line);
}

}  // namespace LightCpp::App::Debug
