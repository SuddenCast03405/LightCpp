# LightCppLuaTests — sandbox contract and the Lua plugin host.

add_executable(LightCppLuaTests
    LuaSandboxTests.cpp
    LuaPluginHostTests.cpp
)

# The language pack ships from the source tree (the build stages it into
# dist/plugins/lua). One test loads that very file rather than a toy script,
# so it needs the path — a copy would stop verifying what users get.
target_compile_definitions(LightCppLuaTests PRIVATE
    LIGHTCPP_LUA_PLUGIN_DIR="${CMAKE_SOURCE_DIR}/plugins/lua")

target_link_libraries(LightCppLuaTests PRIVATE
    LightCppLua
    GTest::gtest_main
    LightCppWarnings
)

if(MINGW)
    target_link_options(LightCppLuaTests PRIVATE -static-libgcc -static-libstdc++)
endif()

if(WIN32)
    add_custom_command(TARGET LightCppLuaTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:LightCppLuaTests>
                $<TARGET_FILE_DIR:LightCppLuaTests>
        COMMAND_EXPAND_LISTS)
endif()

include(GoogleTest)

get_filename_component(LIGHTCPP_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)

gtest_discover_tests(LightCppLuaTests
    PROPERTIES
        ENVIRONMENT "PATH=${LIGHTCPP_TOOLCHAIN_BIN}\;$ENV{PATH}"
)
