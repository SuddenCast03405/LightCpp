# LightCppPluginTests — PluginManager behaviour, the plugins-directory
# resolution, and a real QPluginLoader round-trip against the built
# DevCppPlugin DLL.

add_executable(LightCppPluginTests
    PluginManagerTests.cpp
    PluginPathsTests.cpp
    ProjectPluginTests.cpp
)

target_compile_definitions(LightCppPluginTests PRIVATE
    LIGHTCPP_BUILT_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins"
)

target_link_libraries(LightCppPluginTests PRIVATE
    LightCppPlugin
    GTest::gtest_main
    LightCppWarnings
)

# ProjectPluginTests loads the real plugin DLLs out of LIGHTCPP_BUILT_PLUGIN_DIR.
# Depend on them so `cmake --build . --target LightCppPluginTests` stages them
# too; a test run without the DLLs would skip instead of exercising them.
foreach(plugin_target IN ITEMS
        DevCppPlugin
        MsvcPlugin
        GoPlugin
        RustPlugin
        PythonPlugin
        ZigPlugin
        QtCppPlugin
        WxWidgetsPlugin)
    if(TARGET ${plugin_target})
        add_dependencies(LightCppPluginTests ${plugin_target})
    endif()
endforeach()

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
