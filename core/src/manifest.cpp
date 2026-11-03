#include "lightcpp/manifest.h"

#include <algorithm>
#include <set>

#include "lightcpp/fs.h"

namespace lightcpp {
namespace {

std::vector<std::string> toStringList(const json::Value& v) {
    std::vector<std::string> out;
    for (const json::Value& item : v.elements()) {
        if (item.isString()) {
            out.push_back(item.asString());
        }
    }
    return out;
}

/// 允许 kinds 白名单内的取值，其他一律归零（交给 validator 报错）。
void normalizeStringArray(json::Value& parent, const char* key, std::vector<std::string>* out,
                          std::vector<std::string>* issues) {
    json::Value& field = parent[key];
    if (field.isNull()) {
        return;
    }
    if (field.isString()) {
        // 单个字符串是手工写清单时最常见的形态，纠正成数组并记录。
        issues->push_back(std::string(key) + " 必须是数组，实际是字符串（是否少写了方括号？）");
        json::Value fixed = json::Value::array();
        fixed.push(field);
        field = fixed;
    } else if (!field.isArray()) {
        issues->push_back(std::string(key) + " 必须是数组");
        field = json::Value::array();
    }
    *out = toStringList(field);
    for (const std::string& item : *out) {
        if (item.empty()) {
            issues->push_back(std::string(key) + " 中存在空字符串");
        }
    }
}

}  // namespace

const char* layerId(Layer layer) {
    switch (layer) {
        case Layer::Core: return "core";
        case Layer::Language: return "language";
        case Layer::BuildFramework: return "build-framework";
        case Layer::BuildSystem: return "build-system";
        case Layer::Debugger: return "debugger";
        case Layer::Tooling: return "tooling";
        case Layer::Pack: return "pack";
    }
    return "tooling";
}

bool parseLayer(const std::string& text, Layer* out) {
    static const std::pair<const char*, Layer> kLayers[] = {
        {"core", Layer::Core},
        {"language", Layer::Language},
        {"build-framework", Layer::BuildFramework},
        {"build-system", Layer::BuildSystem},
        {"debugger", Layer::Debugger},
        {"tooling", Layer::Tooling},
        {"pack", Layer::Pack},
    };
    for (const auto& [id, layer] : kLayers) {
        if (text == id) {
            *out = layer;
            return true;
        }
    }
    return false;
}

bool Plugin::hasPlatform(const std::string& platform) const {
    return platforms.empty() || std::find(platforms.begin(), platforms.end(), platform) != platforms.end();
}

bool Plugin::hasContribution(const std::string& key) const {
    return contributes.contains(key) && !contributes[key].isNull();
}

std::string Plugin::dependencyVersion(const std::string& dep) const {
    for (const auto& entry : dependencies) {
        if (entry.first == dep) {
            return entry.second;
        }
    }
    return {};
}

const CatalogSet* Catalog::findSet(const std::string& id) const {
    for (const CatalogSet& set : sets) {
        if (set.id == id) {
            return &set;
        }
    }
    return nullptr;
}

bool Catalog::hasDomain(const std::string& id) const {
    return std::find(domains.begin(), domains.end(), id) != domains.end();
}

std::size_t Catalog::featureCount() const {
    std::size_t count = 0;
    for (const CatalogSet& set : sets) {
        count += set.features.size();
    }
    return count;
}

NormalizedPlugin normalizePlugin(const json::Value& raw, const std::string& directory,
                                 const std::string& sourcePath) {
    NormalizedPlugin result;
    std::vector<std::string>& issues = result.typeIssues;
    Plugin& p = result.plugin;
    p.directory = directory;
    p.sourcePath = sourcePath;

    if (!raw.isObject()) {
        issues.push_back("清单根节点必须是对象");
        return result;
    }
    const json::Value& root = raw;

    auto readString = [&](const char* key) -> std::string {
        const json::Value& v = root[key];
        if (v.isNull()) {
            return {};
        }
        if (!v.isString()) {
            issues.push_back(std::string(key) + " 必须是字符串");
            return {};
        }
        return v.asString();
    };

    p.id = readString("id");
    p.name = readString("name");
    p.nameZh = readString("nameZh");
    p.version = readString("version");
    p.publisher = readString("publisher");
    p.description = readString("description");

    const json::Value& kind = root["kind"];
    if (kind.isString()) {
        if (kind.asString() == "pack") {
            p.kind = Kind::Pack;
        } else if (kind.asString() == "plugin") {
            p.kind = Kind::Plugin;
        } else {
            issues.push_back("kind 只能是 plugin 或 pack，实际是 " + kind.asString());
            p.kind = Kind::Plugin;
        }
    } else if (!kind.isNull()) {
        issues.push_back("kind 必须是字符串");
    }

    const json::Value& layer = root["layer"];
    if (layer.isString()) {
        if (!parseLayer(layer.asString(), &p.layer)) {
            issues.push_back("layer 取值非法：" + layer.asString());
            p.layer = Kind::Pack == p.kind ? Layer::Pack : Layer::Tooling;
        }
    } else if (!layer.isNull()) {
        issues.push_back("layer 必须是字符串");
    }

    normalizeStringArray(*const_cast<json::Value*>(&root), "platforms", &p.platforms, &issues);
    normalizeStringArray(*const_cast<json::Value*>(&root), "capabilitySets", &p.capabilitySets, &issues);
    normalizeStringArray(*const_cast<json::Value*>(&root), "activation", &p.activation, &issues);
    normalizeStringArray(*const_cast<json::Value*>(&root), "tags", &p.tags, &issues);

    const json::Value& deps = root["dependencies"];
    if (deps.isNull()) {
        // 缺省即无依赖。
    } else if (deps.isArray()) {
        // 数组形态：接受纯字符串或 {id, version}，统一纠正成对象。
        issues.push_back("dependencies 必须是对象（id -> 版本约束），实际是数组");
        for (const json::Value& item : deps.elements()) {
            if (item.isString()) {
                p.dependencies.emplace_back(item.asString(), "*");
            } else if (item.isObject() && item["id"].isString()) {
                const json::Value& v = item["version"];
                p.dependencies.emplace_back(item["id"].asString(), v.isString() ? v.asString() : "*");
            }
        }
    } else if (deps.isObject()) {
        for (const auto& [id, constraint] : deps.members()) {
            if (constraint.isString()) {
                p.dependencies.emplace_back(id, constraint.asString());
            } else {
                issues.push_back("dependencies." + id + " 的值必须是版本约束字符串");
                p.dependencies.emplace_back(id, "*");
            }
        }
    } else {
        issues.push_back("dependencies 必须是对象");
    }

    const json::Value& recommends = root["recommends"];
    if (!recommends.isNull()) {
        if (!recommends.isObject()) {
            issues.push_back("recommends 必须是对象");
        } else {
            for (const auto& [id, item] : recommends.members()) {
                if (!item.isObject()) {
                    issues.push_back("recommends." + id + " 必须是对象");
                    continue;
                }
                Recommendation rec;
                const json::Value& version = item["version"];
                if (version.isString()) {
                    rec.version = version.asString();
                } else {
                    issues.push_back("recommends." + id + ".version 必须是字符串");
                    rec.version = "*";
                }
                rec.reason = item["reason"].asString();
                const json::Value& autoInstall = item["autoInstall"];
                if (!autoInstall.isNull() && !autoInstall.isBool()) {
                    issues.push_back("recommends." + id + ".autoInstall 必须是布尔值");
                }
                rec.autoInstall = autoInstall.asBool(false);
                p.recommends[id] = rec;
            }
        }
    }

    const json::Value& contributes = root["contributes"];
    if (contributes.isNull()) {
        p.contributes = json::Value::object();
    } else if (!contributes.isObject()) {
        issues.push_back("contributes 必须是对象");
        p.contributes = json::Value::object();
    } else {
        p.contributes = contributes;
    }

    if (p.id.empty()) {
        p.id = directory;
    }
    return result;
}

namespace {

/// 在 keys 中查找，常见拼写错误给"是否想写 X"的提示。
std::string knownKeyHint(const std::string& key, const std::vector<std::string>& known) {
    std::string best;
    std::size_t bestDistance = 3;
    for (const std::string& candidate : known) {
        const std::size_t distance = [&] {
            // 简易编辑距离，只关心相近拼写。
            std::vector<std::size_t> prev(candidate.size() + 1);
            std::vector<std::size_t> cur(candidate.size() + 1);
            for (std::size_t j = 0; j <= candidate.size(); ++j) {
                prev[j] = j;
            }
            for (std::size_t i = 1; i <= key.size(); ++i) {
                cur[0] = i;
                for (std::size_t j = 1; j <= candidate.size(); ++j) {
                    const std::size_t cost = key[i - 1] == candidate[j - 1] ? 0 : 1;
                    cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
                }
                prev = cur;
            }
            return prev[candidate.size()];
        }();
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

bool looksLikePluginRoot(const std::string& dir) {
    return fs::isDirectory(fs::join(dir, "plugins")) && fs::isDirectory(fs::join(dir, "catalog"));
}

}  // namespace

Repository Repository::load(const std::string& root) {
    Repository repo;
    std::string resolved = fs::absolute(root);
    // 从执行目录向上找仓库根，允许在任意子目录运行。
    for (int depth = 0; depth < 8 && !looksLikePluginRoot(resolved); ++depth) {
        const std::string up = fs::parent(resolved);
        if (up == resolved) {
            break;
        }
        resolved = up;
    }
    repo.root = resolved;

    const std::string pluginsDir = fs::join(repo.root, "plugins");
    for (const std::string& entry : fs::listDirectory(pluginsDir)) {
        const std::string dir = fs::join(pluginsDir, entry);
        if (!fs::isDirectory(dir)) {
            continue;
        }
        const std::string manifestPath = fs::join(dir, kManifestFile);
        if (!fs::isRegularFile(manifestPath)) {
            // 目录存在但没有清单：通常是删插件时漏删目录，报出来。
            repo.loadErrors.push_back("plugins/" + entry + "：缺少 " + kManifestFile);
            continue;
        }
        json::ParseError error;
        json::Value raw = json::parseFile(manifestPath, &error);
        if (!error.message.empty()) {
            repo.loadErrors.push_back("plugins/" + entry + "/" + kManifestFile + "：" + error.what());
            continue;
        }
        NormalizedPlugin normalized = normalizePlugin(raw, entry, manifestPath);
        for (std::string& issue : normalized.typeIssues) {
            issue = "plugins/" + entry + "/" + kManifestFile + "：" + issue;
        }
        std::set<std::string> known;
        for (const auto& [key, value] : raw.members()) {
            known.insert(key);
        }
        static const std::vector<std::string> kKnown = {
            "$schema",      "id",        "name",        "nameZh",     "version",      "publisher",
            "kind",         "layer",     "description", "platforms",  "engines",      "dependencies",
            "recommends",   "capabilitySets", "activation", "contributes", "tags", "entry",
            "apiVersion",   "permissions",
        };
        for (const std::string& key : known) {
            if (std::find(kKnown.begin(), kKnown.end(), key) == kKnown.end()) {
                const std::string hint = knownKeyHint(key, kKnown);
                normalized.typeIssues.push_back(
                    "plugins/" + entry + "/" + kManifestFile + "：未知字段 \"" + key + "\"" +
                    (hint.empty() ? "" : "，是否想写 \"" + hint + "\"？"));
            }
        }
        repo.plugins.push_back(std::move(normalized.plugin));
        // 未知字段的问题挂到对应插件上，由 validator 统一报 E026。
        repo.plugins.back().sourcePath = manifestPath;
        if (!normalized.typeIssues.empty()) {
            repo.loadErrors.insert(repo.loadErrors.end(), normalized.typeIssues.begin(),
                                   normalized.typeIssues.end());
        }
    }
    std::sort(repo.plugins.begin(), repo.plugins.end(),
              [](const Plugin& a, const Plugin& b) { return a.id < b.id; });

    const std::string featuresPath = fs::join(repo.root, "catalog/features.json");
    json::ParseError featureError;
    json::Value features = json::parseFile(featuresPath, &featureError);
    if (!featureError.message.empty()) {
        repo.loadErrors.push_back("catalog/features.json：" + featureError.what());
    } else {
        repo.catalog.root = features;
        for (const json::Value& domain : features["domains"].elements()) {
            if (domain.isObject() && domain["id"].isString()) {
                repo.catalog.domains.push_back(domain["id"].asString());
            } else if (domain.isString()) {
                repo.catalog.domains.push_back(domain.asString());
            }
        }
        for (const json::Value& set : features["sets"].elements()) {
            if (!set.isObject()) {
                repo.loadErrors.push_back("catalog/features.json：sets 中存在非对象条目");
                continue;
            }
            CatalogSet parsed;
            parsed.id = set["id"].asString();
            parsed.domain = set["domain"].asString();
            parsed.label = set["label"].asString();
            for (const json::Value& p : set["plugins"].elements()) {
                if (p.isString()) {
                    parsed.plugins.push_back(p.asString());
                }
            }
            parsed.features = set["features"].isArray() ? set["features"] : json::Value::array();
            repo.catalog.sets.push_back(std::move(parsed));
        }
    }

    const std::string invariantsPath = fs::join(repo.root, "catalog/invariants.json");
    json::ParseError invariantError;
    json::Value invariants = json::parseFile(invariantsPath, &invariantError);
    if (!invariantError.message.empty()) {
        repo.loadErrors.push_back("catalog/invariants.json：" + invariantError.what());
    } else {
        for (const json::Value& rule : invariants["rules"].elements()) {
            if (!rule.isObject()) {
                continue;
            }
            Invariant parsed;
            parsed.id = rule["id"].asString();
            parsed.type = rule["type"].asString();
            parsed.description = rule["description"].asString();
            parsed.body = rule;
            repo.invariants.push_back(std::move(parsed));
        }
    }

    // 附加不变量：catalog/*.invariants.json。允许在不改动规划原文的情况下追加工程约束。
    for (const std::string& name : fs::listDirectory(fs::join(repo.root, "catalog"))) {
        if (name.size() < 15 || name.compare(name.size() - 15, 15, ".invariants.json") != 0) {
            continue;
        }
        json::ParseError error;
        json::Value extra = json::parseFile(fs::join(fs::join(repo.root, "catalog"), name), &error);
        if (!error.message.empty()) {
            repo.loadErrors.push_back("catalog/" + name + "：" + error.what());
            continue;
        }
        for (const json::Value& rule : extra["rules"].elements()) {
            if (!rule.isObject()) {
                continue;
            }
            Invariant parsed;
            parsed.id = rule["id"].asString();
            parsed.type = rule["type"].asString();
            parsed.description = rule["description"].asString();
            parsed.body = rule;
            repo.invariants.push_back(std::move(parsed));
        }
    }

    return repo;
}

const Plugin* Repository::find(const std::string& id) const {
    for (const Plugin& p : plugins) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

Plugin* Repository::find(const std::string& id) {
    for (Plugin& p : plugins) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

std::vector<const Plugin*> Repository::byLayer(Layer layer) const {
    std::vector<const Plugin*> out;
    for (const Plugin& p : plugins) {
        if (p.layer == layer) {
            out.push_back(&p);
        }
    }
    return out;
}

std::vector<const Plugin*> Repository::sorted() const {
    std::vector<const Plugin*> out;
    out.reserve(plugins.size());
    for (const Plugin& p : plugins) {
        out.push_back(&p);
    }
    return out;
}

}  // namespace lightcpp
