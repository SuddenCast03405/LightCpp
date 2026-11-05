# make：构建系统层插件，复用 BuildSystemCore 的适配器实现。
lightcpp_add_plugin(lightcpp_plugin_make
    OUTPUT_OUT make
    SOURCES MakePlugin.cpp)
target_link_libraries(lightcpp_plugin_make PRIVATE lightcpp_buildsystem_core)
target_compile_definitions(lightcpp_plugin_make PRIVATE
    LIGHTC_PLUGIN_ID="make"
    LIGHTC_PLUGIN_VERSION="0.2.0")
target_compile_definitions(lightcpp_plugin_make PRIVATE LIGHTC_HAVE_BUILD_SYSTEM_CORE=1)
