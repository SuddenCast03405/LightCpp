// DebugSymbols — the symbol layer of the debugger (lightcpp.1.md §五:
// "符号加载", "源码映射", "pretty-printer", "STL 容器展开", "智能指针展开",
// "枚举显示", plus natvis).
//
// All three backends can already show a variable tree; what they cannot do is
// agree on how to ask for a *readable* tree. Without pretty-printing, a
// std::vector<int> with three elements prints as
// `{_M_start=0x…, _M_finish=0x…, _M_end_of_storage=0x…}` in gdb and as
// `{__begin_=…, __end_=…}` in lldb — the same program, two different walls of
// pointers. The commands that turn that into `{1, 2, 3}` are one line per
// backend, they are easy to get subtly wrong (gdb's `-enable-pretty-printing`
// is an MI command and must not be written as a console command), and they
// must be issued *after* the inferior exists but *before* the first frame is
// read, otherwise the tree the user is looking at stays in raw form.
//
// This module is the mapping from "what the user asked for" to "the exact
// command for this backend", with no process and no UI, so it can be tested.

#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::Debug {

/// The three debugger dialects the IDE drives.
enum class Backend {
    Unknown = 0,
    Gdb,
    Lldb,
    Cdb,
};

[[nodiscard]] QString backendName(Backend backend);
[[nodiscard]] Backend backendFromName(const QString& name);

/// A source-path rewriting rule: what the debug info says → where the file
/// really is on this machine (a build server's `/build/src` → `C:/proj/src`).
struct PathMapping {
    QString from;
    QString to;

    [[nodiscard]] bool isValid() const { return !from.isEmpty() && !to.isEmpty(); }
};

/// Everything the symbol layer can be told to do.
struct SymbolSettings {
    bool prettyPrinting = true;   ///< STL containers, smart pointers, enums
    bool showEnumerators = true;  ///< print `Color::Red` rather than `1`
    bool dynamicType = true;      ///< resolve the real type of a polymorphic object
    QList<PathMapping> sourceMappings;
    QStringList symbolSearchPaths;   ///< extra directories for .debug / .pdb
    QStringList natvisFiles;         ///< cdb only; harmless elsewhere
    QStringList symbolFiles;         ///< extra binaries to load symbols from
    /// Guard against a pathological settings file: 200 substitute-path rules
    /// make every frame lookup slow, and nobody writes 200 by hand.
    static constexpr int kMaxMappings = 64;
    static constexpr int kMaxPaths = 64;
};

/// The commands that apply `settings` for `backend`, in the order they must be
/// sent: path rewriting first (it affects every later file lookup), then
/// symbol locations, then the display switches.
///
/// Empty entries are omitted, and a backend that has no equivalent for a
/// setting returns nothing for it rather than a wrong command — cdb has no
/// gdb-style pretty-printer, so `prettyPrintCommands(Cdb, …)` is empty and the
/// caller's log shows nothing was attempted.
[[nodiscard]] QStringList startupCommands(Backend backend,
                                          const SymbolSettings& settings);

/// `-enable-pretty-printing` (gdb), `type synthetic enable` (lldb), none (cdb).
[[nodiscard]] QStringList prettyPrintCommands(Backend backend, bool enabled);

/// `set print pretty on|off` (gdb) / `settings set target.enable-synthetic-value`
/// (lldb). `showEnumerators` additionally asks gdb not to print enums as
/// numbers.
[[nodiscard]] QStringList enumDisplayCommands(Backend backend, bool enabled);

/// `set print object on|off` (gdb) / `settings set target.prefer-dynamic-value`
/// (lldb) / `.exepath`-independent `dx` default (cdb, left alone).
[[nodiscard]] QStringList dynamicTypeCommands(Backend backend, bool enabled);

/// `set substitute-path FROM TO` (gdb) / `settings set target.source-map FROM TO`
/// (lldb) / `.srcpath+ PATH` (cdb).
///
/// gdb's form takes exactly two arguments, so both are quoted; a Windows path
/// with a drive letter keeps its colon because gdb treats the pair as two
/// whitespace-separated words rather than as `a:b`.
[[nodiscard]] QStringList sourceMappingCommands(
    Backend backend, const QList<PathMapping>& mappings);

