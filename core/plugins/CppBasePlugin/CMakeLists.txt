# cpp-base：核心层，注册语言与编译器提供者。
lightcpp_add_plugin(lightcpp_plugin_cpp_base
    OUTPUT_OUT cpp_base
    SOURCES CppBasePlugin.cpp)
target_link_libraries(lightcpp_plugin_cpp_base PRIVATE lightcpp_core lightcpp_warnings)
target_compile_definitions(lightcpp_plugin_cpp_base PRIVATE
    LIGHTC_PLUGIN_ID="cpp-base"
    LIGHTC_PLUGIN_VERSION="0.2.0")
