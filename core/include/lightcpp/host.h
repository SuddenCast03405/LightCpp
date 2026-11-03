// 插件宿主：把清单从"静态数据"变成"可加载的运行时"。
//
// 一个插件由两部分组成：
//   1. plugins/<id>/plugin.json —— 清单（贡献点声明）
//   2. plugins/<id>/lib<id>.so|.dylib|<id>.dll —— 可选的原生实现
//
// 清单声明了需要原生实现的贡献点（buildSystems / debuggers / lsp / testFramework ...）时，
// 宿主必须能加载到对应动态库，否则在加载阶段就报错，而不是等到用户点按钮才发现。
// 这样一来"清单说有、实现没有"这类飘移会在启动时暴露。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lightcpp/manifest.h"

namespace lightcpp {

enum class Level { Debug, Info, Warn, Error };

using LogSink = void (*)(Level level, const char* target, const char* message, void* user);

struct HostOptions {
    /// 插件根目录，通常是 <repo>/plugins。
    std::string pluginsDirectory;
    /// 构建产物中的实现库目录（如 core/build/bin/plugins）；为空时只查插件目录。
    std::string buildOutputDirectory;
    /// 是否允许缺少原生实现（只做清单级自检时打开）。
    bool requireNative = false;
    /// 只激活这些插件；为空表示全部可激活插件。
    std::vector<std::string> only;
    LogSink sink = nullptr;
    void* user = nullptr;
};

struct ContributionRecord {
    std::string type;
    std::string id;
    std::string provider;
};

struct PluginState {
    std::string id;
    std::string version;
    bool nativeLoaded = false;
    bool activated = false;
    std::string libraryPath;
    std::vector<ContributionRecord> contributions;
    std::vector<std::string> skippedReasons;
};

class Host {
public:
    explicit Host(HostOptions options);
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    /// 加载清单、解析依赖顺序、加载原生库并调用插件入口。
    bool load(const Repository& repo);

    /// 按贡献点类型取注册表。
    std::vector<ContributionRecord> contributions(const std::string& type) const;
    /// 某类贡献点的实例（如 buildSystems -> cmake）。
    std::vector<std::string> contributionIds(const std::string& type) const;
    /// 运行时选定的 LSP 引擎（依据互斥组）。
    std::string activeEngine(const std::string& group) const;

    const std::vector<PluginState>& states() const { return states_; }
    const std::vector<std::string>& errors() const { return errors_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    std::size_t activatedCount() const;

    /// 供插件调用的宿主 API；插件在入口里通过它注册贡献点。
    void registerContribution(const std::string& pluginId, const std::string& type, const std::string& id);

    /// 供 plugin_api 的 log 回调转发。
    void logFromPlugin(int level, const std::string& message);

    static std::string libraryName(const std::string& id);
    static std::string platformTag();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<PluginState> states_;
    std::vector<std::string> errors_;
    std::vector<std::string> warnings_;
};

}  // namespace lightcpp