/// Where to look for separate debug info: `set debug-file-directory` and
/// `set solib-search-path` (gdb), `settings set target.debug-file-search-paths`
/// and `target.exec-search-paths` (lldb), `.sympath+` (cdb).
[[nodiscard]] QStringList symbolPathCommands(Backend backend,
                                             const QStringList& paths);

/// One natvis file, once the search order has been applied.
struct NatvisFile {
    QString path;
    /// True when this file shadows a visualiser an earlier file already
    /// defined for the same type.
    bool overrides = false;

    [[nodiscard]] bool operator==(const NatvisFile& other) const
    {
        return path == other.path && overrides == other.overrides;
    }
};

/// Where a natvis file came from. The order below is the precedence order,
/// lowest first: a user's file beats a project's, which beats the one the
/// toolchain ships, and a private file beats a public one at every level —
/// the same rule Visual Studio documents (`/public` then `/private`, with
/// later files winning), and the reason it matters is that the *winner* is
/// what a developer sees when they are debugging why their visualiser is
/// ignored.
enum class NatvisOrigin {
    Toolchain = 0,  ///< the debugger's own autoexp.dat / *.natvis directory
    Public,         ///< shared with the team, checked in
    Private,        ///< the viewer's own, not to be shared
    Project,        ///< declared by the project file
    User,           ///< the user's IDE settings
};

[[nodiscard]] QString natvisOriginName(NatvisOrigin origin);

/// A file plus what it claims: its origin and the types it defines.
struct NatvisCandidate {
    QString path;
    NatvisOrigin origin = NatvisOrigin::User;
    /// Fully qualified type names the file has a `<Type Name="…">` for. Empty
    /// is allowed and means "unknown", which sorts *below* a known claim:
    /// a file that says it handles `std::vector<int>` beats one that might.
    QStringList types;
};

/// Apply the search order to `candidates`.
///
/// Sorting is stable on origin (lowest first), then on path, then on input
/// position, so the result is deterministic regardless of how the settings
/// were assembled. Types a later file redefines mark that file `overrides`,
/// which is what the UI shows next to it — a silent shadowing is the single
/// most confusing thing about natvis.
[[nodiscard]] QList<NatvisFile> orderNatvis(const QList<NatvisCandidate>& candidates);

/// The types in `candidates` that more than one file defines, each mapped to
/// the paths that define it, in precedence order. For the "your file is
/// being overridden" warning.
[[nodiscard]] QList<QPair<QString, QStringList>> natvisConflicts(
    const QList<NatvisCandidate>& candidates);

/// The type names a natvis document defines (`<Type Name="…">`). A best
/// effort: the format is XML and the parser here is a scan, because the file
/// may be malformed and a debugger must not refuse to start over it.
[[nodiscard]] QStringList natvisTypesIn(const QString& xml);

/// natvis visualisers (cdb: `.nvload <file>`; lldb has type summaries instead,
/// gdb has neither). Files are loaded in `orderNatvis` order when the caller
/// passes candidates through `natvisCommands(candidates)`.
[[nodiscard]] QStringList natvisCommands(Backend backend,
                                         const QStringList& files);

/// The same, applying the search order first. This is the overload the
/// startup path uses, because loading the user's file before the toolchain's
/// would let the toolchain win.
[[nodiscard]] QStringList natvisCommands(Backend backend,
                                         const QList<NatvisCandidate>& candidates);

/// Load symbols from an extra binary: `add-symbol-file` (gdb),
/// `target symbols add` (lldb), `.reload /f` (cdb).
[[nodiscard]] QStringList loadSymbolCommands(Backend backend,
                                             const QStringList& files);

/// Human-readable description of what `settings` will do, for the settings
/// page's summary line. Never empty.
[[nodiscard]] QString describe(const SymbolSettings& settings);

/// Quote one argument for a debugger console command. Debugger consoles split
/// on spaces and have no portable quoting rule, so a path containing a space
/// is wrapped in double quotes with inner quotes escaped — the form all three
/// accept for a path argument.
[[nodiscard]] QString quoteArgument(const QString& argument);

/// gdb's `set substitute-path` wants the two paths as separate words, so a
/// pair is rendered as `set substitute-path "<from>" "<to>"`.
[[nodiscard]] QString substitutePathCommand(const PathMapping& mapping);

}  // namespace LightCpp::Workbench::Debug
