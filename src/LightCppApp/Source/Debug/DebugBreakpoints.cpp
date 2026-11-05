// DebugBreakpoints — implementation. See the header for the contract.

#include "LightCpp/App/Debug/DebugBreakpoints.hpp"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace LightCpp::App::Debug {
namespace {

using LightCpp::App::DebuggerController;

QString normalize(QString path)
{
    path = QDir::fromNativeSeparators(path);
    // LLDB can report a Windows drive path even when it is fed one on POSIX;
    // keep that spelling comparable too.
    if (path.size() >= 3 && path.at(1) == QLatin1Char(':')
        && path.at(2) == QLatin1Char('\\')) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    }
    return path;
}

QString escapeField(const QString& value)
{
    QString out = value;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('|'), QLatin1String("\\p"));
    out.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    return out;
}

QString unescapeField(const QString& value)
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
        if (next == QLatin1Char('p')) {
            out.append(QLatin1Char('|'));
        } else if (next == QLatin1Char('n')) {
            out.append(QLatin1Char('\n'));
        } else if (next == QLatin1Char('\\')) {
            out.append(QLatin1Char('\\'));
        } else {
            out.append(QLatin1Char('\\')).append(next);
        }
    }
    return out;
}

QString flagsOf(const Breakpoint& bp)
{
    QStringList flags;
    if (!bp.enabled) {
        flags << QStringLiteral("disabled");
    }
    if (bp.temporary) {
        flags << QStringLiteral("temporary");
    }
    if (bp.logpoint) {
        flags << QStringLiteral("logpoint");
    }
    return flags.join(QLatin1Char(','));
}

QString memoryModeName(MemoryMode mode)
{
    switch (mode) {
    case MemoryMode::Write:      return QStringLiteral("w");
    case MemoryMode::Read:       return QStringLiteral("r");
    case MemoryMode::ReadWrite:  return QStringLiteral("rw");
    case MemoryMode::Execute:    return QStringLiteral("x");
    }
    return QStringLiteral("w");
}

MemoryMode memoryModeFromName(const QString& name)
{
    if (name == QLatin1String("r"))  return MemoryMode::Read;
    if (name == QLatin1String("rw")) return MemoryMode::ReadWrite;
    if (name == QLatin1String("x"))  return MemoryMode::Execute;
    return MemoryMode::Write;
}

/// cdb spells the watch size as `w1`/`w2`/`w4`; anything else is clamped.
int cdbWatchSize(int size)
{
    if (size <= 1) {
        return 1;
    }
    if (size <= 2) {
        return 2;
    }
    return 4;
}

/// A location that GDB accepts verbatim, with MI escapes applied by the caller.
QString gdbLocation(const Breakpoint& bp)
{
    switch (bp.kind) {
    case BreakpointKind::Line:
        return normalize(bp.file) + QLatin1Char(':') + QString::number(bp.line + 1);
    case BreakpointKind::Function:
    case BreakpointKind::Address:
    case BreakpointKind::Memory:
        return bp.expression;
    }
    return {};
}

QString quotedMi(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return escaped;
}

