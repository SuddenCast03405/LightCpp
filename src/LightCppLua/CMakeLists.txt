# LightCppLua — the sandboxed Lua 5.4 plugin host.
#
# LightCppLuaCore is the upstream Lua VM (vendored under third_party/, built
# from source with warnings off — it is C code we do not own). LightCppLua
# itself adds the sandbox allocator/hooks plus the plugin adapter that turns a
# *.lua file into a Plugin. The `ide` surface is bound with LuaBridge3
# (LightCppLuaBridge below) — one addFunction per member, on namespaces rather
# than classes, because scripts call it with dot syntax.

set(LUA_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/lua-5.4.7/src")
set(LUA_SOURCES
    lapi.c
    lauxlib.c
    lbaselib.c
    lcode.c
    lcorolib.c
    lctype.c
    ldblib.c
    ldebug.c
    ldo.c
    ldump.c
    lfunc.c
    lgc.c
    linit.c
    llex.c
    lmathlib.c
    lmem.c
    loadlib.c
    lobject.c
    lopcodes.c
    loslib.c
    lparser.c
    lstate.c
    lstring.c
    lstrlib.c
    ltablib.c
    ltable.c
    ltm.c
    lundump.c
    lutf8lib.c
    lvm.c
    lzio.c
)
list(TRANSFORM LUA_SOURCES PREPEND "${LUA_SOURCE_DIR}/")

add_library(LightCppLuaCore STATIC ${LUA_SOURCES})
target_include_directories(LightCppLuaCore PUBLIC ${LUA_SOURCE_DIR})
# Upstream C code: warnings are not ours to fix, and -Werror would be.
if(MSVC)
    target_compile_options(LightCppLuaCore PRIVATE /w)
else()
    target_compile_options(LightCppLuaCore PRIVATE -w)
endif()

# LuaBridge is vendored third-party code. Mark its headers as system headers so
# diagnostics originating inside the dependency do not fail our /W4 /WX build.
add_library(LightCppLuaBridge INTERFACE)
target_include_directories(LightCppLuaBridge SYSTEM
    INTERFACE "${CMAKE_SOURCE_DIR}/third_party/LuaBridge3-3.0-rc12/Source")

add_library(LightCppLua STATIC
    Include/LightCpp/Lua/LuaPluginHost.hpp
    Include/LightCpp/Lua/LuaSandbox.hpp
    Source/LuaSandbox.cpp
    Source/LuaPluginHost.cpp
)

add_library(LightCpp::Lua ALIAS LightCppLua)

target_include_directories(LightCppLua PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(LightCppLua PUBLIC cxx_std_20)

target_link_libraries(LightCppLua
    PUBLIC LightCppLuaCore
    PUBLIC LightCppLuaBridge
    PUBLIC LightCppPlugin
    PUBLIC Qt6::Core
    PRIVATE LightCppWarnings
)
