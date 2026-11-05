// GdbMi — implementation. See the header for the contract.

#include "LightCpp/App/Debug/GdbMi.hpp"

#include <QChar>

namespace LightCpp::App::Debug {
namespace {

/// MI's C string escapes: \\ \" \n \t \r plus octal and a few control names.
/// Anything else keeps the backslash, which is what GDB does for unknown
/// escapes and what makes round-tripping a path with a literal backslash
/// (Windows) survivable.
constexpr int kMaxNesting = 64;

bool isTupleOpen(QChar ch) { return ch == QLatin1Char('{'); }
bool isListOpen(QChar ch) { return ch == QLatin1Char('['); }

/// A name is `[A-Za-z_-][A-Za-z0-9_-]*` in MI; `=` follows it. cdb's
/// `p` output and LLDB's `frame variable` text reuse this parser with the
/// identical rule, so no dialect flag is needed here.
bool isNameStart(QChar ch)
{
    return ch.isLetter() || ch == QLatin1Char('_') || ch == QLatin1Char('-');
}

bool isNameChar(QChar ch)
{
    return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-');
}

void skipSpace(const QString& text, int& pos)
{
    while (pos < text.size() && text.at(pos).isSpace()) {
        ++pos;
    }
}

MiValue parseInternal(const QString& text, int& pos, int depth);

/// Reads members until `close` (or the end of `text` when close is NUL).
/// Shared by tuples, lists, and the bare `key=value` tail of a result record
/// — the three places MI puts members, differing only in their terminator.
void parseMembers(const QString& text, int& pos, QChar close, int depth,
                  MiValue& node)
{
    for (;;) {
        skipSpace(text, pos);
        if (pos >= text.size()) {
            break;
        }
        if (close != QChar() && text.at(pos) == close) {
            ++pos;
            break;
        }
        if (text.at(pos) == QLatin1Char(',')) {
            ++pos;
            continue;
        }
        // `name=value` or a bare value (list items, and GDB's positionals).
        if (isNameStart(text.at(pos))) {
            int scan = pos;
            while (scan < text.size() && isNameChar(text.at(scan))) {
                ++scan;
            }
            if (scan < text.size() && text.at(scan) == QLatin1Char('=')) {
                const QString name = text.mid(pos, scan - pos);
                pos = scan + 1;
                node.append(name, parseInternal(text, pos, depth + 1));
                continue;
            }
        }
        const int before = pos;
        MiValue bare = parseInternal(text, pos, depth + 1);
        if (pos == before) {
            ++pos;  // nothing consumed: step over the character and go on
            continue;
        }
        node.append(QString(), std::move(bare));
    }
}

MiValue parseInternal(const QString& text, int& pos, int depth)
{
    if (depth > kMaxNesting) {
        // Malformed: give up without consuming, so the caller reports one
        // failure rather than recursing to the stack limit.
        return MiValue::constant(QString());
    }
    skipSpace(text, pos);
    if (pos >= text.size()) {
        return MiValue::constant(QString());
    }
    const QChar ch = text.at(pos);

    if (ch == QLatin1Char('"')) {
        ++pos;
        QString body;
        while (pos < text.size()) {
            const QChar c = text.at(pos);
            if (c == QLatin1Char('\\') && pos + 1 < text.size()) {
                // Keep the escape pair intact for unescapeC(); a `\"` must
                // not end the string, which is exactly the bug a naive
                // "read until the next quote" has.
                body.append(c);
                body.append(text.at(pos + 1));
                pos += 2;
                continue;
            }
            if (c == QLatin1Char('"')) {
                ++pos;
                break;
            }
            body.append(c);
            ++pos;
        }
        return MiValue::constant(unescapeC(body));
    }

    if (isTupleOpen(ch) || isListOpen(ch)) {
        const QChar close = isTupleOpen(ch) ? QLatin1Char('}') : QLatin1Char(']');
        MiValue node = isTupleOpen(ch) ? MiValue::tuple() : MiValue::list();
        ++pos;
        parseMembers(text, pos, close, depth, node);
        return node;
    }

    // Bare word: everything up to , } ] or end.
    const int start = pos;
    while (pos < text.size()) {
        const QChar c = text.at(pos);
        if (c == QLatin1Char(',') || c == QLatin1Char('}') || c == QLatin1Char(']')) {
            break;
        }
        ++pos;
    }
    return MiValue::constant(text.mid(start, pos - start).trimmed());
}

/// Reads a values list (`results="k:v;k:v"`) into a tuple, so
/// `frame="..."`-style cdb output looks like an MI tuple to the callers.
MiValue kvListToTuple(const QString& text)
{
    MiValue tuple = MiValue::tuple();
    const QStringList pairs = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& pair : pairs) {
        const qsizetype colon = pair.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            continue;
        }
        tuple.append(pair.left(colon), MiValue::constant(pair.mid(colon + 1)));
    }
    return tuple;
}

}  // namespace

