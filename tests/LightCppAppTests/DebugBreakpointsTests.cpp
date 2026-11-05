// DebugBreakpointsTests — the eight breakpoint kinds across three dialects.
//
// The interesting part is not "does it produce a string" but "does each
// string say what that debugger actually accepts". Each expectation below is
// tied to a documented flag:
//
//   gdb  -break-insert -t  (temporary)
//        -break-insert -c "x > 3"  (condition)
//        -break-insert -i 5  (ignore the next 5 hits; GDB calls it
//                              "ignore count" and -break-after sets it later)
//        -break-insert -d  (create disabled)
//        -break-commands N "printf ..." "continue"  (logpoint)
//        rwatch/awatch via -interpreter-exec (no MI form for read watchpoints)
//   lldb breakpoint set --name / --address / --file --line
//        --condition "x > 3" --ignore-count 5 --one-shot
//        watchpoint set expression --size 8 -- &x
//        breakpoint command add -o 'printf ...' -o continue
//   cdb  bp /1 `file:line`   (one-shot)
//        bp <addr> / bp <sym>
//        ba w4 &x  (break on access, 4-byte write watchpoint)
//
// The model's serialisation is pinned too: a session file that loses a
// condition would silently change what the user debugged next time.

#include <gtest/gtest.h>

#include "LightCpp/App/Debug/DebugBreakpoints.hpp"

namespace {

using LightCpp::App::Debug::Breakpoint;
using LightCpp::App::Debug::BreakpointKind;
using LightCpp::App::Debug::BreakpointModel;
using LightCpp::App::Debug::MemoryMode;
using LightCpp::App::DebuggerController;

Breakpoint lineBreakpoint(const QString& file, int line)
{
    Breakpoint bp;
    bp.kind = BreakpointKind::Line;
    bp.file = file;
    bp.line = line;
    return bp;
}

}  // namespace

TEST(DebugBreakpoints, ALineBreakpointIsOneBasedInEveryDialect)
{
    const Breakpoint bp = lineBreakpoint(QStringLiteral("E:/src/a.cpp"), 7);
    EXPECT_EQ(bp.location(), QStringLiteral("E:/src/a.cpp:8"));
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Gdb, bp),
              QStringList{QStringLiteral("-break-insert \"E:/src/a.cpp:8\"")});
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Cdb, bp),
              QStringList{QStringLiteral("bp `E:/src/a.cpp:8`")});
    // LLDB sets by file *name* and line, never by path:line.
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Lldb, bp),
              QStringList{QStringLiteral("breakpoint set --file \"a.cpp\" --line 8")});
}

TEST(DebugBreakpoints, GdbConditionAndIgnoreCountRideOnTheInsert)
{
    Breakpoint bp = lineBreakpoint(QStringLiteral("a.cpp"), 2);
    bp.condition = QStringLiteral("i > 3");
    bp.ignoreCount = 5;
    const QStringList commands =
        LightCpp::App::Debug::armCommands(DebuggerController::Backend::Gdb, bp);
    ASSERT_EQ(commands.size(), 1);
    EXPECT_TRUE(commands.first().contains(QStringLiteral("-c \"i > 3\"")));
    EXPECT_TRUE(commands.first().contains(QStringLiteral("-i 5")));
    EXPECT_TRUE(commands.first().startsWith(QStringLiteral("-break-insert")));
}

TEST(DebugBreakpoints, ATemporaryBreakpointUsesEachBackendsOneShotForm)
{
    Breakpoint bp = lineBreakpoint(QStringLiteral("a.cpp"), 2);
    bp.temporary = true;
    EXPECT_TRUE(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Gdb, bp)
                    .first()
                    .contains(QStringLiteral(" -t")));
    EXPECT_TRUE(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Lldb, bp)
                    .first()
                    .contains(QStringLiteral("--one-shot")));
    EXPECT_TRUE(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Cdb, bp)
                    .first()
                    .startsWith(QStringLiteral("bp /1")));
}

TEST(DebugBreakpoints, AFunctionBreakpointHasNoFileOrLine)
{
    Breakpoint bp;
    bp.kind = BreakpointKind::Function;
    bp.expression = QStringLiteral("Widget::paint");
    EXPECT_EQ(bp.location(), QStringLiteral("func Widget::paint"));
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Gdb, bp),
              QStringList{QStringLiteral("-break-insert \"Widget::paint\"")});
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Lldb, bp),
              QStringList{QStringLiteral("breakpoint set --name \"Widget::paint\"")});
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Cdb, bp),
              QStringList{QStringLiteral("bp Widget::paint")});
}

