# LightCppApp — IDE GUI application (Qt6 Widgets)

set(LIGHTCPP_APP_SOURCES
    main.cpp
    Include/LightCpp/App/BatchFormatter.hpp
    Include/LightCpp/App/CodeEditor.hpp
    Include/LightCpp/App/CodeFormatter.hpp
    Include/LightCpp/App/CompletionPopup.hpp
    Include/LightCpp/App/ConsoleDecoder.hpp
    Include/LightCpp/App/CppSyntaxHighlighter.hpp
    Include/LightCpp/App/DefaultSettings.hpp
    Include/LightCpp/App/DebuggerController.hpp
    Include/LightCpp/App/FileExplorer.hpp
    Include/LightCpp/App/FindBar.hpp
    Include/LightCpp/App/FindScan.hpp
    Include/LightCpp/App/GitClient.hpp
    Include/LightCpp/App/LanguageServerClient.hpp
    Include/LightCpp/App/LibClangCompleter.hpp
    Include/LightCpp/App/LspLocationsDialog.hpp
    Include/LightCpp/App/MsvcToolchain.hpp
    Include/LightCpp/App/OutputPanel.hpp
    Include/LightCpp/App/MainWindow.hpp
    Include/LightCpp/App/ProcessShutdown.hpp
    Include/LightCpp/App/ProjectSearchDialog.hpp
    Include/LightCpp/App/SettingsDialog.hpp
    Include/LightCpp/App/StartupPage.hpp
    Include/LightCpp/App/ToolchainHeaders.hpp
    Include/LightCpp/App/ToolchainPaths.hpp
    Source/BatchFormatter.cpp
    Source/CodeEditor.cpp
    Source/CodeFormatter.cpp
    Source/CompileDatabase.cpp
    Source/CompletionPopup.cpp
    Source/ConsoleDecoder.cpp
    Source/CppSyntaxHighlighter.cpp
    Source/DefaultSettings.cpp
    Source/DebuggerController.cpp
    Source/FileExplorer.cpp
    Source/FindBar.cpp
    Source/FindScan.cpp
    Source/GitClient.cpp
    Source/LanguageServerClient.cpp
    Source/LibClangCompleter.cpp
    Source/LspLocationsDialog.cpp
    Source/MsvcToolchain.cpp
    Source/OutputPanel.cpp
    Source/MainWindow.cpp
    Source/ProcessShutdown.cpp
    Source/ProjectSearchDialog.cpp
    Source/SettingsDialog.cpp
    Source/StartupPage.cpp
    Source/TerminalLaunch.cpp
    Source/TextColumns.cpp
)

add_executable(lightcpp ${LIGHTCPP_APP_SOURCES})

# Enable MOC for Q_OBJECT classes in headers.
set_target_properties(lightcpp PROPERTIES AUTOMOC ON)

target_include_directories(lightcpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppCore/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppLua/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppPlugin/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppProject/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppSettings/Include
    ${CMAKE_SOURCE_DIR}/src/LightCppText/Include
)

target_link_libraries(lightcpp PRIVATE
    LightCppCore
    LightCppLua
    LightCppPlugin
    LightCppProject
    LightCppSettings
    LightCppText
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
file(GLOB LIGHTCPP_LUA_PLUGINS "${CMAKE_SOURCE_DIR}/plugins/lua/*.lua")
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
if(WIN32)
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
