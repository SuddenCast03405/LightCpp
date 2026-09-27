#include "LightCpp/Lua/LuaSandbox.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <chrono>
#include <cstdlib>
#include <ctime>

#include <QRegularExpression>

namespace LightCpp::Lua {

namespace {

qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

/// The reduced `os` table: time sources only — nothing that touches the
/// filesystem or the process environment.
int luaOsTime(lua_State* L)
{
    const time_t t = ::time(nullptr);
    lua_pushinteger(L, static_cast<lua_Integer>(t));
    return 1;
}

int luaOsClock(lua_State* L)
{
    lua_pushnumber(L, static_cast<lua_Number>(::clock())
                          / static_cast<lua_Number>(CLOCKS_PER_SEC));
    return 1;
}

int luaOsDate(lua_State* L)
{
    const char* fmt = luaL_optstring(L, 1, "%c");
    if (*fmt == '!') {  // no UTC handling beyond the standard marker
        ++fmt;
    }
    if (*fmt == '%' && *(fmt + 1) == 'c') {
        // %c is locale-dependent; force a stable ASCII-ish rendering.
        fmt = "%Y-%m-%d %H:%M:%S";
    }
    char buffer[256];
    const time_t t = ::time(nullptr);
    struct tm local;
    ::localtime_s(&local, &t);
    const size_t written = ::strftime(buffer, sizeof(buffer), fmt, &local);
    lua_pushlstring(L, buffer, written);
    return 1;
}

}  // namespace

LuaSandbox::LuaSandbox(SandboxLimits limits) : limits_(limits)
{
    budget_.ceiling = limits.memoryCeilingBytes;
    L_ = lua_newstate(&LuaSandbox::luaAlloc, &budget_);
    if (L_ == nullptr) {
        return;  // valid() reports the failure
    }
    // The owning sandbox, reachable from the static callbacks.
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, kSelfKey);

    openWhitelistedLibraries();
}

LuaSandbox::~LuaSandbox()
{
    if (L_ != nullptr) {
        lua_close(L_);
        L_ = nullptr;
    }
}

void* LuaSandbox::luaAlloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
    auto* budget = static_cast<Budget*>(ud);
    if (nsize == 0) {
        std::free(ptr);
        budget->used -= static_cast<qint64>(osize);
        return nullptr;
    }
    // Realloc accounting: the old block's size is released before the new
    // one is charged, so a growth within the ceiling is allowed.
    const qint64 current = budget->used
                           - (ptr != nullptr ? static_cast<qint64>(osize) : 0);
    if (current + static_cast<qint64>(nsize) > budget->ceiling) {
        return nullptr;  // lua reports LUA_ERRMEM("not enough memory")
    }
    void* grown = std::realloc(ptr, nsize);
    if (grown == nullptr) {
        return nullptr;
    }
    budget->used = current + static_cast<qint64>(nsize);
    return grown;
}

void LuaSandbox::openWhitelistedLibraries()
{
    lua_State* L = L_;

    // Base minus its escape hatches: load* (binary chunks, arbitrary
    // bytecode), dofile (filesystem), and print (stdout — rerouted below).
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    static const char* const removed[] = {
        "load", "loadfile", "dofile", "require", "package",
    };
    for (const char* name : removed) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_pushcfunction(L, &LuaSandbox::luaPrint);
    lua_setglobal(L, "print");

    // Pure computation libraries — none of them touch the outside world.
    struct NamedOpener {
        const char* name;
        lua_CFunction opener;
    };
    static const NamedOpener openers[] = {
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_COLIBNAME, luaopen_coroutine},
    };
    for (const NamedOpener& opener : openers) {
        luaL_requiref(L, opener.name, opener.opener, 1);
        lua_pop(L, 1);
    }

    // os, reduced to clocks and dates. io, debug and package are simply
    // never opened — no file handles, no VM introspection, no require.
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, &luaOsTime);
    lua_setfield(L, -2, "time");
    lua_pushcfunction(L, &luaOsClock);
    lua_setfield(L, -2, "clock");
    lua_pushcfunction(L, &luaOsDate);
    lua_setfield(L, -2, "date");
    lua_setglobal(L, "os");
}

void LuaSandbox::setPrintSink(std::function<void(const QString&)> sink)
{
    printSink_ = std::move(sink);
}

int LuaSandbox::luaPrint(lua_State* L)
{
    auto* owner = static_cast<LuaSandbox*>(self(L));
    if (owner == nullptr || !owner->printSink_) {
        return 0;
    }
    const int n = lua_gettop(L);
    QString line;
    for (int i = 1; i <= n; ++i) {
        size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);
        if (i > 1) {
            line += QLatin1Char('\t');
        }
        line += QString::fromUtf8(text, static_cast<qsizetype>(length));
        lua_pop(L, 1);  // luaL_tolstring pushed the string
    }
    owner->printSink_(line);
    return 0;
}

