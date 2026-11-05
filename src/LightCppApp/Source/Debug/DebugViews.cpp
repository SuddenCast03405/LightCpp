// DebugViews — implementation. See the header for the contract, including the
// captured-transcript provenance of every accepted form.

#include "LightCpp/App/Debug/DebugViews.hpp"

#include <QDir>
#include <QRegularExpression>

namespace LightCpp::App::Debug {
namespace {

QString normalizePath(QString path)
{
    path = path.trimmed();
    // A Windows drive path is converted even when this build runs on POSIX:
    // the transcript fixtures carry one, and QDir::fromNativeSeparators only
    // knows the *host* separator.
    if (path.size() >= 3 && path.at(1) == QLatin1Char(':')
        && (path.at(2) == QLatin1Char('\\') || path.at(2) == QLatin1Char('/'))) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return path;
    }
    return QDir::fromNativeSeparators(path);
}

/// Count top-level members of an aggregate printed as `{a = 1, b = 2}` or
/// `[1, 2, 3]`, ignoring nesting. Zero when `text` is not bracketed.
int topLevelCount(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.size() < 2) {
        return 0;
    }
    const QChar open = trimmed.at(0);
    if (open != QLatin1Char('{') && open != QLatin1Char('[')) {
        return 0;
    }
    int depth = 0;
    int count = 0;
    bool sawItem = false;
    for (const QChar ch : trimmed) {
        if (ch == QLatin1Char('{') || ch == QLatin1Char('[')) {
            ++depth;
            continue;
        }
        if (ch == QLatin1Char('}') || ch == QLatin1Char(']')) {
            --depth;
            continue;
        }
        if (depth == 1) {
            if (ch == QLatin1Char(',')) {
                if (sawItem) {
                    ++count;
                    sawItem = false;
                }
            } else if (!ch.isSpace()) {
                sawItem = true;
            }
        }
    }
    if (sawItem) {
        ++count;
    }
    return count;
}

VariableInfo variableFromMi(const MiValue& node)
{
    VariableInfo info;
    info.name = node.value(QStringLiteral("name"));
    info.value = node.value(QStringLiteral("value"));
    info.type = node.value(QStringLiteral("type"));
    if (info.type.isEmpty()) {
        info.type = node.value(QStringLiteral("displayhint"));
    }
    const MiValue* children = node.find(QStringLiteral("children"));
    if (children != nullptr) {
        info.children = parseGdbChildren(*children);
        info.childCount = static_cast<int>(info.children.size());
        info.hasChildren = info.childCount > 0;
        if (info.value.isEmpty() && info.hasChildren) {
            info.value = QStringLiteral("{...}");
        }
    } else {
        info.childCount = topLevelCount(info.value);
        info.hasChildren = info.childCount > 0;
    }
    return info;
}

}  // namespace

// ---------------------------------------------------------------------------
// Variables
// ---------------------------------------------------------------------------

QList<VariableInfo> parseGdbVariables(const MiValue& variables, int scope)
{
    const MiValue* list = &variables;
    if (const MiValue* inner = variables.find(QStringLiteral("variables"))) {
        list = inner;
    }
    QList<VariableInfo> out;
    for (const auto& entry : list->children()) {
        const MiValue& node = entry.second;
        if (node.kind() != MiValue::Kind::Tuple) {
            continue;
        }
        const bool isArg = node.value(QStringLiteral("arg")) == QLatin1String("1")
                           || node.value(QStringLiteral("arg")) == QLatin1String("true");
        if (scope == 0 && isArg) {
            continue;
        }
        if (scope == 1 && !isArg) {
            continue;
        }
        out.append(variableFromMi(node));
    }
    return out;
}

QList<VariableInfo> parseGdbChildren(const MiValue& children)
{
    const MiValue* list = &children;
    if (const MiValue* inner = children.find(QStringLiteral("children"))) {
        list = inner;
    }
    QList<VariableInfo> out;
    for (const auto& entry : list->children()) {
        const MiValue& node = entry.second;
        if (node.kind() != MiValue::Kind::Tuple) {
            continue;
        }
        // -var-list-children names the child variable "child{name=...}"; the
        // display name is either `exp` or `name`.
        const MiValue* childNode = node.find(QStringLiteral("child"));
        const MiValue& child = (childNode != nullptr) ? *childNode : node;
        VariableInfo info = variableFromMi(child);
        if (info.name.isEmpty()) {
            info.name = node.value(QStringLiteral("exp"));
        }
        out.append(info);
    }
    return out;
}

