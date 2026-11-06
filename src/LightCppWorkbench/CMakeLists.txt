# LightCppWorkbench — the IDE domains that are pure text processing.
#
# Four domains that lightcpp.1.md asks for and that share one property: every
# piece of them is "read text a tool printed, decide something, show it". None
# needs a QProcess of its own, and none needs a widget, so all of it compiles
# into one Qt-Core-only library that the test suite can drive directly.
#
#   Source/AnalysisReport.cpp  static analysis (§七): cppcheck, clang-tidy,
#                              scan-build, PVS-Studio, Coverity, compiler
#                              warnings, and a complexity metric.
#   Source/GitModel.cpp        version control (§八): branches, tags, remotes,
#                              stashes, blame, conflicts, and the command each
#                              operation needs.
#   Source/MarkupRender.cpp    documentation (§十一): Markdown and a
#                              reStructuredText subset rendered to HTML, and
#                              Doxygen comment parsing.
#   Source/QtProjectFiles.cpp  Qt ecosystem (§十二): .pro/.pri, .ui, .qrc.
#   Source/QtPreview.cpp       the preview half of §十二: .ui / .qrc / QML.
#   Source/LspModel.cpp        LSP answers as data (§六): semantic tokens, call
#                              and type hierarchy, class view, call graph.
#   Source/EditorTools.cpp     include sorting and the gutter's line diff (§六/§八).
#   Source/DebugSymbols.cpp    the debugger's symbol layer (§五).
#   Source/GitExtra.cpp        blame blocks, three-way merge, signing (§八).
#   Source/PackageManager.cpp  vcpkg / conan / hunter / apt / brew / winget (§九).
#   Source/Viewers.cpp         hex and log decoding, binary sniffing (§九).
#   Source/BuildOptions.cpp    cross toolchain, ccache, targets, compile
#                              commands (§三).
#   Source/DocsCatalog.cpp     the documentation catalogue (§十一).
#   Source/ExternalTools.cpp   the user's own tools (§九): template expansion
#                              and the quoting each shell needs.
#   Source/TerminalEmulator.cpp the terminal's screen model (§九): CSI/OSC,
#                              UTF-8 across chunks, scrollback.
#   Source/ShellIntegration.cpp the prompt marks (§九): OSC 133 parsing and
#                              the command history it produces.

add_library(LightCppWorkbench STATIC
    Source/AnalysisReport.cpp
    Source/BuildOptions.cpp
    Source/DebugSymbols.cpp
    Source/DocsCatalog.cpp
    Source/EditorTools.cpp
    Source/ExternalTools.cpp
    Source/GitExtra.cpp
    Source/GitModel.cpp
    Source/LspModel.cpp
    Source/MarkupRender.cpp
    Source/PackageManager.cpp
    Source/QtPreview.cpp
    Source/QtProjectFiles.cpp
    Source/ShellIntegration.cpp
    Source/TerminalEmulator.cpp
    Source/Viewers.cpp
)

add_library(LightCpp::Workbench ALIAS LightCppWorkbench)

target_include_directories(LightCppWorkbench PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(LightCppWorkbench PUBLIC cxx_std_20)

target_link_libraries(LightCppWorkbench
    PUBLIC Qt6::Core
    PRIVATE Qt6::Xml
    PRIVATE LightCppWarnings
)
