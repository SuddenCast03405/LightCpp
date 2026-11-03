#include "lightcpp/docgen.h"

#include <algorithm>
#include <map>

#include "lightcpp/fs.h"
#include "lightcpp/text.h"
#include "lightcpp/validator.h"

namespace lightcpp::docs {
namespace {

const char* kHeader = "<!-- 由 lightcpp-pm docs 生成，请勿手工编辑 -->\n";

const std::vector<std::pair<Layer, const char*>> kLayerLabels = {
    {Layer::Core, "核心基础层"},
    {Layer::Language, "语言服务器与语言支持"},
    {Layer::BuildFramework, "项目与构建系统核心框架"},
    {Layer::BuildSystem, "具体项目 / 构建系统支持"},
    {Layer::Debugger, "调试器"},
    {Layer::Tooling, "工具与集成"},
    {Layer::Pack, "扩展包"},
};

std::string backtick(const std::string& s) { return "`" + s + "`"; }

std::string joinBackticks(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        out += (i == 0 ? "" : ", ") + backtick(items[i]);
    }
    return out;
}

}  // namespace

std::string renderPlugins(const Repository& repo) {
    std::size_t packs = 0;
    for (const Plugin& p : repo.plugins) {
        if (p.isPack()) {
            ++packs;
        }
    }
    std::string out = kHeader;
    out += "# 插件清单\n\n";
    out += "共 **" + std::to_string(repo.plugins.size()) + "** 个组件：" +
           std::to_string(repo.plugins.size() - packs) + " 个插件，" + std::to_string(packs) +
           " 个扩展包。\n\n";

    for (const auto& [layer, label] : kLayerLabels) {
        std::vector<const Plugin*> members = repo.byLayer(layer);
        if (members.empty()) {
            continue;
        }
        std::sort(members.begin(), members.end(),
                  [](const Plugin* a, const Plugin* b) { return a->id < b->id; });
        out += "## ";
        out += label;
        out += "\n\n";
        for (const Plugin* p : members) {
            out += "### " + backtick(p->id) + " — " + p->nameZh + "\n\n" + p->description + "\n\n";
            std::vector<std::string> deps;
            for (const auto& [dep, constraint] : p->dependencies) {
                (void)constraint;
                deps.push_back(dep);
            }
            out += "- 依赖：" + (deps.empty() ? std::string("无") : joinBackticks(deps)) + "\n";
            if (!p->capabilitySets.empty()) {
                out += "- 功能集：" + joinBackticks(p->capabilitySets) + "\n";
            }
            if (p->platforms.empty()) {
                out += "- 平台：全平台\n";
            } else {
                std::string joined;
                for (std::size_t i = 0; i < p->platforms.size(); ++i) {
                    joined += (i == 0 ? "" : ", ") + p->platforms[i];
                }
                out += "- 平台：" + joined + "\n";
            }
            if (!p->recommends.empty()) {
                out += "- 推荐：";
                bool first = true;
                for (const auto& [id, rec] : p->recommends) {
                    if (!first) {
                        out += "；";
                    }
                    first = false;
                    out += backtick(id) + "（" + (rec.autoInstall ? "自动安装" : "建议安装") + "）";
                }
                out += "\n";
            }
            out += "\n";
        }
    }
    return out;
}

std::string renderFeatures(const Repository& repo) {
    // 域顺序以 catalog 中的 domains 为准，保持与规划一致。
    std::map<std::string, std::string> domainLabels;
    std::vector<std::string> domainOrder;
    for (const json::Value& domain : repo.catalog.root["domains"].elements()) {
        if (domain.isObject() && domain["id"].isString()) {
            domainLabels[domain["id"].asString()] = domain["label"].asString();
            domainOrder.push_back(domain["id"].asString());
        } else if (domain.isString()) {
            domainLabels[domain.asString()] = domain.asString();
            domainOrder.push_back(domain.asString());
        }
    }

    std::string out = kHeader;
    out += "# 功能目录\n\n";
    out += "规划中的每一条能力都登记在此，并用 `plugins` / `except` 标注归属，"
           "`lightcpp-pm validate` 会强制要求所有 `required` 功能有归属。\n\n";

    std::size_t total = 0;
    for (const std::string& domainId : domainOrder) {
        std::vector<std::vector<std::string>> rows;
        for (const CatalogSet& set : repo.catalog.sets) {
            if (set.domain != domainId) {
                continue;
            }
            for (const json::Value& f : set.features.elements()) {
                if (!f.isObject()) {
                    continue;
                }
                ++total;
                const std::vector<std::string> owners = featureOwners(set, f);
                const std::string status = f["status"].isString() ? f["status"].asString() : "required";
                const std::string statusLabel =
                    status == "planned" ? "规划中" : (status == "known-issue" ? "已知问题" : "目标");
                rows.push_back({backtick(f["id"].asString()), f["label"].asString(), statusLabel,
                                owners.empty() ? std::string("—") : joinBackticks(owners)});
            }
        }
        if (rows.empty()) {
            continue;
        }
        out += "## " + domainLabels[domainId] + "\n\n";
        out += text::renderTable({"功能 ID", "说明", "状态", "归属插件"}, rows);
        out += "\n";
    }
    out += "合计 **" + std::to_string(total) + "** 条功能。\n";
    return out;
}

namespace {

struct Target {
    std::string path;
    std::string content;
};

std::vector<Target> targetsFor(const Repository& repo) {
    return {
        {fs::join(fs::join(repo.root, "docs"), "PLUGINS.md"), renderPlugins(repo)},
        {fs::join(fs::join(repo.root, "docs"), "FEATURES.md"), renderFeatures(repo)},
    };
}

}  // namespace

bool upToDate(const Repository& repo, std::string* diffHint) {
    for (const Target& target : targetsFor(repo)) {
        std::string disk;
        if (!fs::readFile(target.path, &disk)) {
            if (diffHint != nullptr) {
                *diffHint = "缺少文件 " + fs::filename(target.path);
            }
            return false;
        }
        if (disk != target.content) {
            if (diffHint != nullptr) {
                *diffHint = fs::filename(target.path) + " 与清单不一致，请运行 `lightcpp-pm docs`";
            }
            return false;
        }
    }
    return true;
}

bool write(const Repository& repo, std::string* error) {
    for (const Target& target : targetsFor(repo)) {
        if (!fs::writeFile(target.path, target.content)) {
            if (error != nullptr) {
                *error = "无法写入 " + target.path;
            }
            return false;
        }
    }
    return true;
}

}  // namespace lightcpp::docs
