# LightCppTesting — the test-domain engine of lightcpp.1.md §十.
#
# Qt Core only, no widgets: every function here takes text a test runner
# printed and returns data. That is deliberate — the parsers are where a
# "test explorer" is usually wrong (a skipped case counted as a pass, a
# disabled case counted as a failure, a suite that produced no summary read
# as success), and they are the only part that can be pinned without a
# display or a live run.

add_library(LightCppTesting STATIC
    Source/TestDiscovery.cpp
    Source/TestOutput.cpp
    Source/TestRunner.cpp
    Source/CoverageReport.cpp
)

add_library(LightCpp::Testing ALIAS LightCppTesting)

target_include_directories(LightCppTesting PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(LightCppTesting PUBLIC cxx_std_20)

target_link_libraries(LightCppTesting
    PUBLIC Qt6::Core
    PRIVATE LightCppWarnings
)