// ---------------------------------------------------------------------------
// MiValue
// ---------------------------------------------------------------------------

MiValue MiValue::constant(QString text)
{
    MiValue value;
    value.kind_ = Kind::Const;
    value.text_ = std::move(text);
    return value;
}

MiValue MiValue::tuple()
{
    MiValue value;
    value.kind_ = Kind::Tuple;
    return value;
}

MiValue MiValue::list()
{
    MiValue value;
    value.kind_ = Kind::List;
    return value;
}

void MiValue::append(QString name, MiValue value)
{
    children_.append({std::move(name), std::move(value)});
}

const MiValue* MiValue::find(const QString& name) const
{
    for (const auto& member : children_) {
        if (member.first == name) {
            return &member.second;
        }
    }
    return nullptr;
}

QString MiValue::value(const QString& name) const
{
    const MiValue* member = find(name);
    if (member == nullptr) {
        return QString();
    }
    return member->text_;
}

long long MiValue::number(const QString& name) const
{
    const MiValue* member = find(name);
    if (member == nullptr) {
        return 0;
    }
    bool ok = false;
    const long long parsed = member->text_.toLongLong(&ok);
    return ok ? parsed : 0;
}

// ---------------------------------------------------------------------------
// Escapes
// ---------------------------------------------------------------------------

QString unescapeC(const QString& body)
{
    QString out;
    out.reserve(body.size());
    for (int i = 0; i < body.size(); ++i) {
        const QChar ch = body.at(i);
        if (ch != QLatin1Char('\\') || i + 1 >= body.size()) {
            out.append(ch);
            continue;
        }
        const QChar next = body.at(++i);
        switch (next.unicode()) {
        case '\\': out.append(QLatin1Char('\\')); break;
        case '"':  out.append(QLatin1Char('"')); break;
        case 'n':  out.append(QLatin1Char('\n')); break;
        case 't':  out.append(QLatin1Char('\t')); break;
        case 'r':  out.append(QLatin1Char('\r')); break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            // Up to three octal digits.
            int value = next.digitValue();
            int taken = 1;
            while (taken < 3 && i + 1 < body.size()
                   && body.at(i + 1) >= QLatin1Char('0')
                   && body.at(i + 1) <= QLatin1Char('7')) {
                value = value * 8 + body.at(i + 1).digitValue();
                ++i;
                ++taken;
            }
            out.append(QChar(static_cast<char16_t>(value & 0xFF)));
            break;
        }
        default:
            out.append(QLatin1Char('\\')).append(next);
            break;
        }
    }
    return out;
}