/// LLDB addresses a breakpoint by `--function` / `--address` / `--file --line`,
/// and a watchpoint by `watchpoint set expression`.
QStringList lldbArmCommands(const Breakpoint& bp)
{
    const QString condition = bp.condition.trimmed();
    const QString conditionFlag = condition.isEmpty()
        ? QString()
        : QStringLiteral(" --condition \"%1\"")
              .arg(QString(condition).replace(QLatin1Char('"'), QStringLiteral("\\\"")));
    const QString ignoreFlag = bp.ignoreCount > 0
        ? QStringLiteral(" --ignore-count %1").arg(bp.ignoreCount)
        : QString();
    const QString oneShot = bp.temporary ? QStringLiteral(" --one-shot") : QString();

    QStringList commands;
    switch (bp.kind) {
    case BreakpointKind::Line:
        commands << QStringLiteral("breakpoint set --file \"%1\" --line %2%3%4%5")
                        .arg(QFileInfo(bp.file).fileName())
                        .arg(bp.line + 1)
                        .arg(conditionFlag, ignoreFlag, oneShot);
        break;
    case BreakpointKind::Function:
        commands << QStringLiteral("breakpoint set --name \"%1\"%2%3%4")
                        .arg(bp.expression, conditionFlag, ignoreFlag, oneShot);
        break;
    case BreakpointKind::Address:
        commands << QStringLiteral("breakpoint set --address %1%2%3%4")
                        .arg(bp.expression, conditionFlag, ignoreFlag, oneShot);
        break;
    case BreakpointKind::Memory:
        commands << QStringLiteral("watchpoint set expression --size %1 -- %2")
                        .arg(bp.memorySize > 0 ? bp.memorySize : 8)
                        .arg(bp.expression);
        break;
    }
    if (!bp.enabled) {
        commands << QStringLiteral("%1 disable")
                        .arg(bp.kind == BreakpointKind::Memory
                                 ? QStringLiteral("watchpoint")
                                 : QStringLiteral("breakpoint"));
    }
    // A logpoint must not stop the process; two `-o` commands do that.
    if (bp.logpoint && bp.kind != BreakpointKind::Memory) {
        commands << QStringLiteral("breakpoint command add -o \"%1\" -o continue")
                        .arg(QString(logpointMessage(bp))
                                 .replace(QLatin1Char('"'), QStringLiteral("\\\"")));
    }
    return commands;
}

QStringList cdbArmCommands(const Breakpoint& bp)
{
    const QString oneShot = bp.temporary ? QStringLiteral(" /1") : QString();
    QStringList commands;
    switch (bp.kind) {
    case BreakpointKind::Line:
        commands << QStringLiteral("bp%1 `%2:%3`")
                        .arg(oneShot, normalize(bp.file))
                        .arg(bp.line + 1);
        break;
    case BreakpointKind::Function:
    case BreakpointKind::Address:
        commands << QStringLiteral("bp%1 %2").arg(oneShot, bp.expression);
        break;
    case BreakpointKind::Memory: {
        const QString mode = bp.memoryMode == MemoryMode::Read
                                 ? QStringLiteral("r")
                                 : bp.memoryMode == MemoryMode::ReadWrite
                                       ? QStringLiteral("rw")
                                       : bp.memoryMode == MemoryMode::Execute
                                             ? QStringLiteral("e")
                                             : QStringLiteral("w");
        commands << QStringLiteral("ba %1%2 %3")
                        .arg(mode)
                        .arg(cdbWatchSize(bp.memorySize))
                        .arg(bp.expression);
        break;
    }
    }
    // Conditions and ignore counts need the breakpoint's id, which cdb only
    // reveals through `bl`; editCommands() sets them once it is known.
    return commands;
}

}  // namespace

// ---------------------------------------------------------------------------
// Breakpoint
// ---------------------------------------------------------------------------

QString Breakpoint::location() const
{
    switch (kind) {
    case BreakpointKind::Line:
        return normalize(file) + QLatin1Char(':') + QString::number(line + 1);
    case BreakpointKind::Function:
        return QStringLiteral("func ") + expression;
    case BreakpointKind::Address:
        return QStringLiteral("@") + expression;
    case BreakpointKind::Memory:
        return QStringLiteral("watch %1").arg(expression);
    }
    return {};
}

QString Breakpoint::decorations() const
{
    QStringList parts;
    if (!condition.trimmed().isEmpty()) {
        parts << QStringLiteral("if %1").arg(condition.trimmed());
    }
    if (ignoreCount > 0) {
        parts << QStringLiteral("ignore %1").arg(ignoreCount);
    }
    if (temporary) {
        parts << QStringLiteral("temporary");
    }
    if (logpoint) {
        parts << QStringLiteral("logpoint");
    }
    if (kind == BreakpointKind::Memory) {
        parts << QStringLiteral("size %1").arg(memorySize);
    }
    if (!enabled) {
        parts << QStringLiteral("disabled");
    }
    return parts.join(QLatin1Char(','));
}

