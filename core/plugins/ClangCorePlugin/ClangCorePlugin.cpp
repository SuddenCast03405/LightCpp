// clang-core：语言层插件，提供 clangd LSP 引擎与 clang 工具链。
//
// 规划里"单文件格式化走 clangd，项目级走 clang-format"是两条独立能力，
// 参数构造放在 ClangCoreSpec.h，宿主可直接复用，不必再写一遍。
#include "ClangCoreSpec.h"
#include "plugin_helpers.h"

namespace {

int activate(const LightCppApi* api) {
    if (lightcpp::plugin::registerOne(api, LIGHTC_PLUGIN_ID, "lsp", "clangd") != 0) {
        return 1;
    }
    const char* const tools[] = {"clang-format", "clang-tidy", "clang"};
    return lightcpp::plugin::registerList(api, LIGHTC_PLUGIN_ID, "toolchain", tools);
}

}  // namespace

LIGHTC_PLUGIN_DEFINE(activate, nullptr)
