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

add_library(LightCppWorkbench STATIC
    Source/AnalysisReport.cpp
    Source/GitModel.cpp
    Source/MarkupRender.cpp
    Source/QtProjectFiles.cpp
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
