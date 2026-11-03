// 插件 ABI。
//
// 插件是一份动态库，导出唯一的 C 符号 lightcpp_plugin_entry。用 C 函数而不是 C++ 类，
// 是为了让插件作者不必和宿主用同一编译器版本（std::string 的 ABI 在跨编译器时不可靠）。
// 生命周期钩子只做"注册贡献点"和"清理"，真正的业务能力由宿主按贡献点 ID 调度。
#pragma once

#include <cstdint>

#if defined(_WIN32)
#define LIGHTC_ABI_EXPORT __declspec(dllexport)
#else
#define LIGHTC_ABI_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// 宿主 API 版本；插件与宿主不匹配时宿主会拒绝加载而不是崩溃。
#define LIGHTC_API_VERSION 1u

typedef struct LightCppApi {
    uint32_t apiVersion;
    void* host;  // 不透明指针，传回下面的函数
    /// 注册一个贡献点。type 如 "buildSystems"，id 如 "cmake"。
    int (*registerContribution)(void* host, const char* pluginId, const char* type, const char* id);
    /// 写日志。
    void (*log)(void* host, int level, const char* message);
} LightCppApi;

typedef struct LightCppPluginInfo {
    uint32_t apiVersion;
    const char* id;
    const char* version;
    /// 一次性的初始化；返回 0 表示成功。
    int (*activate)(const LightCppApi* api);
    void (*deactivate)(void);
} LightCppPluginInfo;

/// 所有原生插件必须导出：
///   LIGHTC_ABI_EXPORT const LightCppPluginInfo* lightcpp_plugin_entry(void);
typedef const LightCppPluginInfo* (*LightCppEntryFn)(void);

#define LIGHTC_PLUGIN_EXPORT extern "C" LIGHTC_ABI_EXPORT
#define LIGHTC_PLUGIN_SYMBOL "lightcpp_plugin_entry"

#ifdef __cplusplus
}
#endif
