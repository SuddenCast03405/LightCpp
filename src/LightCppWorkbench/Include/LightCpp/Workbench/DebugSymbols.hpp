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

/// natvis visualisers (cdb: `.nvload <file>`; lldb has type summaries instead,
/// gdb has neither).
[[nodiscard]] QStringList natvisCommands(Backend backend,
                                         const QStringList& files);

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