QList<VariableInfo> parseLldbVariables(const QString& text)
{
    // "    (int) x = 3" / "    (std::vector<int>) v = size=3 {}" /
    // "    (char *) s = 0x0000000100003f50 \"hi\""
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*\\(([^)]*)\\)\\s+(.+?)\\s*=\\s*(.*)$"));
    QList<VariableInfo> out;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const auto match = pattern.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        VariableInfo info;
        info.type = match.captured(1).trimmed();
        info.name = match.captured(2).trimmed();
        info.value = match.captured(3).trimmed();
        info.childCount = topLevelCount(info.value);
        info.hasChildren = info.childCount > 0
                           || info.value.startsWith(QLatin1String("size="));
        // LLDB prints `size=3 {1, 2, 3}` for containers: the count is in the
        // prefix, which is more reliable than counting a truncated body.
        static const QRegularExpression sizePrefix(
            QStringLiteral("^size=(\\d+)"));
        if (const auto sizeMatch = sizePrefix.match(info.value);
            sizeMatch.hasMatch()) {
            info.childCount = sizeMatch.captured(1).toInt();
            info.hasChildren = info.childCount > 0;
        }
        out.append(info);
    }
    return out;
}

QList<VariableInfo> parseCdbVariables(const QString& text)
{
    // "    int x = 0n3" / "    struct S s = {a=0x1 b=0x2}" / "    char *p = 0x..."
    // The type is one token for scalars and several for templates, so the
    // split is anchored on the *last* ` = ` before the value.
    static const QRegularExpression pattern(
        QStringLiteral("^\\s+(.+?)\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(.*)$"));
    QList<VariableInfo> out;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty() || line.trimmed().startsWith(QLatin1String("***"))
            || line.contains(QLatin1String("Unable to"))) {
            continue;
        }
        const auto match = pattern.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        VariableInfo info;
        info.type = match.captured(1).trimmed();
        info.name = match.captured(2).trimmed();
        info.value = match.captured(3).trimmed();
        // cdb prints `0n3` for decimal, `0x1f` for hex: keep both visible but
        // offer the plain number for the numeric columns.
        info.childCount = topLevelCount(info.value);
        info.hasChildren = info.childCount > 0;
        out.append(info);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

QList<ThreadInfo> parseGdbThreads(const MiValue& threads)
{
    const MiValue* list = &threads;
    if (const MiValue* inner = threads.find(QStringLiteral("threads"))) {
        list = inner;
    }
    QList<ThreadInfo> out;
    for (const auto& entry : list->children()) {
        const MiValue& node = entry.second;
        if (node.kind() != MiValue::Kind::Tuple) {
            continue;
        }
        ThreadInfo info;
        info.id = static_cast<int>(node.number(QStringLiteral("id")));
        info.name = node.value(QStringLiteral("name"));
        info.state = node.value(QStringLiteral("state"));
        info.isCurrent = node.value(QStringLiteral("current")) == QLatin1String("true")
                         || node.value(QStringLiteral("current")) == QLatin1String("*");
        if (const MiValue* frame = node.find(QStringLiteral("frame"))) {
            info.frameFunction = frame->value(QStringLiteral("func"));
            info.frameFile = normalizePath(frame->value(QStringLiteral("file")));
            info.frameLine = static_cast<int>(frame->number(QStringLiteral("line")));
            if (!info.frameFile.isEmpty() && info.frameLine > 0) {
                info.frameFullName = QStringLiteral("%1:%2")
                                         .arg(info.frameFile)
                                         .arg(info.frameLine);
            }
        }
        out.append(info);
    }
    return out;
}

