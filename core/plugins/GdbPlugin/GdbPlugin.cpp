// gdb：调试器层插件，注册 debuggers 贡献点；断点逻辑见 GdbBreakpoints.h。
#include "GdbBreakpoints.h"
#include "plugin_helpers.h"

namespace {

int activate(const LightCppApi* api) {
    return lightcpp::plugin::registerOne(api, LIGHTC_PLUGIN_ID, "debuggers", "gdb");
}

}  // namespace

LIGHTC_PLUGIN_DEFINE(activate, nullptr)
