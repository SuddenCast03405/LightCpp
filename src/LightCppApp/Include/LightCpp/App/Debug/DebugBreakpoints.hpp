// DebugBreakpoints — the breakpoint model of a debug session: what the user
// asked for, and what the backend accepted.
//
// The 0.2.0 controller could only plant `file:line`. lightcpp.1.md asks for
// eight kinds (line, conditional, logpoint, hit count, temporary, function,
// address, memory watch) across three backends whose dialects disagree about
// every one of them. This class owns that disagreement:
//
//   * A request is backend-independent — `Breakpoint` says *what* to stop on.
//   * `commandsToArm()` / `commandsToDisarm()` are the dialect: the exact
//     lines to type into the debugger console for one request.
//   * The model survives a session (the window replays it into the next one),
//     and the controller only records what the backend confirmed
//     (`markArmed`, `markDisarmed`), so the UI never shows a breakpoint the
//     debugger does not have.
//
// License and failure notes worth keeping:
//   * GDB enumerates breakpoints (`-break-list`), so a single request can be
//     updated in place (`-break-condition N expr`) or deleted by number.
//   * LLDB cannot delete one breakpoint by location either, and its ids are
//     not addressable from a location: disarming one re-arms the rest, which
//     is why the model holds the ordered list rather than a set.
//   * cdb has `bp /1` (one-shot), `bp <addr>`, `ba r/w1/w2/w4` (memory) and
//     `bp <sym>` (function), and `be`/`bd` to re-enable/disable by id — but a
//     source-line breakpoint's id is never printed by `bp`, so removing one
//     there clears all (`bc *`) and re-arms the rest, exactly like LLDB.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "LightCpp/App/DebuggerController.hpp"

namespace LightCpp::App::Debug {

/// What a breakpoint stops on. The enum is the model's discriminator; the
/// unused fields for a kind are ignored, which keeps the struct copyable and
/// comparable without a variant.
enum class BreakpointKind {
    Line,      ///< file:line
    Function,  ///< function name (all overloads as the debugger resolves it)
    Address,   ///< "0x401000" or "main+16"
    Memory,    ///< watch an address range / expression
};

/// Where a memory breakpoint watches.
enum class MemoryMode { Write, Read, ReadWrite, Execute };

struct Breakpoint {
    int id = 0;             ///< model id, unique for the session's lifetime
    bool enabled = true;
    BreakpointKind kind = BreakpointKind::Line;

    // Line
    QString file;           ///< normalized path, empty for a pending line
    int line = 0;           ///< 0-based, like every editor coordinate here

    // Function / Address / Memory
    QString expression;     ///< "main" / "0x401000" / "&counter" / "arr[0]"

    // Any kind
    QString condition;      ///< empty = unconditional
    int ignoreCount = 0;    ///< stop after this many more hits
    bool temporary = false; ///< delete after the first stop
    bool logpoint = false;  ///< do not stop, print and continue
    MemoryMode memoryMode = MemoryMode::Write;
    int memorySize = 8;     ///< bytes for MemoryMode::Read/ReadWrite

    /// The backend's own number for this breakpoint once it is armed; -1
    /// while pending (and always -1 for the backends that do not report one).
    int backendNumber = -1;
    /// How many times the debugger says it hit.
    int hitCount = 0;

    /// "file:line" / "func main()" / "@0x401000" / "watch &counter" — the
    /// one-line rendering the UI and the log use.
    [[nodiscard]] QString label() const;
    /// The location part only, in the dialect-neutral form the backends
    /// accept ("path:line", "main", "0x401000", "&counter").
    [[nodiscard]] QString location() const;
    /// Extra attributes appended after the location in a label.
    [[nodiscard]] QString decorations() const;
};

/// The session's breakpoint list, in the order the user created them.
class BreakpointModel {
public:
    /// Add `bp` (id assigned when zero) and return its id. A line request for
    /// a location that already has one is merged into the existing entry —
    /// F9 on the same line twice must not plant two breakpoints.
    int add(Breakpoint bp);

