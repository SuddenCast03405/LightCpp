// 插件公共头：把 ABI 样板收敛成两个宏。
//
// 用法（每个插件只有这三行）：
//   namespace { int activate(const LightCppApi* api) { ... return 0; } }  // namespace
//   LIGHTC_PLUGIN_DEFINE(activate, nullptr)
//
// 插件 id 与版本由 CMake 通过编译定义注入，保证与清单一致：
//   目标里必须有 LIGHTC_PLUGIN_ID="<清单 id>"，可选 LIGHTC_PLUGIN_VERSION="x.y.z"。
#pragma once

#include <cstddef>
#include <string>

#include "lightcpp/plugin_api.h"

#ifndef LIGHTC_PLUGIN_ID
#error "缺少 LIGHTC_PLUGIN_ID 编译定义（应与 plugins/<id>/plugin.json 的 id 一致）"
#endif
#ifndef LIGHTC_PLUGIN_VERSION
#define LIGHTC_PLUGIN_VERSION "0.1.0"
#endif

// 注意：宏体内不能出现 // 注释，否则续行符会被注释吞掉。
#define LIGHTC_PLUGIN_DEFINE(activateFn, deactivateFn)                                  \
    extern "C" LIGHTC_ABI_EXPORT const LightCppPluginInfo* lightcpp_plugin_entry(void) { \
        static const LightCppPluginInfo info = {LIGHTC_API_VERSION, LIGHTC_PLUGIN_ID,    \
                                                LIGHTC_PLUGIN_VERSION, activateFn,      \
                                                deactivateFn};                          \
        return &info;                                                                   \
    }

namespace lightcpp::plugin {

/// 注册一组字符串贡献点，例如 buildSystems 下的 "cmake"。
inline int registerList(const LightCppApi* api, const char* pluginId, const char* type,
                        const char* const* items, std::size_t count) {
    if (api == nullptr || api->registerContribution == nullptr) {
        return -1;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (api->registerContribution(api->host, pluginId, type, items[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

/// 便捷重载：从数组推导元素个数。
template <std::size_t N>
inline int registerList(const LightCppApi* api, const char* pluginId, const char* type,
                        const char* const (&items)[N]) {
    return registerList(api, pluginId, type, items, N);
}

/// 注册单个贡献点。
inline int registerOne(const LightCppApi* api, const char* pluginId, const char* type,
                       const char* id) {
    if (api == nullptr || api->registerContribution == nullptr) {
        return -1;
    }
    return api->registerContribution(api->host, pluginId, type, id);
}

/// 写日志（级别：0 debug / 1 info / 2 warn / 3 error）。
inline void log(const LightCppApi* api, int level, const std::string& message) {
    if (api != nullptr && api->log != nullptr) {
        api->log(api->host, level, message.c_str());
    }
}

}  // namespace lightcpp::plugin