QString Breakpoint::label() const
{
    QString text = location();
    const QString extra = decorations();
    if (!extra.isEmpty()) {
        text += QStringLiteral("  (") + extra + QLatin1Char(')');
    }
    if (hitCount > 0) {
        text += QStringLiteral("  hit %1").arg(hitCount);
    }
    return text;
}

// ---------------------------------------------------------------------------
// BreakpointModel
// ---------------------------------------------------------------------------

int BreakpointModel::add(Breakpoint bp)
{
    if (bp.kind == BreakpointKind::Line && !bp.file.isEmpty()) {
        for (const Breakpoint& existing : std::as_const(items_)) {
            if (existing.kind == BreakpointKind::Line
                && normalize(existing.file) == normalize(bp.file)
                && existing.line == bp.line) {
                Breakpoint* target = findMutable(existing.id);
                target->condition = bp.condition;
                target->ignoreCount = bp.ignoreCount;
                target->temporary = bp.temporary;
                target->logpoint = bp.logpoint;
                target->enabled = bp.enabled;
                return existing.id;
            }
        }
    }
    if (bp.id <= 0) {
        bp.id = nextId_++;
    } else {
        nextId_ = std::max(nextId_, bp.id + 1);
    }
    items_.append(bp);
    return bp.id;
}

bool BreakpointModel::remove(int id)
{
    for (qsizetype i = 0; i < items_.size(); ++i) {
        if (items_.at(i).id == id) {
            items_.removeAt(i);
            return true;
        }
    }
    return false;
}

bool BreakpointModel::update(int id, const Breakpoint& fields)
{
    Breakpoint* target = findMutable(id);
    if (target == nullptr) {
        return false;
    }
    target->condition = fields.condition;
    target->ignoreCount = fields.ignoreCount;
    target->temporary = fields.temporary;
    target->logpoint = fields.logpoint;
    target->enabled = fields.enabled;
    target->memoryMode = fields.memoryMode;
    target->memorySize = fields.memorySize;
    if (!fields.expression.isEmpty() && target->kind != BreakpointKind::Line) {
        target->expression = fields.expression;
    }
    return true;
}

const Breakpoint* BreakpointModel::find(int id) const
{
    for (const Breakpoint& bp : items_) {
        if (bp.id == id) {
            return &bp;
        }
    }
    return nullptr;
}

Breakpoint* BreakpointModel::findMutable(int id)
{
    for (Breakpoint& bp : items_) {
        if (bp.id == id) {
            return &bp;
        }
    }
    return nullptr;
}

const Breakpoint* BreakpointModel::findLine(const QString& file, int line) const
{
    const QString wanted = normalize(file);
    for (const Breakpoint& bp : items_) {
        if (bp.kind == BreakpointKind::Line && bp.line == line
            && normalize(bp.file) == wanted) {
            return &bp;
        }
    }
    return nullptr;
}

int BreakpointModel::toggleLine(const QString& file, int line)
{
    if (const Breakpoint* existing = findLine(file, line)) {
        remove(existing->id);
        return 0;
    }
    Breakpoint bp;
    bp.kind = BreakpointKind::Line;
    bp.file = normalize(file);
    bp.line = line;
    return add(bp);
}

QList<Breakpoint> BreakpointModel::enabledItems() const
{
    QList<Breakpoint> out;
    for (const Breakpoint& bp : items_) {
        if (bp.enabled) {
            out.append(bp);
        }
    }
    return out;
}

void BreakpointModel::markArmed(int id, int backendNumber)
{
    if (Breakpoint* bp = findMutable(id)) {
        bp->backendNumber = backendNumber;
    }
}

void BreakpointModel::markDisarmed(int id)
{
    if (Breakpoint* bp = findMutable(id)) {
        bp->backendNumber = -1;
    }
}

QStringList BreakpointModel::lineKeys() const
{
    QStringList keys;
    for (const Breakpoint& bp : items_) {
        if (bp.kind == BreakpointKind::Line && !bp.file.isEmpty()) {
            keys << normalize(bp.file) + QLatin1Char(':') + QString::number(bp.line);
        }
    }
    return keys;
}

void BreakpointModel::clear()
{
    items_.clear();
    nextId_ = 1;
}