TEST(DebugBreakpoints, AnAddressBreakpointIsItsOwnLocation)
{
    Breakpoint bp;
    bp.kind = BreakpointKind::Address;
    bp.expression = QStringLiteral("0x401000");
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Gdb, bp),
              QStringList{QStringLiteral("-break-insert \"0x401000\"")});
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Lldb, bp),
              QStringList{QStringLiteral("breakpoint set --address 0x401000")});
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Cdb, bp),
              QStringList{QStringLiteral("bp 0x401000")});
}

TEST(DebugBreakpoints, AMemoryWatchUsesTheWatchVerbPerBackend)
{
    Breakpoint bp;
    bp.kind = BreakpointKind::Memory;
    bp.expression = QStringLiteral("&counter");
    bp.memorySize = 4;

    // GDB has no MI record for read watchpoints: the console interpreter.
    const QStringList gdb =
        LightCpp::App::Debug::armCommands(DebuggerController::Backend::Gdb, bp);
    ASSERT_EQ(gdb.size(), 1);
    EXPECT_TRUE(gdb.first().contains(QStringLiteral("watch &counter")));

    bp.memoryMode = MemoryMode::Read;
    EXPECT_TRUE(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Gdb, bp)
                    .first()
                    .contains(QStringLiteral("rwatch")));

    bp.memoryMode = MemoryMode::Write;
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Lldb, bp),
              QStringList{QStringLiteral("watchpoint set expression --size 4 -- &counter")});
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Cdb, bp),
              QStringList{QStringLiteral("ba w4 &counter")});
}

TEST(DebugBreakpoints, ACdbWatchSizeIsClampedToTheHardwareWidths)
{
    Breakpoint bp;
    bp.kind = BreakpointKind::Memory;
    bp.expression = QStringLiteral("&x");
    bp.memorySize = 8;
    EXPECT_EQ(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Cdb, bp),
              QStringList{QStringLiteral("ba w4 &x")});
}

TEST(DebugBreakpoints, ALogpointContinuesInsteadOfStopping)
{
    Breakpoint bp = lineBreakpoint(QStringLiteral("a.cpp"), 4);
    bp.logpoint = true;
    bp.condition = QStringLiteral("i = {i}");
    const QStringList commands =
        LightCpp::App::Debug::gdbLogpointCommands(bp);
    ASSERT_EQ(commands.size(), 2);
    EXPECT_TRUE(commands.first().startsWith(QStringLiteral("printf \"")));
    EXPECT_EQ(commands.last(), QStringLiteral("continue"));

    const QStringList lldb =
        LightCpp::App::Debug::armCommands(DebuggerController::Backend::Lldb, bp);
    ASSERT_EQ(lldb.size(), 2);
    EXPECT_TRUE(lldb.last().startsWith(QStringLiteral("breakpoint command add -o")));
    EXPECT_TRUE(lldb.last().endsWith(QStringLiteral("-o continue")));
}

TEST(DebugBreakpoints, ADisabledBreakpointIsCreatedDisabledNotOmitted)
{
    Breakpoint bp = lineBreakpoint(QStringLiteral("a.cpp"), 4);
    bp.enabled = false;
    EXPECT_TRUE(LightCpp::App::Debug::armCommands(DebuggerController::Backend::Gdb, bp)
                    .first()
                    .contains(QStringLiteral(" -d")));
    const QStringList lldb =
        LightCpp::App::Debug::armCommands(DebuggerController::Backend::Lldb, bp);
    ASSERT_EQ(lldb.size(), 2);
    EXPECT_EQ(lldb.last(), QStringLiteral("breakpoint disable"));
}

TEST(DebugBreakpoints, OnlyGdbCanEditInPlace)
{
    EXPECT_TRUE(LightCpp::App::Debug::canEditInPlace(DebuggerController::Backend::Gdb));
    EXPECT_FALSE(LightCpp::App::Debug::canEditInPlace(DebuggerController::Backend::Lldb));
    EXPECT_FALSE(LightCpp::App::Debug::canEditInPlace(DebuggerController::Backend::Cdb));
}

