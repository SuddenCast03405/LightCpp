cmake_minimum_required(VERSION 3.24)

project(LightCpp
    VERSION 0.1.0
    DESCRIPTION "LightC++ - Lightweight C/C++ IDE"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global compile settings (C++20 baseline, strict warnings)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings are exposed as a linkable INTERFACE target instead of global
# compile options, so third-party sources fetched by CPM are not affected.
add_library(LightCppWarnings INTERFACE)
add_library(LightCpp::Warnings ALIAS LightCppWarnings)

if(MSVC)
    target_compile_options(LightCppWarnings INTERFACE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(LightCppWarnings INTERFACE -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# Dependencies (spec 2: Qt 6.5 Widgets is the UI shell; this module only
# needs QtCore so far, which the MSYS2 toolchain provides)
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 COMPONENTS Core Widgets Xml REQUIRED)

# ---------------------------------------------------------------------------
# CPM.cmake dependency management (spec 4.2: all third-party deps via CPM)
# ---------------------------------------------------------------------------
include(cmake/CPM.cmake)

# ---------------------------------------------------------------------------
# Coverage (spec 4.4: gcov/lcov, optional target LightCppCoverage)
# ---------------------------------------------------------------------------
option(LIGHTCPP_ENABLE_COVERAGE "Enable gcov instrumentation for coverage reports" OFF)

# windeployqt after building `lightcpp`. On by default so a local Windows build
# still produces a runnable dist/; CI turns it off (no test needs it, and the
# step is the one part of the build that depends on a writable Qt install).
option(LIGHTCPP_DEPLOY_QT "Deploy the Qt runtime next to lightcpp (Windows only)" ON)

if(LIGHTCPP_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Coverage requires GCC or Clang (gcov-based).")
    endif()
    target_compile_options(LightCppWarnings INTERFACE --coverage -O0 -g)
    target_link_options(LightCppWarnings INTERFACE --coverage)
    find_program(LCOV_EXECUTABLE lcov)
    find_program(GENHTML_EXECUTABLE genhtml)
    if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
        add_custom_target(LightCppCoverage
            COMMAND ${LCOV_EXECUTABLE} --directory . --capture --output-file coverage.info
            COMMAND ${LCOV_EXECUTABLE} --remove coverage.info '/usr/*' '*/_deps/*' --output-file coverage.info
            COMMAND ${GENHTML_EXECUTABLE} coverage.info --output-directory coverage_html
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating lcov coverage report in coverage_html/"
            VERBATIM)
    else()
        message(STATUS "lcov/genhtml not found: LightCppCoverage target not registered.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Testing (GTest via CPM, integrated with CTest)
# ---------------------------------------------------------------------------
enable_testing()
include(CTest)

if(BUILD_TESTING)
    # googletest is managed by CPM. The source tree is vendored under
    # third_party/ so the build is reproducible offline; set
    # LIGHTCPP_GTEST_SOURCE to override it, or replace SOURCE_DIR with
    # GITHUB_REPOSITORY/GIT_TAG to let CPM fetch it at configure time.
    set(LIGHTCPP_GTEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest-1.14.0"
        CACHE PATH "GoogleTest source tree used by CPM")

    CPMAddPackage(
        NAME googletest
        SOURCE_DIR ${LIGHTCPP_GTEST_SOURCE}
        VERSION 1.14.0
        FORCE TRUE
        OPTIONS
            "INSTALL_GTEST OFF"
            "BUILD_GMOCK ON"
            "gtest_force_shared_crt ON"
    )

    if(NOT TARGET GTest::gtest_main)
        message(FATAL_ERROR "GTest not available from ${LIGHTCPP_GTEST_SOURCE}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Modules
# ---------------------------------------------------------------------------
add_subdirectory(src/LightCppCore)
add_subdirectory(src/LightCppText)
add_subdirectory(src/LightCppProject)
add_subdirectory(src/LightCppSettings)
add_subdirectory(src/LightCppPlugin)
add_subdirectory(src/LightCppLua)
add_subdirectory(src/LightCppApp)
add_subdirectory(apps/LightCppEncodingCli)
add_subdirectory(plugins)

if(BUILD_TESTING)
    add_subdirectory(tests/LightCppCoreTests)
    add_subdirectory(tests/LightCppTextTests)
    add_subdirectory(tests/LightCppProjectTests)
    add_subdirectory(tests/LightCppSettingsTests)
    add_subdirectory(tests/LightCppPluginTests)
    add_subdirectory(tests/LightCppLuaTests)
    add_subdirectory(tests/LightCppAppTests)
endif()