QStringList BreakpointModel::serialize() const
{
    QStringList lines;
    for (const Breakpoint& bp : items_) {
        QString kindName;
        QString locationField;
        switch (bp.kind) {
        case BreakpointKind::Line:
            kindName = QStringLiteral("line");
            locationField = escapeField(normalize(bp.file) + QLatin1Char(':')
                                        + QString::number(bp.line));
            break;
        case BreakpointKind::Function:
            kindName = QStringLiteral("function");
            locationField = escapeField(bp.expression);
            break;
        case BreakpointKind::Address:
            kindName = QStringLiteral("address");
            locationField = escapeField(bp.expression);
            break;
        case BreakpointKind::Memory:
            kindName = QStringLiteral("memory");
            locationField = escapeField(QStringLiteral("%1:%2:%3")
                                            .arg(memoryModeName(bp.memoryMode))
                                            .arg(bp.memorySize)
                                            .arg(bp.expression));
            break;
        }
        lines << QStringList{kindName,
                             locationField,
                             escapeField(bp.condition),
                             QString::number(bp.ignoreCount),
                             flagsOf(bp)}
                     .join(QLatin1Char('|'));
    }
    return lines;
}

void BreakpointModel::deserialize(const QStringList& lines)
{
    for (const QString& raw : lines) {
        const QStringList parts = raw.split(QLatin1Char('|'));
        if (parts.size() < 5) {
            continue;
        }
        Breakpoint bp;
        bp.condition = unescapeField(parts.at(2));
        bp.ignoreCount = parts.at(3).toInt();
        const QStringList flags = parts.at(4).split(QLatin1Char(','));
        bp.enabled = !flags.contains(QStringLiteral("disabled"));
        bp.temporary = flags.contains(QStringLiteral("temporary"));
        bp.logpoint = flags.contains(QStringLiteral("logpoint"));

        const QString kind = parts.at(0);
        const QString locationField = unescapeField(parts.at(1));
        if (kind == QLatin1String("line")) {
            const qsizetype colon = locationField.lastIndexOf(QLatin1Char(':'));
            if (colon <= 1) {
                continue;
            }
            bp.kind = BreakpointKind::Line;
            bp.file = normalize(locationField.left(colon));
            bp.line = locationField.mid(colon + 1).toInt();
        } else if (kind == QLatin1String("function")) {
            bp.kind = BreakpointKind::Function;
            bp.expression = locationField;
        } else if (kind == QLatin1String("address")) {
            bp.kind = BreakpointKind::Address;
            bp.expression = locationField;
        } else if (kind == QLatin1String("memory")) {
            const QStringList pieces = locationField.split(QLatin1Char(':'));
            if (pieces.size() < 3) {
                continue;
            }
            bp.kind = BreakpointKind::Memory;
            bp.memoryMode = memoryModeFromName(pieces.at(0));
            bp.memorySize = pieces.at(1).toInt();
            // The expression may itself contain ':'; re-join the tail.
            bp.expression = pieces.mid(2).join(QLatin1Char(':'));
        } else {
            continue;
        }
        add(bp);
    }
}

// ---------------------------------------------------------------------------
// Dialect
// ---------------------------------------------------------------------------

QString gdbArmFlags(const Breakpoint& bp)
{
    QString flags;
    if (bp.temporary) {
        flags += QStringLiteral(" -t");
    }
    if (!bp.enabled) {
        flags += QStringLiteral(" -d");
    }
    if (!bp.condition.trimmed().isEmpty()) {
        flags += QStringLiteral(" -c \"%1\"").arg(quotedMi(bp.condition));
    }
    if (bp.ignoreCount > 0) {
        flags += QStringLiteral(" -i %1").arg(bp.ignoreCount);
    }
    return flags;
}

QStringList gdbLogpointCommands(const Breakpoint& bp)
{
    if (!bp.logpoint) {
        return {};
    }
    QString message = logpointMessage(bp);
    // GDB's `printf` format string: what it treats specially must be escaped.
    message.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    message.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return {QStringLiteral("printf \"%1\\n\"").arg(message),
            QStringLiteral("continue")};
}