QList<ThreadInfo> parseLldbThreads(const QString& text)
{
    // "thread #1: tid = 0x1f0f, 0x0000000100003f50 app`main at a.cpp:8:5, name = 'main'"
    // The fields are optional and vary by LLDB version, so each one is taken
    // by its own narrow pattern instead of one all-or-nothing regex.
    static const QRegularExpression head(
        QStringLiteral("^\\s*(\\*)?\\s*thread #(\\d+):\\s*tid = (0x[0-9a-fA-F]+)"));
    static const QRegularExpression position(
        QStringLiteral("\\bat ([^,:]+):(\\d+)"));
    static const QRegularExpression name(
        QStringLiteral("name = '([^']*)'"));
    static const QRegularExpression reason(
        QStringLiteral("stop reason = (.+)$"));

    QList<ThreadInfo> out;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (!line.contains(QLatin1String("thread #"))) {
            continue;
        }
        const auto headMatch = head.match(line);
        if (!headMatch.hasMatch()) {
            continue;
        }
        ThreadInfo info;
        info.id = headMatch.captured(2).toInt();
        info.isCurrent = !headMatch.captured(1).isEmpty();
        if (const auto positionMatch = position.match(line); positionMatch.hasMatch()) {
            info.frameFile = normalizePath(positionMatch.captured(1));
            info.frameLine = positionMatch.captured(2).toInt();
            info.frameFullName = QStringLiteral("%1:%2")
                                     .arg(info.frameFile)
                                     .arg(info.frameLine);
        }
        if (const auto nameMatch = name.match(line); nameMatch.hasMatch()) {
            info.name = nameMatch.captured(1);
        }
        if (const auto reasonMatch = reason.match(line); reasonMatch.hasMatch()) {
            info.state = QStringLiteral("stopped");
        }
        out.append(info);
    }
    if (!out.isEmpty()) {
        bool anyCurrent = false;
        for (const ThreadInfo& info : out) {
            anyCurrent = anyCurrent || info.isCurrent;
        }
        if (!anyCurrent) {
            out.first().isCurrent = true;
        }
    }
    return out;
}

QList<ThreadInfo> parseCdbThreads(const QString& text)
{
    // `~*k` rows:
    //   0  id: 1234 create child  ...
    //      app!main+0x4 [a.cpp @ 8]
    // `~*` rows:
    //    0  id: 1234  Teb: ... Win32Thread: ...  RUNNING
    static const QRegularExpression stackHead(
        QStringLiteral("^\\s*(\\d+)\\s+id:\\s*(\\S+)\\s+(.*)$"));
    static const QRegularExpression flatHead(
        QStringLiteral("^\\s*(\\d+)\\s+id:\\s*(\\S+).*?\\b(RUNNING|WAIT|SUSPENDED)\\b"));
    static const QRegularExpression frameLine(
        QStringLiteral("^\\s+(\\S+)!(.+?)\\+0x[0-9a-fA-F]+\\s*\\[([^\\]@]+)\\s*@\\s*(\\d+)\\]"));

    QList<ThreadInfo> out;
    ThreadInfo* pending = nullptr;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (const auto head = stackHead.match(line); head.hasMatch()) {
            ThreadInfo info;
            info.id = head.captured(2).toInt();
            info.name = head.captured(3).trimmed();
            info.state = QStringLiteral("stopped");
            out.append(info);
            pending = &out.last();
            continue;
        }
        if (const auto head = flatHead.match(line); head.hasMatch()) {
            ThreadInfo info;
            info.id = head.captured(2).toInt();
            info.state = head.captured(3).toLower();
            out.append(info);
            pending = &out.last();
            continue;
        }
        if (pending != nullptr) {
            if (const auto frame = frameLine.match(line); frame.hasMatch()) {
                pending->frameFunction = frame.captured(2);
                pending->frameFile = normalizePath(frame.captured(3));
                pending->frameLine = frame.captured(4).toInt();
                pending->frameFullName = QStringLiteral("%1:%2")
                                             .arg(pending->frameFile)
                                             .arg(pending->frameLine);
                pending = nullptr;
            }
        }
    }
    if (!out.isEmpty()) {
        out.first().isCurrent = true;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

QList<RegisterInfo> parseGdbRegisters(const MiValue& values,
                                      const QStringList& names)
{
    const MiValue* list = &values;
    if (const MiValue* inner = values.find(QStringLiteral("register-values"))) {
        list = inner;
    }
    QList<RegisterInfo> out;
    for (const auto& entry : list->children()) {
        const MiValue& node = entry.second;
        if (node.kind() != MiValue::Kind::Tuple) {
            continue;
        }
        RegisterInfo info;
        info.number = static_cast<int>(node.number(QStringLiteral("number")));
        if (info.number >= 0 && info.number < names.size()) {
            info.name = names.at(info.number);
        }
        info.value = node.value(QStringLiteral("value"));
        out.append(info);
    }
    return out;
}

QList<RegisterInfo> parseLldbRegisters(const QString& text)
{
    // "     rax = 0x0000000000000000"
    // "     rip = 0x0000000100003f50  app`main + 4"
    static const QRegularExpression pattern(
        QStringLiteral("^\\s{2,}([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(.+?)\\s*$"));
    QList<RegisterInfo> out;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const auto match = pattern.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        RegisterInfo info;
        info.name = match.captured(1);
        info.value = match.captured(2);
        out.append(info);
    }
    return out;
}

