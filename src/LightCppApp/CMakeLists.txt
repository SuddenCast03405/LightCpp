# LightCppApp — IDE GUI application (Qt6 Widgets)

set(LIGHTCPP_APP_SOURCES
    main.cpp
    Include/LightCpp/App/AnalysisPanel.hpp
    Include/LightCpp/App/BatchFormatter.hpp
    Include/LightCpp/App/CodeEditor.hpp
    Include/LightCpp/App/CodeFormatter.hpp
    Include/LightCpp/App/CommandPalette.hpp
    Include/LightCpp/App/CompletionPopup.hpp
    Include/LightCpp/App/ConsoleDecoder.hpp
    Include/LightCpp/App/CppSyntaxHighlighter.hpp
    Include/LightCpp/App/CppTokenScan.hpp
    Include/LightCpp/App/DefaultSettings.hpp
    Include/LightCpp/App/Debug/DebugBreakpoints.hpp
    Include/LightCpp/App/Debug/DebugViews.hpp
    Include/LightCpp/App/Debug/DebugViewsModel.hpp
    Include/LightCpp/App/Debug/GdbMi.hpp
    Include/LightCpp/App/DebugViewsPanel.hpp
    Include/LightCpp/App/DebuggerController.hpp
    Include/LightCpp/App/FileExplorer.hpp
    Include/LightCpp/App/FindBar.hpp
    Include/LightCpp/App/FindScan.hpp
    Include/LightCpp/App/GitBranchDialog.hpp
    Include/LightCpp/App/GitClient.hpp
    Include/LightCpp/App/GitCommandPolicy.hpp
    Include/LightCpp/App/KitToolchainOptions.hpp
    Include/LightCpp/App/LspResponse.hpp
    Include/LightCpp/App/MsvcNames.hpp
    Include/LightCpp/App/LanguageServerClient.hpp
    Include/LightCpp/App/LibClangCompleter.hpp
    Include/LightCpp/App/LspLocationsDialog.hpp
    Include/LightCpp/App/MarkdownPreviewDialog.hpp
    Include/LightCpp/App/MsvcToolchain.hpp
    Include/LightCpp/App/OutputPanel.hpp
    Include/LightCpp/App/MainWindow.hpp
    Include/LightCpp/App/ProcessShutdown.hpp
    Include/LightCpp/App/ProcessTree.hpp
    Include/LightCpp/App/ProjectSearchDialog.hpp
    Include/LightCpp/App/SettingsDialog.hpp
    Include/LightCpp/App/StartupPage.hpp
    Include/LightCpp/App/TestExplorerPanel.hpp
    Include/LightCpp/App/TerminalWidget.hpp
    Include/LightCpp/App/ToolchainHeaders.hpp
    Include/LightCpp/App/ToolchainPaths.hpp
    Source/AnalysisPanel.cpp
    Source/BatchFormatter.cpp
    Source/CodeEditor.cpp
    Source/CodeFormatter.cpp
    Source/CommandPalette.cpp
    Source/CompileDatabase.cpp
    Source/CompletionPopup.cpp
    Source/ConsoleDecoder.cpp
    Source/CppSyntaxHighlighter.cpp
    Source/CppTokenScan.cpp
    Source/DefaultSettings.cpp
    Source/Debug/DebugBreakpoints.cpp
    Source/Debug/DebugViews.cpp
    Source/Debug/DebugViewsModel.cpp
    Source/Debug/GdbMi.cpp
    Source/DebugViewsPanel.cpp
    Source/DebuggerController.cpp
    Source/FileExplorer.cpp
    Source/FindBar.cpp
    Source/FindScan.cpp
    Source/GitBranchDialog.cpp
    Source/GitClient.cpp
    Source/GitCommandPolicy.cpp
    Source/KitToolchainOptions.cpp
    Source/LspResponse.cpp
    Source/MsvcNames.cpp
    Source/LanguageServerClient.cpp
    Source/LibClangCompleter.cpp
    Source/LspLocationsDialog.cpp
    Source/MarkdownPreviewDialog.cpp
    Source/MsvcToolchain.cpp
    Source/OutputPanel.cpp
    Source/MainWindow.cpp
    Source/ProcessShutdown.cpp
    Source/ProcessTree.cpp
    ${LIGHTCPP_PROCESS_TREE_PLATFORM_SOURCE}
    Source/ProjectSearchDialog.cpp
    Source/SettingsDialog.cpp
    Source/StartupPage.cpp
    Source/TestExplorerPanel.cpp
    Source/TerminalLaunch.cpp
    Source/TerminalWidget.cpp
    Source/TextColumns.cpp
    Source/ToolchainPaths.cpp
)

