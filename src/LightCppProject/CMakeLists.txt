# LightCppProject: everything the IDE knows about a project on disk.
#
# Build configurations, the CMake and Dev-C++ project models, git status
# parsing, and recognising what a directory contains. Qt Core only — no
# widgets — so all of it can be unit-tested without a display, which is the
# point: these are the parts where a wrong answer is silent (a flag table, an
# INI dialect, a porcelain parser, "is this folder a project").

add_library(LightCppProject STATIC
    Source/BuildConfiguration.cpp
    Source/CMakeProject.cpp
    Source/CompilerDiagnostics.cpp
    Source/DevCppProject.cpp
    Source/FolderContents.cpp
    Source/GitStatus.cpp
)

add_library(LightCpp::Project ALIAS LightCppProject)

target_include_directories(LightCppProject PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(LightCppProject PUBLIC cxx_std_20)

target_link_libraries(LightCppProject
    PUBLIC Qt6::Core
    PRIVATE LightCppWarnings
)