QList<RegisterInfo> parseCdbRegisters(const QString& text)
{
    // "rax=0000000000000000 rbx=0000000000000000 ..."
    static const QRegularExpression pair(
        QStringLiteral("([A-Za-z][A-Za-z0-9]*)=([0-9a-fA-F]+)"));
    QList<RegisterInfo> out;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        auto it = pair.globalMatch(line);
        while (it.hasNext()) {
            const auto match = it.next();
            RegisterInfo info;
            info.name = match.captured(1);
            info.value = QStringLiteral("0x") + match.captured(2);
            out.append(info);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

QList<MemoryRow> parseGdbMemory(const MiValue& memory)
{
    const MiValue* list = &memory;
    if (const MiValue* inner = memory.find(QStringLiteral("memory"))) {
        list = inner;
    }
    QList<MemoryRow> out;
    for (const auto& entry : list->children()) {
        const MiValue& node = entry.second;
        if (node.kind() != MiValue::Kind::Tuple) {
            continue;
        }
        MemoryRow row;
        row.address = node.value(QStringLiteral("begin"));
        const QString contents = node.value(QStringLiteral("contents"));
        row.bytes = QByteArray::fromHex(contents.toLatin1());
        if (row.address.isEmpty()) {
            continue;
        }
        out.append(row);
    }
    return out;
}

QList<MemoryRow> parseHexDump(const QString& text)
{
    // lldb: "0x0000000100003f50: 48 89 e5 48 83 ec 20 ..."
    // cdb : "00000000`0012ff00  48 89 e5 48 83 ec 20 ..."
    // Both are "address, colon or spaces, then hex byte pairs".
    static const QRegularExpression addressFirst(
        QStringLiteral("^\\s*(0x[0-9a-fA-F]+|[0-9a-fA-F]{8}`[0-9a-fA-F]{8}|[0-9a-fA-F]{8,16})[:\\s]\\s*(.*)$"));
    static const QRegularExpression bytePattern(
        QStringLiteral("\\b([0-9a-fA-F]{2})\\b"));

    QList<MemoryRow> out;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const auto match = addressFirst.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        QString address = match.captured(1);
        address.remove(QLatin1Char('`'));
        MemoryRow row;
        row.address = address.startsWith(QLatin1String("0x"))
            ? address : QStringLiteral("0x") + address;
        const QString tail = match.captured(2);
        // cdb appends the ASCII rendering after the bytes; stop at the first
        // group of printable text that is not a hex pair.
        auto bytes = bytePattern.globalMatch(tail);
        QStringList collected;
        QString previousEnd;
        while (bytes.hasNext()) {
            const auto byte = bytes.next();
            // Only accept a contiguous run: a gap means the ASCII column began.
            if (!previousEnd.isEmpty()
                && byte.capturedStart() - previousEnd.toInt() > 3) {
                break;
            }
            collected << byte.captured(1);
            previousEnd = QString::number(byte.capturedEnd());
        }
        for (const QString& hex : collected) {
            row.bytes.append(static_cast<char>(hex.toInt(nullptr, 16)));
        }
        if (!row.bytes.isEmpty()) {
            out.append(row);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Disassembly
// ---------------------------------------------------------------------------

QList<DisassemblyLine> parseGdbDisassembly(const MiValue& asmInsns)
{
    const MiValue* list = &asmInsns;
    if (const MiValue* inner = asmInsns.find(QStringLiteral("asm_insns"))) {
        list = inner;
    }
    QList<DisassemblyLine> out;
    for (const auto& entry : list->children()) {
        const MiValue& node = entry.second;
        if (node.kind() != MiValue::Kind::Tuple) {
            continue;
        }
        // `-- 1` (mixed source and assembly) wraps each line:
        //   {line="8",file="a.cpp",line_asm_insn=[{address=,func-name=,offset=,inst=}]}
        if (const MiValue* nested = node.find(QStringLiteral("line_asm_insn"))) {
            for (const auto& item : nested->children()) {
                const MiValue& insn = item.second;
                if (insn.kind() != MiValue::Kind::Tuple) {
                    continue;
                }
                DisassemblyLine line;
                line.address = insn.value(QStringLiteral("address"));
                line.function = insn.value(QStringLiteral("func-name"));
                line.instruction = insn.value(QStringLiteral("inst"));
                if (!line.instruction.isEmpty()) {
                    out.append(line);
                }
            }
            continue;
        }
        DisassemblyLine line;
        line.address = node.value(QStringLiteral("address"));
        line.function = node.value(QStringLiteral("func-name"));
        line.instruction = node.value(QStringLiteral("inst"));
        if (!line.instruction.isEmpty()) {
            out.append(line);
        }
    }
    return out;
}

QList<DisassemblyLine> parseTextDisassembly(const QString& text, Backend backend)
{
    QList<DisassemblyLine> out;
    if (backend == Backend::Lldb) {
        // "->  0x0000000100003f50 <+0>: pushq  %rbp"
        static const QRegularExpression pattern(
            QStringLiteral("^\\s*(?:->\\s*)?(0x[0-9a-fA-F]+)\\s*(?:<(?:[+-]\\d+|\\S+)>:\\s*)?(.+)$"));
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            const auto match = pattern.match(line);
            if (!match.hasMatch()) {
                continue;
            }
            DisassemblyLine insn;
            insn.address = match.captured(1);
            insn.instruction = match.captured(2).trimmed();
            if (!insn.instruction.isEmpty()) {
                out.append(insn);
            }
        }
        return out;
    }
    if (backend == Backend::Cdb) {
        // "00007ff6`12341000 4883ec28        sub     rsp,28h"
        static const QRegularExpression pattern(
            QStringLiteral("^([0-9a-fA-F]{8}`[0-9a-fA-F]{8})\\s+([0-9a-fA-F]{2}(?:\\s?[0-9a-fA-F]{2})*)\\s+(.+)$"));
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            const auto match = pattern.match(line);
            if (!match.hasMatch()) {
                continue;
            }
            DisassemblyLine insn;
            QString address = match.captured(1);
            address.remove(QLatin1Char('`'));
            insn.address = QStringLiteral("0x") + address;
            insn.instruction = match.captured(3).trimmed();
            out.append(insn);
        }
        return out;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------

bool parseFramePosition(const QString& text, Backend backend,
                        QString& file, int& oneBasedLine)
{
    QRegularExpression pattern;
    int fileGroup = 1;
    int lineGroup = 2;
    switch (backend) {
    case Backend::Gdb:
        // "8\t  x = 3;" is not a position; GDB reports positions in MI only.
        return false;
    case Backend::Lldb:
        pattern = QRegularExpression(QStringLiteral("at ([^:]+):(\\d+)(?::\\d+)?\\s*$"));
        break;
    case Backend::Cdb:
        pattern = QRegularExpression(QStringLiteral("\\[([^\\]@]+)\\s*@\\s*(\\d+)\\]"));
        break;
    default:
        return false;
    }
    const auto match = pattern.match(text);
    if (!match.hasMatch()) {
        return false;
    }
    file = normalizePath(match.captured(fileGroup));
    oneBasedLine = match.captured(lineGroup).toInt();
    return oneBasedLine > 0;
}

bool splitCdbSymbol(const QString& text, QString& module, QString& function)
{
    const qsizetype bang = text.indexOf(QLatin1Char('!'));
    if (bang <= 0) {
        return false;
    }
    module = text.left(bang).trimmed();
    function = text.mid(bang + 1).trimmed();
    // Strip the "+0x1f" offset: the function's name is what the UI shows.
    const qsizetype plus = function.indexOf(QLatin1String("+0x"));
    if (plus > 0) {
        function = function.left(plus);
    }
    return !module.isEmpty();
}

}  // namespace LightCpp::App::Debug
