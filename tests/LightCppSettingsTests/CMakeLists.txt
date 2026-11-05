# LightCppSettingsTests: GTest suite for the configuration model.

add_executable(LightCppSettingsTests
    SettingsTests.cpp
    ToolchainScannerTests.cpp
)

target_link_libraries(LightCppSettingsTests PRIVATE
    LightCppSettings
    GTest::gtest_main
    LightCppWarnings
)

# Statically link the GCC runtime on MinGW so the binary runs without the
# toolchain DLLs on PATH (required for reliable ctest).
if(MINGW)
    target_link_options(LightCppSettingsTests PRIVATE -static-libgcc -static-libstdc++)
endif()

# Qt is a shared library: copy its runtime DLLs next to the test executable
# so ctest-spawned processes can load them without PATH setup.
if(WIN32)
    add_custom_command(TARGET LightCppSettingsTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:LightCppSettingsTests>
                $<TARGET_FILE_DIR:LightCppSettingsTests>
        COMMAND_EXPAND_LISTS)
endif()

include(GoogleTest)

get_filename_component(LIGHTCPP_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)

gtest_discover_tests(LightCppSettingsTests
    PROPERTIES
        ENVIRONMENT "PATH=${LIGHTCPP_TOOLCHAIN_BIN}\;$ENV{PATH}"
)
