# LightCppTestingTests — the test-domain parsers and runner arguments.

add_executable(LightCppTestingTests
    TestingTests.cpp
)

target_include_directories(LightCppTestingTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/LightCppTesting/Include
)

target_compile_features(LightCppTestingTests PRIVATE cxx_std_20)

target_link_libraries(LightCppTestingTests PRIVATE
    LightCppTesting
    GTest::gtest_main
    LightCppWarnings
)

include(GoogleTest)
gtest_discover_tests(LightCppTestingTests)