LuaSandbox* LuaSandbox::self(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kSelfKey);
    auto* owner = static_cast<LuaSandbox*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return owner;
}

void LuaSandbox::raiseLimit(lua_State* L)
{
    limitHit_ = true;
    luaL_error(L, "script aborted: instruction or time budget exceeded");
}

void LuaSandbox::hook(lua_State* L, lua_Debug* /*ar*/)
{
    auto* owner = static_cast<LuaSandbox*>(self(L));
    if (owner == nullptr) {
        return;
    }
    if (owner->hookTicksLeft_ <= 0
        || nowMs() - owner->callStartMs_ > owner->limits_.timeBudgetMs) {
        owner->raiseLimit(L);
    }
    --owner->hookTicksLeft_;
}

void LuaSandbox::beginCall()
{
    hookTicksLeft_ = limits_.instructionBudget
                     / (limits_.hookInterval > 0 ? limits_.hookInterval : 1);
    callStartMs_ = nowMs();
    limitHit_ = false;
    if (L_ != nullptr) {
        lua_sethook(L_, &LuaSandbox::hook, LUA_MASKCOUNT,
                    static_cast<int>(limits_.hookInterval));
    }
}

ExecutionResult LuaSandbox::endCall(int status)
{
    if (L_ != nullptr) {
        lua_sethook(L_, nullptr, 0, 0);
    }
    ExecutionResult result;
    result.stepOrTimeLimitHit = limitHit_;
    switch (status) {
    case LUA_OK:
        result.ok = true;
        break;
    case LUA_ERRMEM:
        result.memoryLimitHit = true;
        result.error = QStringLiteral("out of memory (sandbox ceiling is %1 "
                                       "bytes)")
                           .arg(limits_.memoryCeilingBytes);
        break;
    case LUA_ERRRUN:
    case LUA_ERRSYNTAX:
    case LUA_ERRERR: {
        // Stringify whatever was raised — numbers and booleans included —
        // and drop the "[chunk]:line:" position prefix lua prepends to
        // string messages: the caller wants the raised value, not its
        // source location.
        size_t length = 0;
        const char* text = luaL_tolstring(L_, -1, &length);
        QString message =
            QString::fromUtf8(text, static_cast<qsizetype>(length));
        lua_pop(L_, 2);  // tolstring's pushed copy + the raised value
        static const QRegularExpression positionPrefix(
            QStringLiteral("^\\[string \"[^\"]*\"\\]:\\d+:\\s*"));
        message.remove(positionPrefix);
        result.error = message;
        break;
    }
    default:
        result.error = QStringLiteral("Lua error status %1").arg(status);
        lua_pop(L_, 1);
        break;
    }
    return result;
}

ExecutionResult LuaSandbox::runString(const QString& code,
                                      const QString& chunkName)
{
    if (L_ == nullptr) {
        return {false, QStringLiteral("out of memory creating the Lua state"),
                true, false};
    }
    const QByteArray utf8 = code.toUtf8();
    const QByteArray chunk = chunkName.toUtf8();

    beginCall();
    int status = luaL_loadbuffer(L_, utf8.constData(),
                                 static_cast<size_t>(utf8.size()), chunk.constData());
    if (status == LUA_OK) {
        status = lua_pcall(L_, 0, 0, 0);
    }
    return endCall(status);
}

ExecutionResult LuaSandbox::callGlobal(const QString& name)
{
    if (L_ == nullptr) {
        return {false, QStringLiteral("out of memory creating the Lua state"),
                true, false};
    }
    const QByteArray utf8 = name.toUtf8();
    lua_getglobal(L_, utf8.constData());
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        return {false, QStringLiteral("%1 is not a function").arg(name),
                false, false};
    }
    beginCall();
    const int status = lua_pcall(L_, 0, 0, 0);
    return endCall(status);
}

ExecutionResult LuaSandbox::callFunction(int funcIndex, int nargs)
{
    if (L_ == nullptr) {
        return {false, QStringLiteral("out of memory creating the Lua state"),
                true, false};
    }
    beginCall();
    lua_pushvalue(L_, funcIndex);          // copy the function to the top
    lua_insert(L_, funcIndex + 1);         // and put it below the arguments
    const int status = lua_pcall(L_, nargs, 0, 0);
    return endCall(status);
}

}  // namespace LightCpp::Lua
