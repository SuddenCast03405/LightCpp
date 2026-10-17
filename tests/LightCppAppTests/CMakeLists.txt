# LightCppAppTests — the App layer's first suite.
#
# The App layer is Qt widgets and has no suite for that reason. CodeFormatter
# is not a widget: it is a state machine over QProcess, which is exactly the
# kind of thing that breaks silently (R2-07). Its source is compiled straight
# from the application instead of pulling in the whole app target.
#
# ProcessShutdown is not a widget either: it is the policy behind stopping the
# children a window owns (R2-26). It is compiled from the application for the
# same reason, and it deliberately knows nothing about QProcess — the machine
# this was written on cannot create a process' pipes, so a policy that needed
# a real child could not be tested here at all.
#
# FindScan (R2-30) is the resume position the three find/replace scans share,
# and DebuggerController (R2-45) is the state machine around one QProcess:
# both are Qt6::Core only, which is why they can be compiled here and the
# widgets that use them cannot.
#
# TerminalLaunch (R2-20) is the line typed into an embedded terminal to start
# a run, and GitClient (R2-18) is the queue that must refuse to run git with
# no working directory. GitClient pulls in LightCppProject for its status
# parser; both stay Qt6::Core only.

# The header is listed next to the source on purpose: AUTOMOC only generates
# moc_CodeFormatter.cpp for headers that belong to the target, and without it
# the link fails on CodeFormatter::staticMetaObject and its two signals.
add_executable(LightCppAppTests
    CodeFormatterTests.cpp
    CompileDatabaseTests.cpp
    ConsoleDecoderTests.cpp
    DebuggerControllerTests.cpp
    FindScanTests.cpp
    GitClientTests.cpp
    ProcessShutdownTests.cpp
    TerminalLaunchTests.cpp
    TextColumnsTests.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/CodeFormatter.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/CompileDatabase.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/ConsoleDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/DebuggerController.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/FindScan.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/GitClient.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/ProcessShutdown.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/TerminalLaunch.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Source/TextColumns.cpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/CodeFormatter.hpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/CompileDatabase.hpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/ConsoleDecoder.hpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/DebuggerController.hpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/FindScan.hpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/GitClient.hpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/ProcessShutdown.hpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/TerminalLaunch.hpp
    ${CMAKE_SOURCE_DIR}/src/LightCppApp/Include/LightCpp/App/TextColumns.hpp
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
    # GitClient's own header includes the status type it reports.
    LightCppProject
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
