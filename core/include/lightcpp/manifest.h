// 插件清单与功能目录的数据模型。
//
// 清单是唯一事实源（SSOT）：plugins/<id>/plugin.json。
// 这里只负责"读进来并变成稳定形状"，语义规则全部交给 validator。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "lightcpp/json.h"

namespace lightcpp {

inline constexpr const char* kManifestFile = "plugin.json";

enum class Kind { Plugin, Pack };
enum class Layer { Core, Language, BuildFramework, BuildSystem, Debugger, Tooling, Pack };

const char* layerId(Layer layer);
bool parseLayer(const std::string& text, Layer* out);

struct Recommendation {
    std::string version;
    std::string reason;
    bool autoInstall = false;
};

struct Plugin {
    std::string id;
    std::string name;
    std::string nameZh;
    std::string version;
    std::string publisher;
    Kind kind = Kind::Plugin;
    Layer layer = Layer::Tooling;
    std::string description;
    std::vector<std::string> platforms;
    /// 硬依赖：插件 id -> 版本约束
    std::vector<std::pair<std::string, std::string>> dependencies;
    std::map<std::string, Recommendation> recommends;
    std::vector<std::string> capabilitySets;
    std::vector<std::string> activation;
    std::vector<std::string> tags;
    /// 贡献点原文：宿主与校验器各自解释，避免每加一种贡献点就改结构体。
    json::Value contributes = json::Value::object();

    std::string directory;  // plugins/<directory>
    std::string sourcePath;

    bool isPack() const { return kind == Kind::Pack; }
    bool hasPlatform(const std::string& platform) const;
    bool hasContribution(const std::string& key) const;
    std::string dependencyVersion(const std::string& dep) const;
};

struct CatalogSet {
    std::string id;
    std::string domain;
    std::string label;
    std::vector<std::string> plugins;
    json::Value features = json::Value::array();
};

struct Catalog {
    json::Value root;
    std::vector<std::string> domains;
    std::vector<CatalogSet> sets;

    const CatalogSet* findSet(const std::string& id) const;
    bool hasDomain(const std::string& id) const;
    std::size_t featureCount() const;
};

struct Invariant {
    std::string id;
    std::string type;
    std::string description;
    json::Value body;
};

/// 规整结果：字段类型不对时被纠正，并把原始问题记下来交给 validator 报错。
struct NormalizedPlugin {
    Plugin plugin;
    std::vector<std::string> typeIssues;
};

NormalizedPlugin normalizePlugin(const json::Value& raw, const std::string& directory,
                                 const std::string& sourcePath);

struct Repository {
    std::string root;
    std::vector<Plugin> plugins;
    Catalog catalog;
    std::vector<Invariant> invariants;
    json::Object extraCatalog;  // 附加不变量文件，见 Repository::load
    /// 加载期错误：JSON 解析失败、缺 features.json 等。
    std::vector<std::string> loadErrors;

    /// 读取 root 下的 plugins/*/plugin.json、catalog/features.json、catalog/invariants.json。
    ///
    /// 若 root 下没有 plugin.json（例如宿主运行目录），会自动向上查找包含
    /// plugins 与 catalog 的最近祖先目录，因此从任意子目录执行都无需额外参数。
    static Repository load(const std::string& root);

    const Plugin* find(const std::string& id) const;
    Plugin* find(const std::string& id);
    std::vector<const Plugin*> byLayer(Layer layer) const;
    std::vector<const Plugin*> sorted() const;
};

}  // namespace lightcpp