    /// Remove by model id. Returns true when something was removed.
    bool remove(int id);

    /// Replace every settable field of `id` except kind and location.
    /// Returns true when the entry exists.
    bool update(int id, const Breakpoint& fields);

    /// The entry with `id`, or nullptr.
    [[nodiscard]] const Breakpoint* find(int id) const;
    [[nodiscard]] Breakpoint* findMutable(int id);

    /// The entry planted at file:line, or nullptr.
    [[nodiscard]] const Breakpoint* findLine(const QString& file, int line) const;

    /// The line request for file:line, added when absent.
    int toggleLine(const QString& file, int line);

    [[nodiscard]] const QList<Breakpoint>& items() const noexcept { return items_; }
    [[nodiscard]] bool isEmpty() const noexcept { return items_.isEmpty(); }
    [[nodiscard]] qsizetype size() const noexcept { return items_.size(); }

    /// Entries the backend should have armed, in planting order.
    [[nodiscard]] QList<Breakpoint> enabledItems() const;

    /// Record what the backend answered for `id`.
    void markArmed(int id, int backendNumber);
    void markDisarmed(int id);

    /// "file:line" keys of the line breakpoints — the format the editor
    /// gutters and the session file use.
    [[nodiscard]] QStringList lineKeys() const;

    /// Load from "kind|location|condition|ignore|flags" lines; skips text it
    /// does not understand. See serialize().
    void deserialize(const QStringList& lines);
    /// One line per entry: `line|<file>:<line>|<cond>|<ignore>|<flags>`,
    /// `function|<expr>|...`, `address|<expr>|...`, `memory|<mode>:<size>:<expr>|...`.
    /// `|` and newlines inside fields are escaped as `\p` and `\n`.
    [[nodiscard]] QStringList serialize() const;

    void clear();

private:
    QList<Breakpoint> items_;
    int nextId_ = 1;
};

// ---------------------------------------------------------------------------
// Dialect: one breakpoint -> console commands
// ---------------------------------------------------------------------------

/// The commands that arm `bp` from scratch.
[[nodiscard]] QStringList armCommands(DebuggerController::Backend backend,
                                      const Breakpoint& bp);

/// The commands that take `bp` back out of a live session while leaving
/// `stillArmed` planted, in the order they must be written.
[[nodiscard]] QStringList disarmCommands(DebuggerController::Backend backend,
                                         const Breakpoint& bp,
                                         const QList<Breakpoint>& stillArmed);

/// The commands that re-apply condition / ignore count / enable state to a
/// breakpoint the backend already knows (by number for GDB, by index for
/// LLDB, by id for cdb). Empty when the backend cannot edit in place — the
/// caller then disarms and re-arms instead.
[[nodiscard]] QStringList editCommands(DebuggerController::Backend backend,
                                       const Breakpoint& bp);

/// True when `backend` can change a breakpoint in place. GDB can; LLDB and
/// cdb cannot express "the one at file:line" as a command argument.
[[nodiscard]] bool canEditInPlace(DebuggerController::Backend backend);

/// GDB's `-break-insert` flags for `bp`: `-t` temporary, `-c cond`,
/// `-i ignore`, `-d` disabled.
[[nodiscard]] QString gdbArmFlags(const Breakpoint& bp);

/// The logpoint's `-break-commands` payload for GDB: `printf` plus
/// `continue`, or just `continue` when the user typed no message. Empty for
/// a non-logpoint.
[[nodiscard]] QStringList gdbLogpointCommands(const Breakpoint& bp);

/// The message a logpoint prints. `{expr}` placeholders are expanded the
/// way GDB/LLDB/cdb all accept: as raw debugger expressions evaluated at the
/// stop point. A message without placeholders prints verbatim.
[[nodiscard]] QString logpointMessage(const Breakpoint& bp);

}  // namespace LightCpp::App::Debug