QString escapeC(const QString& value)
{
    QString out;
    out.reserve(value.size() + 8);
    for (const QChar ch : value) {
        switch (ch.unicode()) {
        case '\\': out.append(QLatin1String("\\\\")); break;
        case '"':  out.append(QLatin1String("\\\"")); break;
        case '\n': out.append(QLatin1String("\\n")); break;
        case '\t': out.append(QLatin1String("\\t")); break;
        case '\r': out.append(QLatin1String("\\r")); break;
        default:   out.append(ch); break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

MiValue parseMiValue(const QString& text, int& pos)
{
    // A bare `name=value,name=value` tail (the shape after `^done,` and the
    // shape fixtures pass in whole) is a tuple body without its braces.
    int scan = pos;
    while (scan < text.size() && text.at(scan).isSpace()) {
        ++scan;
    }
    if (scan < text.size() && isNameStart(text.at(scan))) {
        int nameEnd = scan;
        while (nameEnd < text.size() && isNameChar(text.at(nameEnd))) {
            ++nameEnd;
        }
        if (nameEnd < text.size() && text.at(nameEnd) == QLatin1Char('=')) {
            MiValue tuple = MiValue::tuple();
            parseMembers(text, scan, QChar(), 0, tuple);
            pos = scan;
            return tuple;
        }
    }
    return parseInternal(text, pos, 0);
}

MiRecord parseMiRecord(const QString& line)
{
    MiRecord record;
    int pos = 0;

    // A leading token: digits, then the sigil.
    int sigil = 0;
    while (sigil < line.size() && line.at(sigil).isDigit()) {
        ++sigil;
    }
    if (sigil < line.size()) {
        record.token = line.left(sigil);
        pos = sigil;
    } else {
        pos = 0;
    }
    if (pos >= line.size()) {
        return record;
    }

    const QChar marker = line.at(pos);
    switch (marker.unicode()) {
    case '^': record.kind = MiRecord::Kind::Result; break;
    case '*': record.kind = MiRecord::Kind::ExecAsync; break;
    case '+': record.kind = MiRecord::Kind::StatusAsync; break;
    case '=': record.kind = MiRecord::Kind::NotifyAsync; break;
    case '~': record.kind = MiRecord::Kind::Console; break;
    case '@': record.kind = MiRecord::Kind::Target; break;
    case '&': record.kind = MiRecord::Kind::Log; break;
    default:
        if (line.startsWith(QLatin1String("(gdb)"))) {
            record.kind = MiRecord::Kind::Prompt;
            record.text = line;
            return record;
        }
        record.kind = MiRecord::Kind::Unknown;
        record.text = line;
        return record;
    }
    ++pos;

    if (marker == QLatin1Char('~') || marker == QLatin1Char('@')
        || marker == QLatin1Char('&')) {
        // `~"text"`; everything between the first and last quote is the body.
        const qsizetype firstQuote = line.indexOf(QLatin1Char('"'), pos);
        const qsizetype lastQuote = line.lastIndexOf(QLatin1Char('"'));
        if (firstQuote >= 0 && lastQuote > firstQuote) {
            record.text = unescapeC(line.mid(firstQuote + 1, lastQuote - firstQuote - 1));
        } else {
            record.text = line.mid(pos);
        }
        record.name = QString::fromLatin1(
            marker == QLatin1Char('~') ? "console"
            : marker == QLatin1Char('@') ? "target" : "log");
        return record;
    }

    // `name` up to `,` or end.
    const qsizetype comma = line.indexOf(QLatin1Char(','), pos);
    record.name = (comma < 0 ? line.mid(pos) : line.mid(pos, comma - pos)).trimmed();
    pos = (comma < 0) ? line.size() : static_cast<int>(comma + 1);
    if (pos < line.size()) {
        // A result record's tail is a bare `k=v,k=v` list with no braces
        // around it: parse it as a tuple body rather than as one value.
        record.results = MiValue::tuple();
        int tail = pos;
        parseMembers(line, tail, QChar(), 0, record.results);
        if (record.results.children().isEmpty()
            && line.mid(pos).contains(QLatin1Char(';'))) {
            // `k:v;k:v` — the shape some cdb/LLDB texts use.
            record.results = kvListToTuple(line.mid(pos));
        }
    }
    return record;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

QList<FrameInfo> framesFromMi(const MiValue& stack)
{
    QList<FrameInfo> frames;

    // GDB: stack=[frame={level="0",addr="0x...",func="main",file="a.c",line="3"}]
    // `-stack-list-frames` without flags answers exactly that; with
    // `--no-frame-filters` it may answer the same shape. Nested threads form
    // (stack=[{thread-id="1",frame=[...]}]) is also accepted — the shape that
    // `-thread-apply-all` produces.
    const MiValue* entries = &stack;
    if (const MiValue* inner = stack.find(QStringLiteral("stack"))) {
        entries = inner;
    }
    for (const auto& entry : entries->children()) {
        const MiValue& node = entry.second;
        if (node.kind() != MiValue::Kind::Tuple) {
            continue;
        }
        // A thread wrapper: descend into its `frame` list.
        if (const MiValue* nested = node.find(QStringLiteral("frame"))) {
            if (nested->kind() == MiValue::Kind::List) {
                for (const auto& item : nested->children()) {
                    const MiValue& frame = item.second;
                    if (frame.kind() != MiValue::Kind::Tuple) {
                        continue;
                    }
                    FrameInfo info;
                    info.level = static_cast<int>(frame.number(QStringLiteral("level")));
                    info.address = frame.value(QStringLiteral("addr"));
                    info.function = frame.value(QStringLiteral("func"));
                    info.file = frame.value(QStringLiteral("file"));
                    info.line = static_cast<int>(frame.number(QStringLiteral("line")));
                    frames.append(info);
                }
            }
            continue;
        }
        // A flat frame entry: `frame={...}` or the tuple itself.
        const MiValue* frameNode = node.find(QStringLiteral("frame"));
        const MiValue& frame = (frameNode != nullptr) ? *frameNode : node;
        if (frame.kind() != MiValue::Kind::Tuple) {
            continue;
        }
        FrameInfo info;
        info.level = static_cast<int>(frame.number(QStringLiteral("level")));
        info.address = frame.value(QStringLiteral("addr"));
        info.function = frame.value(QStringLiteral("func"));
        info.file = frame.value(QStringLiteral("file"));
        info.line = static_cast<int>(frame.number(QStringLiteral("line")));
        if (frame.find(QStringLiteral("func")) == nullptr
            && frame.find(QStringLiteral("level")) == nullptr) {
            continue;  // a wrapper we do not understand
        }
        frames.append(info);
    }
    return frames;
}

}  // namespace LightCpp::App::Debug
