// cpp-base：核心层插件。
//
// 职责与清单一致：注册语言（c / cpp）与编译器提供者（libclang / gcc / llvm / msvc）。
// 这里不放语法高亮的实现细节，那是编辑器侧的事；插件只负责把能力"登记"到宿主，
// 让上层（clang-core、构建系统、调试器）能通过贡献点找到它。
#include "plugin_helpers.h"

namespace {

const char* const kLanguages[] = {"c", "cpp"};
const char* const kCompilers[] = {"libclang", "gcc", "llvm", "msvc"};

int activate(const LightCppApi* api) {
    using lightcpp::plugin::registerList;
    if (registerList(api, LIGHTC_PLUGIN_ID, "languages", kLanguages) != 0) {
        return 1;
    }
    if (registerList(api, LIGHTC_PLUGIN_ID, "compilers", kCompilers) != 0) {
        return 2;
    }
    return 0;
}

}  // namespace

LIGHTC_PLUGIN_DEFINE(activate, nullptr)