TEST(DebugBreakpoints, AGdbEditAddressesTheBreakpointByNumber)
{
    Breakpoint bp = lineBreakpoint(QStringLiteral("a.cpp"), 4);
    bp.backendNumber = 3;
    bp.condition = QStringLiteral("n < 10");
    bp.ignoreCount = 2;
    const QStringList commands =
        LightCpp::App::Debug::editCommands(DebuggerController::Backend::Gdb, bp);
    EXPECT_TRUE(commands.contains(QStringLiteral("-break-enable 3")));
    EXPECT_TRUE(commands.contains(QStringLiteral("-break-condition 3 n < 10")));
    EXPECT_TRUE(commands.contains(QStringLiteral("-break-after 3 2")));

    bp.condition.clear();
    const QStringList cleared =
        LightCpp::App::Debug::editCommands(DebuggerController::Backend::Gdb, bp);
    // Clearing a condition must send GDB's "always true", not an empty
    // argument (which it rejects with a syntax error).
    EXPECT_TRUE(cleared.contains(QStringLiteral("-break-condition 3 1")));
}

TEST(DebugBreakpoints, AnEditWithoutANumberIsAReArmNotASilentNoOp)
{
    Breakpoint bp = lineBreakpoint(QStringLiteral("a.cpp"), 4);
    EXPECT_TRUE(LightCpp::App::Debug::editCommands(DebuggerController::Backend::Gdb, bp)
                    .isEmpty());
}

TEST(DebugBreakpoints, GdbRemovalByNumberTouchesOnlyThatBreakpoint)
{
    Breakpoint target = lineBreakpoint(QStringLiteral("a.cpp"), 4);
    target.backendNumber = 2;
    Breakpoint other = lineBreakpoint(QStringLiteral("a.cpp"), 9);
    other.backendNumber = 3;
    const QStringList commands = LightCpp::App::Debug::disarmCommands(
        DebuggerController::Backend::Gdb, target, {target, other});
    EXPECT_EQ(commands, QStringList{QStringLiteral("-break-delete 2")});
}

TEST(DebugBreakpoints, LldbAndCdbRemovalReArmsEverythingElseInOrder)
{
    Breakpoint a = lineBreakpoint(QStringLiteral("a.cpp"), 1);
    a.id = 1;
    Breakpoint b = lineBreakpoint(QStringLiteral("a.cpp"), 2);
    b.id = 2;
    Breakpoint c = lineBreakpoint(QStringLiteral("b.cpp"), 3);
    c.id = 3;

    const QStringList lldb = LightCpp::App::Debug::disarmCommands(
        DebuggerController::Backend::Lldb, b, {a, b, c});
    ASSERT_EQ(lldb.size(), 3);
    EXPECT_EQ(lldb.at(0), QStringLiteral("breakpoint delete -f"));
    EXPECT_TRUE(lldb.at(1).contains(QStringLiteral("--file \"a.cpp\" --line 2")));
    EXPECT_TRUE(lldb.at(2).contains(QStringLiteral("--file \"b.cpp\" --line 4")));

    const QStringList cdb = LightCpp::App::Debug::disarmCommands(
        DebuggerController::Backend::Cdb, b, {a, b, c});
    ASSERT_EQ(cdb.size(), 3);
    EXPECT_EQ(cdb.at(0), QStringLiteral("bc *"));
    EXPECT_EQ(cdb.at(1), QStringLiteral("bp `a.cpp:2`"));
}

TEST(DebugBreakpoints, TogglingTheSameLineTwiceLeavesOneEntry)
{
    BreakpointModel model;
    const int first = model.toggleLine(QStringLiteral("a.cpp"), 4);
    ASSERT_NE(first, 0);
    EXPECT_EQ(model.size(), 1);
    // Adding the same location again merges rather than duplicating.
    const int merged = model.add(lineBreakpoint(QStringLiteral("a.cpp"), 4));
    EXPECT_EQ(merged, first);
    EXPECT_EQ(model.size(), 1);
    // Toggling removes it.
    EXPECT_EQ(model.toggleLine(QStringLiteral("a.cpp"), 4), 0);
    EXPECT_TRUE(model.isEmpty());
}

