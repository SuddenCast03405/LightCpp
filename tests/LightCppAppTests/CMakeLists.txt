# LightCppAppTests — the App layer's first suite.
#
# The App layer is Qt widgets and has no suite for that reason. CodeFormatter
# is not a widget: it is a state machine over QProcess, which is exactly the
# kind of thing that breaks silently (R2-07). Its source is compiled straight
# from the application instead of pulling in the whole app target.

# The header is listed next to the source on purpose: AUTOMOC only generates
# moc_CodeFormatter.cpp for headers that belong to the target, and without it
# the link fails on CodeFormatter::staticMetaObject and its two signals.
add_executable(LightCppAppTests
    CodeFormatterTests.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/CodeFormatter.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/CodeFormatter.hpp
)

# The app target sets this on itself; CodeFormatter declares Q_OBJECT, so the
# suite needs its own moc pass for the header it includes.
set_target_properties(LightCppAppTests PROPERTIES AUTOMOC ON)

target_include_directories(LightCppAppTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include
)

target_compile_features(LightCppAppTests PRIVATE cxx_std_20)

# GTest::gtest rather than gtest_main: the suite provides main() so it can
# create the QCoreApplication the QProcess/QTimer machinery needs.
target_link_libraries(LightCppAppTests PRIVATE
    Qt6::Core
    GTest::gtest
    LightCppWarnings
)

if(MINGW)
    target_link_options(LightCppAppTests PRIVATE -static-libgcc -static-libstdc++)
endif()

if(WIN32)
    add_custom_command(TARGET LightCppAppTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:LightCppAppTests>
                $<TARGET_FILE_DIR:LightCppAppTests>
        COMMAND_EXPAND_LISTS)
endif()

include(GoogleTest)

# The toolchain directory on PATH as well: the formatter looks for
# clang-format there, and the tests that need it skip rather than fail when it
# is missing.
get_filename_component(LIGHTCPP_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)

gtest_discover_tests(LightCppAppTests
    PROPERTIES
        ENVIRONMENT "PATH=${LIGHTCPP_TOOLCHAIN_BIN}\;$ENV{PATH}"
)
