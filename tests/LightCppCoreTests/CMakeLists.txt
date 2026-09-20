# LightCppCoreTests: GTest suite for the core library.

add_executable(LightCppCoreTests
    Utf8Tests.cpp
    Utf8FileTests.cpp
)

target_link_libraries(LightCppCoreTests PRIVATE
    LightCppCore
    GTest::gtest_main
    LightCppWarnings
)

# Statically link the GCC/Winpthread runtime on MinGW so the test binary runs
# without the toolchain DLLs on PATH (e.g. under CI runners and ctest).
if(MINGW)
    target_link_options(LightCppCoreTests PRIVATE -static-libgcc -static-libstdc++)
endif()

include(GoogleTest)

# MinGW-built test binaries need libstdc++/libgcc from the toolchain directory.
# Pass it explicitly so `ctest` works even when the toolchain is not on PATH.
get_filename_component(LIGHTCPP_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)

gtest_discover_tests(LightCppCoreTests
    PROPERTIES
        ENVIRONMENT "PATH=${LIGHTCPP_TOOLCHAIN_BIN}\;$ENV{PATH}"
)