TEST(DebugBreakpoints, OnlyEnabledEntriesArePlanted)
{
    BreakpointModel model;
    model.add(lineBreakpoint(QStringLiteral("a.cpp"), 1));
    Breakpoint disabled = lineBreakpoint(QStringLiteral("a.cpp"), 2);
    disabled.enabled = false;
    model.add(disabled);
    EXPECT_EQ(model.enabledItems().size(), 1);
    EXPECT_EQ(model.size(), 2);
}

TEST(DebugBreakpoints, ASessionFileRoundTripsEveryField)
{
    BreakpointModel model;
    Breakpoint conditional = lineBreakpoint(QStringLiteral("dir/a|b.cpp"), 5);
    conditional.condition = QStringLiteral("x == 1 && y|z");
    conditional.ignoreCount = 3;
    conditional.temporary = true;
    model.add(conditional);

    Breakpoint watch;
    watch.kind = BreakpointKind::Memory;
    watch.expression = QStringLiteral("&g[0]");
    watch.memoryMode = MemoryMode::ReadWrite;
    watch.memorySize = 8;
    model.add(watch);

    Breakpoint func;
    func.kind = BreakpointKind::Function;
    func.expression = QStringLiteral("ns::f(int)");
    func.logpoint = true;
    func.enabled = false;
    model.add(func);

    BreakpointModel restored;
    restored.deserialize(model.serialize());
    ASSERT_EQ(restored.size(), 3);

    const auto& items = restored.items();
    EXPECT_EQ(items.at(0).file, QStringLiteral("dir/a|b.cpp"));
    EXPECT_EQ(items.at(0).line, 5);
    EXPECT_EQ(items.at(0).condition, QStringLiteral("x == 1 && y|z"));
    EXPECT_EQ(items.at(0).ignoreCount, 3);
    EXPECT_TRUE(items.at(0).temporary);

    EXPECT_EQ(items.at(1).kind, BreakpointKind::Memory);
    EXPECT_EQ(items.at(1).expression, QStringLiteral("&g[0]"));
    EXPECT_EQ(items.at(1).memoryMode, MemoryMode::ReadWrite);
    EXPECT_EQ(items.at(1).memorySize, 8);

    EXPECT_EQ(items.at(2).kind, BreakpointKind::Function);
    EXPECT_FALSE(items.at(2).enabled);
    EXPECT_TRUE(items.at(2).logpoint);
}

TEST(DebugBreakpoints, AMalformedSessionLineIsSkippedNotFatal)
{
    BreakpointModel model;
    model.deserialize({QStringLiteral("garbage"),
                       QStringLiteral("line|broken|cond|0|"),
                       QStringLiteral("line|a.cpp:3||0|")});
    EXPECT_EQ(model.size(), 1);
    EXPECT_EQ(model.lineKeys(), QStringList{QStringLiteral("a.cpp:3")});
}

TEST(DebugBreakpoints, UpdatingAnEntryKeepsItsKindAndLocation)
{
    BreakpointModel model;
    const int id = model.add(lineBreakpoint(QStringLiteral("a.cpp"), 4));
    Breakpoint fields;
    fields.condition = QStringLiteral("i > 0");
    fields.enabled = false;
    fields.expression = QStringLiteral("ignored for a line breakpoint");
    EXPECT_TRUE(model.update(id, fields));
    const Breakpoint* stored = model.find(id);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(stored->condition, QStringLiteral("i > 0"));
    EXPECT_FALSE(stored->enabled);
    EXPECT_EQ(stored->line, 4);
    EXPECT_EQ(stored->file, QStringLiteral("a.cpp"));
    EXPECT_FALSE(model.update(999, fields));
}

TEST(DebugBreakpoints, ALabelCarriesTheModifiersAndTheHitCount)
{
    Breakpoint bp = lineBreakpoint(QStringLiteral("a.cpp"), 4);
    bp.condition = QStringLiteral("i>0");
    bp.ignoreCount = 2;
    bp.hitCount = 7;
    const QString label = bp.label();
    EXPECT_TRUE(label.startsWith(QStringLiteral("a.cpp:5")));
    EXPECT_TRUE(label.contains(QStringLiteral("if i>0")));
    EXPECT_TRUE(label.contains(QStringLiteral("ignore 2")));
    EXPECT_TRUE(label.contains(QStringLiteral("hit 7")));
}
