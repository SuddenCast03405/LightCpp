# ninja：构建系统层插件，复用 BuildSystemCore 的适配器实现。
lightcpp_add_plugin(lightcpp_plugin_ninja
    OUTPUT_OUT ninja
    SOURCES NinjaPlugin.cpp)
target_link_libraries(lightcpp_plugin_ninja PRIVATE lightcpp_buildsystem_core)
target_compile_definitions(lightcpp_plugin_ninja PRIVATE
    LIGHTC_PLUGIN_ID="ninja"
    LIGHTC_PLUGIN_VERSION="0.1.0")
target_compile_definitions(lightcpp_plugin_ninja PRIVATE LIGHTC_HAVE_BUILD_SYSTEM_CORE=1)
