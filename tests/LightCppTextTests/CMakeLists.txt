# LightCppTextTests: GTest suite for the encoding module.

add_executable(LightCppTextTests
    EncodingTests.cpp
    TextFileTests.cpp
)

target_link_libraries(LightCppTextTests PRIVATE
    LightCppText
    LightCppCore
    GTest::gtest_main
    LightCppWarnings
)

# Statically link the GCC runtime on MinGW so the binary runs without the
# toolchain DLLs on PATH (see the core tests; required for reliable ctest).
if(MINGW)
    target_link_options(LightCppTextTests PRIVATE -static-libgcc -static-libstdc++)
endif()

# Qt is a shared library: copy its runtime DLLs next to the test executable
# so ctest-spawned processes can load them without PATH setup.
if(WIN32)
    add_custom_command(TARGET LightCppTextTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:LightCppTextTests>
                $<TARGET_FILE_DIR:LightCppTextTests>
        COMMAND_EXPAND_LISTS)
endif()

include(GoogleTest)

# Pass the toolchain directory explicitly as well (belt and braces).
get_filename_component(LIGHTCPP_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)

gtest_discover_tests(LightCppTextTests
    PROPERTIES
        ENVIRONMENT "PATH=${LIGHTCPP_TOOLCHAIN_BIN}\;$ENV{PATH}"
)
