# LightCppProjectTests: GTest suite for the project/build/git models.

add_executable(LightCppProjectTests
    BuildConfigurationTests.cpp
    CMakeProjectTests.cpp
    CompilerDiagnosticsTests.cpp
    DevCppProjectTests.cpp
    DspProjectTests.cpp
    FolderContentsTests.cpp
    GitStatusTests.cpp
    MsvcProjectTests.cpp
)

target_link_libraries(LightCppProjectTests PRIVATE
    LightCppProject
    # DevCppProjectTests reads LightCpp/Text/Encoding.hpp directly (the GBK
    # fixture only makes sense where the local ANSI page is GBK).
    LightCppText
    GTest::gtest_main
    LightCppWarnings
)

# Statically link the GCC runtime on MinGW so the binary runs without the
# toolchain DLLs on PATH (see the core tests; required for reliable ctest).
if(MINGW)
    target_link_options(LightCppProjectTests PRIVATE -static-libgcc -static-libstdc++)
endif()

# Qt is a shared library: copy its runtime DLLs next to the test executable
# so ctest-spawned processes can load them without PATH setup.
if(WIN32)
    add_custom_command(TARGET LightCppProjectTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:LightCppProjectTests>
                $<TARGET_FILE_DIR:LightCppProjectTests>
        COMMAND_EXPAND_LISTS)
endif()

include(GoogleTest)

get_filename_component(LIGHTCPP_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)

gtest_discover_tests(LightCppProjectTests
    PROPERTIES
        ENVIRONMENT "PATH=${LIGHTCPP_TOOLCHAIN_BIN}\;$ENV{PATH}"
)
