// LuaSandbox — one Lua 5.4 state under hard resource limits.
//
// Every script gets its own state built on a custom lua_Alloc allocator with
// a hard memory ceiling, and instruction-counting hooks (lua_sethook) plus a
// wall-clock deadline for each call. The standard library is a whitelist:
// the state is built by opening only what is safe, and the remaining escape
// hatches (load / dofile / loadfile, and stdout through print) are removed
// or rerouted. Nothing in the sandbox can open files, load binary chunks,
// or loop forever.

#pragma once

#include <QString>

#include <functional>

struct lua_State;
struct lua_Debug;

namespace LightCpp::Lua {

/// Hard resource ceilings for one sandboxed script. These are the defaults;
/// tests use tighter values to keep failures fast.
struct SandboxLimits {
    /// Hard ceiling on total VM memory (custom allocator rejects beyond it).
    qint64 memoryCeilingBytes = 32 * 1024 * 1024;
    /// VM instructions budget per protected call (hook-enforced).
    qint64 instructionBudget = 200'000'000;
    /// Wall-clock budget per protected call. A backstop for expensive C
    /// internals the instruction counter ticks through slowly.
    qint64 timeBudgetMs = 5000;
    /// Instructions between hook firings (LUA_MASKCOUNT count).
    qint64 hookInterval = 100'000;
};

/// Outcome of one sandboxed call. `error` is a human-readable reason when
/// ok is false; the two limit flags say which ceiling stopped the script.
struct ExecutionResult {
    bool ok = false;
    QString error;
    bool memoryLimitHit = false;
    bool stepOrTimeLimitHit = false;
};

class LuaSandbox {
public:
    explicit LuaSandbox(SandboxLimits limits = {});
    ~LuaSandbox();
    LuaSandbox(const LuaSandbox&) = delete;
    LuaSandbox& operator=(const LuaSandbox&) = delete;

    [[nodiscard]] bool valid() const { return L_ != nullptr; }
    [[nodiscard]] lua_State* state() const { return L_; }
    [[nodiscard]] const SandboxLimits& limits() const { return limits_; }
    /// Bytes the sandboxed VM currently holds (for tests and diagnostics).
    [[nodiscard]] qint64 memoryUsed() const { return budget_.used; }

    /// Where print() output goes instead of stdout. Set before running.
    void setPrintSink(std::function<void(const QString&)> sink);

    /// Compile and execute a UTF-8 source string, under the call budgets.
    /// Text mode only — the sandbox cannot load precompiled bytecode.
    ExecutionResult runString(const QString& code, const QString& chunkName);

    /// Call a global function by name (no arguments, return values
    /// discarded) under the call budgets.
    ExecutionResult callGlobal(const QString& name);

    /// Call the function at absolute stack index `funcIndex` with `nargs`
    /// values already pushed above it. The function stays on the stack
    /// (pop it yourself); the copy and the arguments are consumed. Budgets
    /// apply — this is how registered Lua callbacks are executed.
    ExecutionResult callFunction(int funcIndex, int nargs);

private:
    // Registry key holding the owning LuaSandbox (for static callbacks).
    static constexpr char kSelfKey[] = "lightcpp.sandbox.self";

    static void* luaAlloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static void hook(lua_State* L, lua_Debug* ar);
    static int luaPrint(lua_State* L);
    /// The owning sandbox recorded in the state's registry.
    static LuaSandbox* self(lua_State* L);
    // The reduced os.time/clock/date callbacks live in the cpp's anonymous
    // namespace — they are not members.

    void openWhitelistedLibraries();
    void beginCall();
    ExecutionResult endCall(int status);
    void raiseLimit(lua_State* L);

    SandboxLimits limits_;
    struct Budget {
        qint64 used = 0;
        qint64 ceiling = 0;
    } budget_;
    lua_State* L_ = nullptr;
    std::function<void(const QString&)> printSink_;
    /// Hook bookkeeping, valid between beginCall/endCall.
    qint64 hookTicksLeft_ = 0;
    qint64 callStartMs_ = 0;
    bool limitHit_ = false;
};

}  // namespace LightCpp::Lua
