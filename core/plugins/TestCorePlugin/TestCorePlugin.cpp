// test-core：工具层插件，注册 testExplorer 贡献点；解析逻辑见 TestCoreSpec.h。
#include "TestCoreSpec.h"
#include "plugin_helpers.h"

namespace {

int activate(const LightCppApi* api) {
    return lightcpp::plugin::registerOne(api, LIGHTC_PLUGIN_ID, "testExplorer", "test-core");
}

}  // namespace

LIGHTC_PLUGIN_DEFINE(activate, nullptr)