QString logpointMessage(const Breakpoint& bp)
{
    QString message = bp.expression.startsWith(QLatin1String("log:"))
        ? bp.expression.mid(4) : bp.condition;
    if (message.trimmed().isEmpty()) {
        message = QStringLiteral("hit %1").arg(bp.location());
    }
    return message;
}

QStringList armCommands(DebuggerController::Backend backend,
                        const Breakpoint& bp)
{
    switch (backend) {
    case DebuggerController::Backend::Gdb: {
        if (bp.kind == BreakpointKind::Memory) {
            // `-break-watch` exists but has no read/read-write variant, so
            // rwatch/awatch go through the console interpreter.
            const QString verb = bp.memoryMode == MemoryMode::Read
                                     ? QStringLiteral("rwatch")
                                     : bp.memoryMode == MemoryMode::ReadWrite
                                           ? QStringLiteral("awatch")
                                           : QStringLiteral("watch");
            return {QStringLiteral("-interpreter-exec console \"%1 %2\"")
                        .arg(verb, quotedMi(bp.expression))};
        }
        return {QStringLiteral("-break-insert%1 \"%2\"")
                    .arg(gdbArmFlags(bp), quotedMi(gdbLocation(bp)))};
    }
    case DebuggerController::Backend::Lldb:
        return lldbArmCommands(bp);
    case DebuggerController::Backend::Cdb:
        return cdbArmCommands(bp);
    default:
        return {};
    }
}

QStringList disarmCommands(DebuggerController::Backend backend,
                           const Breakpoint& bp,
                           const QList<Breakpoint>& stillArmed)
{
    switch (backend) {
    case DebuggerController::Backend::Gdb:
        if (bp.backendNumber > 0) {
            return {QStringLiteral("-break-delete %1").arg(bp.backendNumber)};
        }
        // Never confirmed: fall back to clearing by location.
        return {QStringLiteral("-interpreter-exec console \"clear %1\"")
                    .arg(quotedMi(gdbLocation(bp)))};
    case DebuggerController::Backend::Cdb:
    case DebuggerController::Backend::Lldb: {
        // Neither can address one breakpoint by location: clear all, re-arm
        // what is still supposed to be there (see the header).
        QStringList commands;
        commands << (backend == DebuggerController::Backend::Cdb
                         ? QStringLiteral("bc *")
                         : QStringLiteral("breakpoint delete -f"));
        for (const Breakpoint& other : stillArmed) {
            if (other.id == bp.id) {
                continue;
            }
            commands << armCommands(backend, other);
        }
        return commands;
    }
    default:
        return {};
    }
}

bool canEditInPlace(DebuggerController::Backend backend)
{
    return backend == DebuggerController::Backend::Gdb;
}

QStringList editCommands(DebuggerController::Backend backend,
                         const Breakpoint& bp)
{
    if (backend != DebuggerController::Backend::Gdb || bp.backendNumber <= 0) {
        return {};
    }
    const int number = bp.backendNumber;
    const bool isWatch = bp.kind == BreakpointKind::Memory;
    QStringList commands;
    if (!bp.enabled) {
        commands << QStringLiteral("-break-disable %1").arg(number);
        return commands;
    }
    commands << QStringLiteral("-break-enable %1").arg(number);
    if (!isWatch) {
        const QString condition = bp.condition.trimmed().isEmpty()
            ? QStringLiteral("1")
            : bp.condition;
        commands << QStringLiteral("-break-condition %1 %2")
                        .arg(number)
                        .arg(quotedMi(condition));
        commands << QStringLiteral("-break-after %1 %2").arg(number).arg(bp.ignoreCount);
    }
    if (bp.logpoint && !isWatch) {
        QStringList quoted;
        for (const QString& line : gdbLogpointCommands(bp)) {
            quoted << QStringLiteral("\"%1\"").arg(quotedMi(line));
        }
        commands << QStringLiteral("-break-commands %1 %2")
                        .arg(number)
                        .arg(quoted.join(QLatin1Char(' ')));
    }
    return commands;
}

}  // namespace LightCpp::App::Debug
