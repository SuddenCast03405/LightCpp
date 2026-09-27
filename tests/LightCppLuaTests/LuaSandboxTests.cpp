// LuaSandboxTests — the sandbox contract: budget enforcement, the standard
// library whitelist, print rerouting and Lua 5.4 semantics.

#include "LightCpp/Lua/LuaSandbox.hpp"

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>
#include <limits>

namespace {

constexpr char kResultOf[] = "RESULT:";

/// Runs a snippet and returns its runtime error message — a quick way to
/// move values out of the sandbox is to raise them: error(value).
QString errorMessageOf(const QString& code,
                       const LightCpp::Lua::SandboxLimits& limits = {})
{
    LightCpp::Lua::LuaSandbox sandbox(limits);
    const auto result = sandbox.runString(code, "test");
    EXPECT_FALSE(result.ok) << "expected an error for: " << code.toStdString();
    return result.error;
}

}  // namespace

TEST(LuaSandbox, RunsPlainScripts)
{
    LightCpp::Lua::LuaSandbox sandbox;
    const auto result = sandbox.runString(
        "local t = {} for i = 1, 100 do t[i] = i * i end",
        "test");
    EXPECT_TRUE(result.ok) << result.error.toStdString();
}

TEST(LuaSandbox, SyntaxErrorsAreReported)
{
    const auto error = errorMessageOf("this is not } lua");
    EXPECT_TRUE(error.contains("near"));
}

TEST(LuaSandbox, EnforcesMemoryCeiling)
{
    LightCpp::Lua::SandboxLimits limits;
    limits.memoryCeilingBytes = 4 * 1024 * 1024;
    LightCpp::Lua::LuaSandbox sandbox(limits);
    const auto result = sandbox.runString(
        "local t = {} for i = 1, 64 do t[i] = string.rep('x', 1024*1024) end",
        "test");
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(sandbox.valid());
    // The allocator ceiling is what stopped it, not the wall clock.
    EXPECT_TRUE(result.memoryLimitHit);
    EXPECT_LE(sandbox.memoryUsed(), limits.memoryCeilingBytes);
}

TEST(LuaSandbox, EnforcesInstructionBudget)
{
    LightCpp::Lua::SandboxLimits limits;
    limits.instructionBudget = 2'000'000;
    limits.hookInterval = 100'000;
    limits.timeBudgetMs = 30'000;  // want the step counter, not the clock
    LightCpp::Lua::LuaSandbox sandbox(limits);
    const auto result = sandbox.runString("while true do end", "test");
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.stepOrTimeLimitHit);
}

TEST(LuaSandbox, EnforcesWallClockBudget)
{
    LightCpp::Lua::SandboxLimits limits;
    limits.instructionBudget = std::numeric_limits<qint64>::max() / 2;
    limits.hookInterval = 1'000'000;
    limits.timeBudgetMs = 300;
    LightCpp::Lua::LuaSandbox sandbox(limits);
    const auto result = sandbox.runString("while true do end", "test");
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.stepOrTimeLimitHit);
}

TEST(LuaSandbox, WhitelistOmitsDangerousLibraries)
{
    // Escape hatches: raising them out proves they are nil.
    EXPECT_EQ(errorMessageOf("error(tostring(load))"), "nil");
    EXPECT_EQ(errorMessageOf("error(tostring(loadfile))"), "nil");
    EXPECT_EQ(errorMessageOf("error(tostring(dofile))"), "nil");
    EXPECT_EQ(errorMessageOf("error(tostring(require))"), "nil");
    EXPECT_EQ(errorMessageOf("error(tostring(io))"), "nil");
    EXPECT_EQ(errorMessageOf("error(tostring(package))"), "nil");
    EXPECT_EQ(errorMessageOf("error(tostring(debug))"), "nil");
    // os is reduced to clocks: no environment, no files, no exit.
    EXPECT_EQ(errorMessageOf("error(tostring(os.getenv))"), "nil");
    EXPECT_EQ(errorMessageOf("error(tostring(os.execute))"), "nil");
    EXPECT_EQ(errorMessageOf("error(tostring(os.remove))"), "nil");
    // The safe libraries are there. tostring() renders a function as
    // "function: 0x<address>" — match the prefix, not the address.
    EXPECT_TRUE(errorMessageOf("error(tostring(string.rep))").startsWith(
        QStringLiteral("function")));
    EXPECT_TRUE(errorMessageOf("error(tostring(table.insert))").startsWith(
        QStringLiteral("function")));
    EXPECT_TRUE(errorMessageOf("error(tostring(math.floor))").startsWith(
        QStringLiteral("function")));
    EXPECT_TRUE(errorMessageOf("error(tostring(utf8.char))").startsWith(
        QStringLiteral("function")));
    EXPECT_TRUE(errorMessageOf("error(tostring(os.time))").startsWith(
        QStringLiteral("function")));
}

TEST(LuaSandbox, PrintGoesToTheSink)
{
    QStringList printed;
    LightCpp::Lua::LuaSandbox sandbox;
    sandbox.setPrintSink([&printed](const QString& line) { printed << line; });
    const auto result = sandbox.runString("print('hello', 42)", "test");
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_EQ(printed.size(), 1);
    EXPECT_EQ(printed.first(), QStringLiteral("hello\t42"));
}

TEST(LuaSandbox, Lua54IntegerSubtypeAndGenerationalGc)
{
    // 7//2 is integer division in 5.4; math.type distinguishes 3 (integer)
    // from 3.0 (float).
    EXPECT_EQ(errorMessageOf("error(math.type(7//2))"), "integer");
    EXPECT_EQ(errorMessageOf("error(math.type(7/2))"), "float");
    // collectgarbage("isrunning") works on the generational collector.
    EXPECT_EQ(errorMessageOf("error(tostring(collectgarbage('isrunning')))"),
              "true");
}

TEST(LuaSandbox, CallGlobalRunsLifecycleFunctions)
{
    LightCpp::Lua::LuaSandbox sandbox;
    ASSERT_TRUE(sandbox.runString("RAN = 0; function onStart() RAN = 7 end",
                                  "test")
                     .ok);
    const auto result = sandbox.callGlobal("onStart");
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    // Probe RAN in the SAME state — a fresh sandbox would not know it.
    const auto probe = sandbox.runString("error(RAN)", "test");
    EXPECT_FALSE(probe.ok);
    EXPECT_EQ(probe.error, QStringLiteral("7"));
    // Calling something that is not a function reports cleanly.
    const auto missing = sandbox.callGlobal("doesNotExist");
    EXPECT_FALSE(missing.ok);
    EXPECT_TRUE(missing.error.contains("not a function"));
}
