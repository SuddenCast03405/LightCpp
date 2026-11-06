# LightCppWorkbenchTests — analysis, git, markup and Qt file parsers.

add_executable(LightCppWorkbenchTests
    WorkbenchTests.cpp
    WorkbenchExtraTests.cpp
    PtyProcessTests.cpp
    PtySessionTests.cpp
)

target_include_directories(LightCppWorkbenchTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/LightCppWorkbench/Include
)

target_compile_features(LightCppWorkbenchTests PRIVATE cxx_std_20)

target_link_libraries(LightCppWorkbenchTests PRIVATE
    LightCppWorkbench
    GTest::gtest_main
    LightCppWarnings
)

include(GoogleTest)
gtest_discover_tests(LightCppWorkbenchTests)
