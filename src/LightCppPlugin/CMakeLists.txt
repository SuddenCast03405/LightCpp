# LightCppPlugin — the plugin framework: base interfaces, project-backend
# extension point and the QPluginLoader-based manager. Qt Core only; the
# IdeServices implementation lives in the App (it touches widgets).

add_library(LightCppPlugin STATIC
    Include/LightCpp/Plugin/Plugin.hpp
    Include/LightCpp/Plugin/PluginManager.hpp
    Include/LightCpp/Plugin/PluginTypes.hpp
    Include/LightCpp/Plugin/ProjectBackend.hpp
    Source/PluginManager.cpp
)

# PluginManager is a Q_OBJECT class: AUTOMOC must run for this target or
# the linker fails with an undefined vtable / staticMetaObject.
set_target_properties(LightCppPlugin PROPERTIES AUTOMOC ON)

add_library(LightCpp::Plugin ALIAS LightCppPlugin)

target_include_directories(LightCppPlugin PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(LightCppPlugin PUBLIC cxx_std_20)

target_link_libraries(LightCppPlugin
    PUBLIC Qt6::Core
    PRIVATE LightCppWarnings
)