# LibClangCompleter uses the stable C API declarations but resolves the
# shared library at runtime. Headers may live under a versioned LLVM prefix
# (e.g. /usr/lib/llvm-XX/include), so discover them instead of assuming the
# compiler's default include path contains clang-c/Index.h.
find_path(LIGHTCPP_CLANG_C_INCLUDE_DIR clang-c/Index.h
    HINTS
        "${LLVM_ROOT}/include"
        "${CLANG_ROOT}/include"
        "$ENV{LLVM_ROOT}/include"
        "$ENV{CLANG_ROOT}/include"
)
if(NOT LIGHTCPP_CLANG_C_INCLUDE_DIR)
    find_program(LIGHTCPP_LLVM_CONFIG
        NAMES llvm-config llvm-config-22 llvm-config-21 llvm-config-20
              llvm-config-19 llvm-config-18 llvm-config-17 llvm-config-16
              llvm-config-15 llvm-config-14)
    if(LIGHTCPP_LLVM_CONFIG)
        execute_process(
            COMMAND "${LIGHTCPP_LLVM_CONFIG}" --includedir
            OUTPUT_VARIABLE LIGHTCPP_LLVM_INCLUDE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE LIGHTCPP_LLVM_CONFIG_RESULT
        )
        if(LIGHTCPP_LLVM_CONFIG_RESULT EQUAL 0)
            find_path(LIGHTCPP_CLANG_C_INCLUDE_DIR clang-c/Index.h
                HINTS "${LIGHTCPP_LLVM_INCLUDE_DIR}")
        endif()
    endif()
endif()
if(NOT LIGHTCPP_CLANG_C_INCLUDE_DIR)
    message(FATAL_ERROR
        "clang-c/Index.h was not found. Install the Clang development headers "
        "or set LLVM_ROOT to the LLVM installation prefix.")
endif()

add_executable(lightcpp ${LIGHTCPP_APP_SOURCES})

# Enable MOC for Q_OBJECT classes in headers.
set_target_properties(lightcpp PROPERTIES AUTOMOC ON)

target_include_directories(lightcpp PRIVATE
    ${LIGHTCPP_CLANG_C_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppCore/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppLua/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppPlugin/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppProject/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppSettings/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppTesting/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppText/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppWorkbench/Include
)

target_link_libraries(lightcpp PRIVATE
    LightCppCore
    LightCppLua
    LightCppPlugin
    LightCppProject
    LightCppSettings
    LightCppTesting
    LightCppText
    LightCppWorkbench
    Qt6::Widgets
    LightCppWarnings
)

# Staging: copy the exe to dist/ for easy running.
set(LIGHTCPP_DIST_DIR ${CMAKE_BINARY_DIR}/dist)
add_custom_command(TARGET lightcpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LIGHTCPP_DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:lightcpp> ${LIGHTCPP_DIST_DIR}/
)

# Lua plugins stage into dist/plugins/lua, where the sandboxed host scans.
# (Same directory scope as the lightcpp target — CMake requires it.)
file(GLOB LIGHTCPP_LUA_PLUGINS "${CMAKE_SOURCE_DIR}/src/LightCppPlugins/lua/*.lua")
foreach(script ${LIGHTCPP_LUA_PLUGINS})
    add_custom_command(TARGET lightcpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${LIGHTCPP_DIST_DIR}/plugins/lua
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${script} ${LIGHTCPP_DIST_DIR}/plugins/lua/
        COMMENT "Staging ${script} into dist/plugins/lua"
    )
endforeach()

# WinDeployQt: auto-deploy Qt DLLs next to the exe.
# LIGHTCPP_DEPLOY_QT=OFF skips it: the deployment step needs a working pipe and
# a writable Qt install, neither of which a CI runner should have to provide,
# and no test binary depends on the deployed copy.
if(WIN32 AND LIGHTCPP_DEPLOY_QT)
    find_program(WINDOWS_DEPLOY_QT NAMES windeployqt6 windeployqt
        HINTS ${CMAKE_PREFIX_PATH}/bin $ENV{PATH})
    if(WINDOWS_DEPLOY_QT)
        add_custom_command(TARGET lightcpp POST_BUILD
            COMMAND ${WINDOWS_DEPLOY_QT}
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    --release
                    ${LIGHTCPP_DIST_DIR}/$<TARGET_FILE_NAME:lightcpp>
            COMMENT "Deploying Qt DLLs via windeployqt"
        )
    endif()
endif()

# Static-link the C++ runtime so the exe doesn't need UCRT DLLs in the sandbox.
if(MINGW)
    target_link_options(lightcpp PRIVATE -static-libgcc -static-libstdc++)
endif()

install(TARGETS lightcpp RUNTIME DESTINATION bin)
