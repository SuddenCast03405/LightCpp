// DebugViews — turn one backend answer into the structs the panels render.
//
// Every function here takes the *text* a debugger printed and returns data.
// None of them starts a process, so the whole view layer is testable against
// captured transcripts — which is how the transcripts in
// tests/LightCppAppTests/DebugViewTests.cpp were produced (gdb 13, lldb 18,
// cdb 10.0, real sessions on a small C++ program).
//
// The three backends disagree about nearly everything:
//
//   locals      gdb   `-stack-list-variables --all-values` -> variables=[{name=,arg=,value=}]
//               lldb  `frame variable` -> "    (int) x = 3"
//               cdb   `dv /t /v` -> "    int x = 0n3"
//   stack       gdb   `-stack-list-frames` (see framesFromMi)
//               lldb  `thread backtrace` -> "frame #0: 0x... app`main at a.cpp:8:5"
//               cdb   `k` -> "ChildEBP RetAddr  Args to Child ...  app!main+0x4 [a.cpp @ 8]"
//   threads     gdb   `-thread-info` -> threads=[{id=,target-id=,name=,state=,frame=}]
//               lldb  `thread list` -> "thread #1: tid = 0x1f0f, 0x... app`main ... "
//               cdb   `~*k` -> "   0  id: 1234 create child ... "
//   registers   gdb   `-data-list-register-values x` -> register-values=[{number=,value=}]
//               lldb  `register read` -> "     rax = 0x0000000000000000"
//               cdb   `r` -> "rax=0000000000000000 rbx=..."
//   memory      gdb   `-data-read-memory-bytes` -> memory=[{begin=,offset=,contents=}]
//               lldb  `memory read -c <n> <addr>` -> "0x...: 01 02 03 ..."
//               cdb   `db <addr> L?<n>` -> "00000000`0012ff00  01 02 ..."
//   disassembly gdb   `-data-disassemble -s .. -e .. -- 0` -> asm_insns=[{address=,func-name=,inst=}]
//               lldb  `disassemble -n main` -> "app`main:\n->  0x... <+0>: push rbp"
//               cdb   `u` -> "app!main+0x4:\n00007ff6`... 4883ec28  sub rsp,28h"
//
// A parser that returns an empty list for input it does not understand is
// correct here: the panel shows a "no answer" row rather than inventing one.

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "LightCpp/App/Debug/GdbMi.hpp"

namespace LightCpp::App::Debug {

/// Which console dialect a transcript came from.
enum class Backend { Unknown, Gdb, Lldb, Cdb };

/// Parse `-stack-list-variables --all-values --no-values`'s answer (or the
/// `variables` member of it) into locals/arguments. `scope` filters by the
/// `arg="1"` flag: 0 = locals only, 1 = arguments only, -1 = both.
///
/// A `value` of `{...}` or `[...]` (an aggregate printed without
/// `--simple-values`) is kept verbatim and flagged `hasChildren`, with
/// `childCount` counted at the top level so the tree can show a twisty
/// without a second round trip.
[[nodiscard]] QList<VariableInfo> parseGdbVariables(const MiValue& variables,
                                                    int scope = -1);

/// Parse GDB's `-var-list-children --all-values <name>` answer.
[[nodiscard]] QList<VariableInfo> parseGdbChildren(const MiValue& children);

/// LLDB `frame variable` output.
[[nodiscard]] QList<VariableInfo> parseLldbVariables(const QString& text);

/// cdb `dv /t /v` output.
[[nodiscard]] QList<VariableInfo> parseCdbVariables(const QString& text);

/// GDB `-thread-info`.
[[nodiscard]] QList<ThreadInfo> parseGdbThreads(const MiValue& threads);

/// LLDB `thread list`.
[[nodiscard]] QList<ThreadInfo> parseLldbThreads(const QString& text);

/// cdb `~*k` (all threads, stack form) or `~*` (one line each).
[[nodiscard]] QList<ThreadInfo> parseCdbThreads(const QString& text);

/// GDB `-data-list-register-values <fmt>`, with `names` resolved from
/// `-data-list-register-names` (`names` may be shorter than the answer, in
/// which case the register keeps its number and an empty name).
[[nodiscard]] QList<RegisterInfo> parseGdbRegisters(const MiValue& values,
                                                    const QStringList& names);

/// LLDB `register read`.
[[nodiscard]] QList<RegisterInfo> parseLldbRegisters(const QString& text);

/// cdb `r` (one or more "name=value" groups per line).
[[nodiscard]] QList<RegisterInfo> parseCdbRegisters(const QString& text);

/// GDB `-data-read-memory-bytes` answer: memory=[{begin=,offset=,contents=}]
/// Each entry becomes one row of `bytes` (the hex string decoded to bytes).
[[nodiscard]] QList<MemoryRow> parseGdbMemory(const MiValue& memory);

/// LLDB `memory read -c N addr` / cdb `db addr L?N`.
[[nodiscard]] QList<MemoryRow> parseHexDump(const QString& text);

/// GDB `-data-disassemble ... -- 0` (asm_insns) or `-- 1` (src_and_asm_line).
[[nodiscard]] QList<DisassemblyLine> parseGdbDisassembly(const MiValue& asmInsns);

/// LLDB `disassemble` and cdb `u` output.
[[nodiscard]] QList<DisassemblyLine> parseTextDisassembly(const QString& text,
                                                          Backend backend);

/// A parsed `file:line` from any backend's frame text. Kept separate so the
/// "did we recognise the position?" decision is one function.
[[nodiscard]] bool parseFramePosition(const QString& text, Backend backend,
                                      QString& file, int& oneBasedLine);

/// Split `msvcprog!main+0x1f` into module ("msvcprog") and function ("main").
/// Returns false when there is no `!`.
[[nodiscard]] bool splitCdbSymbol(const QString& text, QString& module,
                                  QString& function);

}  // namespace LightCpp::App::Debug
