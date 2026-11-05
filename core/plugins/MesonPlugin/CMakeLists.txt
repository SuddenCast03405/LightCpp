# meson：构建系统层插件，复用 BuildSystemCore 的适配器实现。
lightcpp_add_plugin(lightcpp_plugin_meson
    OUTPUT_OUT meson
    SOURCES MesonPlugin.cpp)
target_link_libraries(lightcpp_plugin_meson PRIVATE lightcpp_buildsystem_core)
target_compile_definitions(lightcpp_plugin_meson PRIVATE
    LIGHTC_PLUGIN_ID="meson"
    LIGHTC_PLUGIN_VERSION="0.2.0")
target_compile_definitions(lightcpp_plugin_meson PRIVATE LIGHTC_HAVE_BUILD_SYSTEM_CORE=1)
