#include "lightcpp/resolver.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace lightcpp {

std::vector<std::string> topologicalOrder(const Repository& repo, const std::vector<std::string>& ids,
                                          std::string* error) {
    std::set<std::string> wanted(ids.begin(), ids.end());
    std::set<std::string> done;
    std::vector<std::string> result;
    std::vector<std::string> trail;
    bool failed = false;

    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (failed || done.count(id) != 0) {
            return;
        }
        if (std::find(trail.begin(), trail.end(), id) != trail.end()) {
            std::string chain;
            for (const std::string& item : trail) {
                chain += item + " -> ";
            }
            chain += id;
            if (error != nullptr) {
                *error = "依赖环: " + chain;
            }
            failed = true;
            return;
        }
        trail.push_back(id);
        if (const Plugin* p = repo.find(id)) {
            for (const auto& [dep, constraint] : p->dependencies) {
                (void)constraint;
                if (wanted.count(dep) != 0) {
                    visit(dep);
                }
            }
        }
        trail.pop_back();
        if (done.insert(id).second) {
            result.push_back(id);
        }
    };

    for (const std::string& id : ids) {
        visit(id);
    }
    if (failed) {
        result.clear();
    }
    return result;
}

Resolution resolve(const Repository& repo, const std::vector<std::string>& roots) {
    Resolution out;
    std::vector<std::string> missing;
    for (const std::string& root : roots) {
        if (repo.find(root) == nullptr) {
            missing.push_back(root);
        }
    }
    if (!missing.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            joined += (i == 0 ? "" : ", ") + missing[i];
        }
        out.errors.push_back("未知插件: " + joined);
        return out;
    }

    std::map<std::string, std::string> origin;
    std::vector<std::string> queue;
    for (const std::string& root : roots) {
        origin[root] = "直接安装";
        queue.push_back(root);
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::string cur = queue[head];
        const Plugin* p = repo.find(cur);
        if (p == nullptr) {
            continue;
        }
        for (const auto& [dep, constraint] : p->dependencies) {
            (void)constraint;
            if (repo.find(dep) == nullptr) {
                out.errors.push_back(p->id + " 依赖的插件不存在: " + dep);
                continue;
            }
            if (origin.count(dep) == 0) {
                origin[dep] = "由 " + cur + " 引入";
                queue.push_back(dep);
            }
        }
        for (const auto& [dep, rec] : p->recommends) {
            if (repo.find(dep) == nullptr) {
                out.errors.push_back(p->id + " 推荐的插件不存在: " + dep);
                continue;
            }
            if (rec.autoInstall && origin.count(dep) == 0) {
                origin[dep] = "自动连带安装（" + cur + "）";
                queue.push_back(dep);
            }
        }
    }

    std::string error;
    const std::vector<std::string> order = topologicalOrder(repo, queue, &error);
    if (!error.empty()) {
        out.errors.push_back(error);
        return out;
    }

    for (const std::string& id : order) {
        const Plugin* p = repo.find(id);
        if (p == nullptr) {
            continue;
        }
        out.steps.push_back({p->id, layerId(p->layer), p->version, origin[id]});
    }

    // 互斥组：同一组内装了多个引擎时提示运行时只能启用其一。
    std::map<std::string, std::vector<std::string>> groups;
    for (const std::string& id : order) {
        const Plugin* p = repo.find(id);
        if (p == nullptr) {
            continue;
        }
        const json::Value& lsp = p->contributes["lsp"];
        if (lsp.isObject() && !lsp["exclusiveGroup"].asString().empty()) {
            groups[lsp["exclusiveGroup"].asString()].push_back(lsp["engine"].asString());
        }
    }
    for (const auto& [group, engines] : groups) {
        if (engines.size() > 1) {
            std::string joined;
            for (std::size_t i = 0; i < engines.size(); ++i) {
                joined += (i == 0 ? "" : " / ") + engines[i];
            }
            out.notes.push_back("互斥组 " + group + " 包含 " + joined + "，运行时仅启用其一");
        }
    }
    return out;
}

}  // namespace lightcpp
