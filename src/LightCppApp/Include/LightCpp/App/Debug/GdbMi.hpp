// GdbMi — a value semantics model of the GDB/MI output grammar, plus the
// console dialect of the other two backends (LLDB, cdb) folded into the same
// shapes.
//
// Why a real parser instead of QString::contains(): every view this IDE shows
// during a debug session (locals, call stack, threads, registers, memory,
// disassembly) is one MI result record, and MI nests tuples and lists that
// contain the same text a regex would have to guess at. `parseValue()` knows
// the grammar, so `-stack-list-frames` answers a tree rather than a string,
// and a value like `{a = 1, b = {c = 2}}` stays a tree instead of becoming a
// flat list of text fragments.
//
// The parser is pure: no QProcess, no Qt widgets, no debugger. That is what
// lets tests/test_dbg cover it, and what keeps the controller below honest --
// it can only react to things the parser actually produced.

#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace LightCpp::App::Debug {

/// One node of an MI value. MI has exactly four kinds and so does this.
class MiValue {
public:
    enum class Kind { Const, Tuple, List };

    MiValue() = default;

    static MiValue constant(QString text);
    static MiValue tuple();
    static MiValue list();

    [[nodiscard]] Kind kind() const noexcept { return kind_; }

    /// The raw text of a Const, with MI's C-style escapes already decoded.
    [[nodiscard]] const QString& text() const noexcept { return text_; }

    /// Tuple members *and* list items, in stream order. Named lookup only
    /// makes sense for tuples; list items have an empty name.
    [[nodiscard]] const QList<QPair<QString, MiValue>>& children() const noexcept
    {
        return children_;
    }

    /// Named member of a tuple (empty when absent or when this is not a
    /// tuple). Names may repeat in malformed input; the first wins.
    [[nodiscard]] const MiValue* find(const QString& name) const;
    /// Member or empty text — the shape almost every call site wants.
    [[nodiscard]] QString value(const QString& name) const;
    /// A numeric member, 0 when absent or not a number.
    [[nodiscard]] long long number(const QString& name) const;

    // --- construction, used by the parser and by the tests ----------------
    void setKind(Kind kind) noexcept { kind_ = kind; }
    void setText(QString text) { text_ = std::move(text); }
    void append(QString name, MiValue value);

private:
    Kind kind_ = Kind::Const;
    QString text_;
    QList<QPair<QString, MiValue>> children_;
};

/// One line of GDB/MI output. MI defines seven record kinds; a line the
/// grammar does not describe parses as `Unknown` with `text` intact, so the
/// consumer can still log it verbatim.
struct MiRecord {
    enum class Kind {
        Result,    ///< `[token]^done,results`
        ExecAsync, ///< `*stopped,results`
        StatusAsync,   ///< `+download`
        NotifyAsync,   ///< `=thread-group-added,id="i1"`
        Console,   ///< `~"text"` (debugger console text)
        Target,    ///< `@"text"` (the debuggee's own stdout)
        Log,       ///< `&"text"` (debugger log)
        Prompt,    ///< `(gdb)`
        Unknown,
    };

    Kind kind = Kind::Unknown;
    /// `^done` / `*stopped` / `=thread-created` / ... : the record class word
    /// without its sigil.
    QString name;
    /// The token in front of the sigil, empty when the command was untagged.
    QString token;
    /// The trailing results after the class word (`reason="exited",code="0"`).
    MiValue results;
    /// For Console / Target / Log / Unknown: the decoded payload.
    QString text;

    /// `reason` of an async record — the field every consumer branches on.
    [[nodiscard]] QString reason() const { return results.value(QStringLiteral("reason")); }
};

/// Parse one MI line (the trailing newline may be present or not).
[[nodiscard]] MiRecord parseMiRecord(const QString& line);

/// Parse an MI value starting at `pos`; `pos` is advanced past it.
[[nodiscard]] MiValue parseMiValue(const QString& text, int& pos);

/// Unescape one MI C string body (`a\"b\\n` -> `a"b\n`). Also used for cdb
/// symbol output, which quotes the same way.
[[nodiscard]] QString unescapeC(const QString& body);

/// Escape a value for inclusion inside a quoted MI string.
[[nodiscard]] QString escapeC(const QString& value);

// ---------------------------------------------------------------------------
// Records -> view snapshots
// ---------------------------------------------------------------------------

/// One frame of the call stack.
struct FrameInfo {
    int level = -1;
    QString address;   ///< "0x....", empty when unknown
    QString function;  ///< "main" / "ns::f(int)" / "ns::f" depending on backend
    QString module;    ///< "app" / "msvcprog!main" prefix split off
    QString file;      ///< normalized, empty when the frame has no source
    int line = 0;      ///< 1-based as the debugger reported it; 0 = none
};

/// A thread as the debugger sees it.
struct ThreadInfo {
    int id = 0;
    QString name;      ///< "main" / "worker" (empty when unnamed)
    QString state;     ///< "stopped" / "running" / "running-in-os-thread"
    QString frameFunction; ///< innermost function, when the backend said so
    QString frameFile;
    QString frameFullName; ///< GDB's file:line for the innermost frame
    int frameLine = 0;
    bool isCurrent = false;
};

/// One register.
struct RegisterInfo {
    int number = -1;   ///< -1 when the backend names registers only
    QString name;      ///< "rax" / "rip"
    QString value;     ///< as printed by the backend
};

/// A local / argument / watch entry. `value` keeps the backend's rendering
/// for primitives and is expanded into `children` for aggregates.
struct VariableInfo {
    QString name;
    QString value;
    QString type;
    bool hasChildren = false;
    int childCount = 0;
    QList<VariableInfo> children;
};

/// A memory row. `values` holds `wordSize` bytes worth of text; `address` is
/// the first byte's address.
struct MemoryRow {
    QString address;
    QByteArray bytes;
};

/// Parse the value of `-stack-list-frames` / `-stack-list-frames N M`.
/// Both the flat (`frame={...}`) and the nested-by-thread form are accepted.
[[nodiscard]] QList<FrameInfo> framesFromMi(const MiValue& stack);

/// One line of `-data-disassemble` output.
struct DisassemblyLine {
    QString address;
    QString function;   ///< `<main+4>` content, empty outside a function
    QString instruction; ///< "mov eax, 0x0"
};

}  // namespace LightCpp::App::Debug
