# clang-core：语言层，提供 clangd LSP 引擎与 clang 工具链。
lightcpp_add_plugin(lightcpp_plugin_clang_core
    OUTPUT_OUT clang_core
    SOURCES ClangCorePlugin.cpp ClangCoreSpec.cpp)
target_compile_definitions(lightcpp_plugin_clang_core PRIVATE
    LIGHTC_PLUGIN_ID="clang-core"
    LIGHTC_PLUGIN_VERSION="0.1.0")
