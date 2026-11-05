// cmake 插件入口：只注册 buildSystems 贡献点，行为由 BuildSystemCore 提供的描述决定。
#include "BuildSystemFactory.h"
#include "plugin_helpers.h"

namespace {

int activate(const LightCppApi* api) {
    const char* const systems[] = {LIGHTC_PLUGIN_ID};
    return lightcpp::plugin::registerList(api, LIGHTC_PLUGIN_ID, "buildSystems", systems);
}

}  // namespace

LIGHTC_PLUGIN_DEFINE(activate, nullptr)
