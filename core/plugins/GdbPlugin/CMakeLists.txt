# gdb：调试器层，断点命令生成与 MI 输出解析。
lightcpp_add_plugin(lightcpp_plugin_gdb
    OUTPUT_OUT gdb
    SOURCES GdbPlugin.cpp GdbBreakpoints.cpp)
target_compile_definitions(lightcpp_plugin_gdb PRIVATE
    LIGHTC_PLUGIN_ID="gdb"
    LIGHTC_PLUGIN_VERSION="0.1.0")
