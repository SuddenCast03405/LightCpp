# LightCppPluginTests — PluginManager behaviour, the plugins-directory
# resolution, and a real QPluginLoader round-trip against the built
# DevCppPlugin DLL.

add_executable(LightCppPluginTests
    PluginManagerTests.cpp
    PluginPathsTests.cpp
)

target_compile_definitions(LightCppPluginTests PRIVATE
    LIGHTCPP_BUILT_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins"
)

target_link_libraries(LightCppPluginTests PRIVATE
    LightCppPlugin
    GTest::gtest_main
    LightCppWarnings
)

if(MINGW)
    target_link_options(LightCppPluginTests PRIVATE -static-libgcc -static-libstdc++)
endif()

if(WIN32)
    add_custom_command(TARGET LightCppPluginTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:LightCppPluginTests>
                $<TARGET_FILE_DIR:LightCppPluginTests>
        COMMAND_EXPAND_LISTS)
endif()

include(GoogleTest)

get_filename_component(LIGHTCPP_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)

gtest_discover_tests(LightCppPluginTests
    PROPERTIES
        ENVIRONMENT "PATH=${LIGHTCPP_TOOLCHAIN_BIN}\;$ENV{PATH}"
)
