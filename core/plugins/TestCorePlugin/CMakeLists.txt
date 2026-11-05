# test-core：工具层，测试发现与结果汇总。
lightcpp_add_plugin(lightcpp_plugin_test_core
    OUTPUT_OUT test_core
    SOURCES TestCorePlugin.cpp TestCoreSpec.cpp)
target_compile_definitions(lightcpp_plugin_test_core PRIVATE
    LIGHTC_PLUGIN_ID="test-core"
    LIGHTC_PLUGIN_VERSION="0.2.0")
