// 命令行工具 lightcpp-pm。
//
// 与 tools/lightcpp_pm.py 功能对齐，但命令行解析用逐项匹配而不是下标，
// 避免"新增一个开关就悄悄改变其他命令行为"这类问题。
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lightcpp/docgen.h"
#include "lightcpp/fs.h"
#include "lightcpp/host.h"
#include "lightcpp/json.h"
#include "lightcpp/manifest.h"
#include "lightcpp/resolver.h"
#include "lightcpp/text.h"
#include "lightcpp/validator.h"
#include "lightcpp/version.h"

namespace {

using namespace lightcpp;

// 版本号由 CMake 从 project(VERSION) 注入，避免和 core/CMakeLists.txt 里
// 的版本各写一份、改一处忘一处。独立构建（cmake -S core）时同样生效。
#ifndef LIGHTC_PROJECT_VERSION
#define LIGHTC_PROJECT_VERSION "0.0.0-unknown"
#endif

const char* kVersion = LIGHTC_PROJECT_VERSION;

const std::vector<std::pair<Layer, const char*>> kLayerLabels = {
    {Layer::Core, "核心基础层"},
    {Layer::Language, "语言服务器与语言支持"},
    {Layer::BuildFramework, "项目与构建系统核心框架"},
    {Layer::BuildSystem, "具体项目 / 构建系统支持"},
    {Layer::Debugger, "调试器"},
    {Layer::Tooling, "工具与集成"},
    {Layer::Pack, "扩展包"},
};

void printUsage() {
    std::cout <<
        R"(lightcpp-pm —— LightCpp 插件生态工具链

用法:
  lightcpp-pm <命令> [参数] [--root <目录>]

命令:
  validate [--strict] [--platform <linux|darwin|win32>] [--json]
                校验清单、依赖、功能覆盖与规划不变量
  resolve <插件>... [--json]
                解析安装闭包与安装顺序（含 autoInstall 连带安装）
  info <插件> [--json]
                查看单个插件的依赖、推荐、功能集与贡献点
  graph [--layer <层级>] [--format <text|mermaid|dot>]
                输出依赖图
  matrix [--json]
                插件 x 能力矩阵
  coverage      按功能域统计功能状态
  engines       互斥引擎组（clangd / ccls）
  docs [--check] [--out <目录>]
                生成 docs/PLUGINS.md 与 docs/FEATURES.md；--check 只比对
  host [--require-native] [--only <插件,插件>] [--lib-dir <目录>]
                加载插件宿主：读清单、按依赖顺序加载原生库、注册贡献点
  list [--layer <层级>]
                列出全部组件

全局参数:
  --root <目录>   仓库根目录，默认从当前目录向上查找
  --version, -v   打印版本
  --help, -h      显示本帮助
)";
}

int layerOrder(Layer layer) {
    switch (layer) {
        case Layer::Core: return 0;
        case Layer::Language: return 1;
        case Layer::BuildFramework: return 2;
        case Layer::BuildSystem: return 3;
        case Layer::Debugger: return 4;
        case Layer::Tooling: return 5;
        case Layer::Pack: return 6;
    }
    return 9;
}

int fail(const std::string& message) {
    std::cerr << "错误: " << message << "\n";
    return 2;
}

bool needsValue(const std::string& flag) {
    for (const char* known : {"--root", "--layer", "--format", "--platform", "--out", "--only", "--lib-dir"}) {
        if (flag == known) {
            return true;
        }
    }
    return false;
}

/// 把 --key value 或 --key=value 统一成 key/value。
struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    std::vector<std::string> flags;

    bool has(const std::string& flag) const {
        return std::find(flags.begin(), flags.end(), flag) != flags.end();
    }
    std::string get(const std::string& key, const std::string& fallback = "") const {
        auto it = options.find(key);
        return it == options.end() ? fallback : it->second;
    }
};

bool parseArgs(int argc, char** argv, Args* args, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        // 短选项只支持 -h / -v 两个，长选项统一 --key [value] 或 --key=value
        if (arg == "-h" || arg == "-v") {
            args->flags.push_back(arg == "-h" ? "--help" : "--version");
            continue;
        }
        if (arg.size() > 2 && arg.compare(0, 2, "--") == 0) {
            const std::size_t eq = arg.find('=');
            if (eq != std::string::npos) {
                args->options[arg.substr(0, eq)] = arg.substr(eq + 1);
            } else if (needsValue(arg)) {
                if (i + 1 >= argc) {
                    *error = arg + " 需要一个值";
                    return false;
                }
                args->options[arg] = argv[++i];
            } else {
                args->flags.push_back(arg);
            }
            continue;
        }
        if (args->command.empty()) {
            args->command = arg;
        } else {
            args->positional.push_back(arg);
        }
    }
    return true;
}

std::string layerLabel(Layer layer) {
    for (const auto& [id, label] : kLayerLabels) {
        if (id == layer) {
            return label;
        }
    }
    return "未知";
}

// ---------------------------------------------------------------- 子命令

int cmdValidate(const Repository& repo, const Args& args) {
    ValidateOptions options;
    options.strict = args.has("--strict");
    options.platform = args.get("--platform");
    const std::vector<Problem> problems = validate(repo, options);

    std::size_t errors = 0;
    std::size_t warnings = 0;
    for (const Problem& p : problems) {
        (p.isError() ? errors : warnings)++;
    }
    if (args.has("--json")) {
        json::Value out = json::Value::object();
        out.set("root", repo.root);
        out.set("plugins", static_cast<long long>(repo.plugins.size()));
        out.set("sets", static_cast<long long>(repo.catalog.sets.size()));
        out.set("features", static_cast<long long>(repo.catalog.featureCount()));
        out.set("errors", static_cast<long long>(errors));
        out.set("warnings", static_cast<long long>(warnings));
        json::Value list = json::Value::array();
        for (const Problem& p : problems) {
            json::Value item = json::Value::object();
            item.set("severity", p.isError() ? "error" : "warning");
            item.set("code", p.code);
            item.set("message", p.message);
            list.push(item);
        }
        out.set("problems", list);
        std::cout << out.dump(2) << "\n";
    } else {
        for (const Problem& p : problems) {
            std::cout << p.format() << "\n";
        }
        std::cout << "\n" << repo.plugins.size() << " 个组件 / " << repo.catalog.sets.size() << " 个功能集 / "
                  << repo.catalog.featureCount() << " 条功能：" << errors << " 个错误，" << warnings
                  << " 个警告";
        if (!options.strict) {
            std::cout << "（非严格模式，W002 已省略；CI 使用 --strict）";
        }
        std::cout << "\n";
    }
    return errors == 0 ? 0 : 1;
}

int cmdResolve(const Repository& repo, const Args& args) {
    if (args.positional.empty()) {
        return fail("resolve 需要至少一个插件 id");
    }
    const Resolution result = resolve(repo, args.positional);
    if (!result.ok()) {
        for (const std::string& e : result.errors) {
            std::cerr << "错误: " << e << "\n";
        }
        return 1;
    }
    if (args.has("--json")) {
        json::Value out = json::Value::object();
        json::Value steps = json::Value::array();
        for (const InstallStep& step : result.steps) {
            json::Value item = json::Value::object();
            item.set("id", step.id);
            item.set("layer", step.layer);
            item.set("version", step.version);
            item.set("origin", step.origin);
            steps.push(item);
        }
        out.set("steps", steps);
        json::Value notes = json::Value::array();
        for (const std::string& note : result.notes) {
            notes.push(note);
        }
        out.set("notes", notes);
        std::cout << out.dump(2) << "\n";
        return 0;
    }
    std::vector<std::vector<std::string>> rows;
    int index = 1;
    for (const InstallStep& step : result.steps) {
        const Plugin* p = repo.find(step.id);
        rows.push_back({std::to_string(index++), step.id, layerLabel(p->layer), step.version, step.origin});
    }
    std::cout << text::renderTable({"#", "插件", "层级", "版本", "来源"}, rows);
    std::size_t packs = 0;
    for (const InstallStep& step : result.steps) {
        const Plugin* p = repo.find(step.id);
        if (p != nullptr && p->isPack()) {
            ++packs;
        }
    }
    std::cout << "\n共 " << result.steps.size() << " 个组件（" << packs << " 个扩展包，"
              << result.steps.size() - packs << " 个插件）\n";
    for (const std::string& note : result.notes) {
        std::cout << "\n提示：" << note << "\n";
    }
    return 0;
}

int cmdInfo(const Repository& repo, const Args& args) {
    if (args.positional.empty()) {
        return fail("info 需要插件 id");
    }
    const Plugin* p = repo.find(args.positional[0]);
    if (p == nullptr) {
        return fail("未知插件: " + args.positional[0]);
    }
    if (args.has("--json")) {
        json::Value out = json::Value::object();
        out.set("id", p->id);
        out.set("name", p->name);
        out.set("nameZh", p->nameZh);
        out.set("version", p->version);
        out.set("layer", layerId(p->layer));
        out.set("kind", p->isPack() ? "pack" : "plugin");
        out.set("description", p->description);
        json::Value deps = json::Value::object();
        for (const auto& [id, constraint] : p->dependencies) {
            deps.set(id, constraint);
        }
        out.set("dependencies", deps);
        json::Value sets = json::Value::array();
        for (const std::string& s : p->capabilitySets) {
            sets.push(s);
        }
        out.set("capabilitySets", sets);
        out.set("contributes", p->contributes);
        std::cout << out.dump(2) << "\n";
        return 0;
    }

    std::cout << p->id << "（" << p->nameZh << "）  " << p->version << "  [" << layerId(p->layer)
              << " / " << (p->isPack() ? "pack" : "plugin") << "]\n";
    std::cout << p->description << "\n\n";
    std::cout << "依赖: ";
    if (p->dependencies.empty()) {
        std::cout << "无";
    }
    for (std::size_t i = 0; i < p->dependencies.size(); ++i) {
        std::cout << (i == 0 ? "" : ", ") << p->dependencies[i].first << " " << p->dependencies[i].second;
    }
    std::cout << "\n推荐: ";
    if (p->recommends.empty()) {
        std::cout << "无";
    }
    {
        bool first = true;
        for (const auto& [id, rec] : p->recommends) {
            std::cout << (first ? "" : ", ") << id << " " << rec.version
                      << (rec.autoInstall ? "（自动安装）" : "");
            first = false;
        }
    }
    std::cout << "\n互斥组: ";
    {
        const json::Value& lsp = p->contributes["lsp"];
        std::cout << (lsp.isObject() ? lsp["exclusiveGroup"].asString() : std::string("无"));
    }
    std::cout << "\n\n功能集:\n";
    for (const std::string& sid : p->capabilitySets) {
        const CatalogSet* set = repo.catalog.findSet(sid);
        if (set == nullptr) {
            std::cout << "  - " << sid << "（未在 catalog 中定义）\n";
            continue;
        }
        std::cout << "  - " << sid << "（" << set->label << "）\n";
        for (const json::Value& f : set->features.elements()) {
            const std::vector<std::string> owners = featureOwners(*set, f);
            const bool mine = std::find(owners.begin(), owners.end(), p->id) != owners.end();
            std::cout << "      " << (mine ? "[x] " : "[ ] ") << f["id"].asString() << "  "
                      << f["label"].asString() << "\n";
        }
    }
    if (!p->contributes.isObject() || p->contributes.members().empty()) {
        std::cout << "\n贡献点: 无\n";
    } else {
        std::cout << "\n贡献点:\n";
        for (const auto& [key, value] : p->contributes.members()) {
            std::cout << "  - " << key << ": " << value.dump(0) << "\n";
        }
    }
    return 0;
}

int cmdGraph(const Repository& repo, const Args& args) {
    const std::string layerFilter = args.get("--layer");
    Layer wanted = Layer::Tooling;
    const bool filterLayer = !layerFilter.empty();
    if (filterLayer && !parseLayer(layerFilter, &wanted)) {
        return fail("未知层级: " + layerFilter);
    }
    std::vector<const Plugin*> selected;
    for (const Plugin& p : repo.plugins) {
        if (!filterLayer || p.layer == wanted) {
            selected.push_back(&p);
        }
    }
    std::set<std::string> ids;
    for (const Plugin* p : selected) {
        ids.insert(p->id);
    }
    std::stable_sort(selected.begin(), selected.end(), [](const Plugin* a, const Plugin* b) {
        return layerOrder(a->layer) < layerOrder(b->layer);
    });
    const std::string format = args.get("--format", "text");

    if (format == "dot") {
        std::cout << "digraph lightcpp {\n  rankdir=LR; node [shape=box];\n";
        for (const Plugin* p : selected) {
            std::cout << "  \"" << p->id << "\";\n";
        }
        for (const Plugin* p : selected) {
            for (const auto& [dep, constraint] : p->dependencies) {
                (void)constraint;
                if (ids.count(dep) != 0) {
                    std::cout << "  \"" << p->id << "\" -> \"" << dep << "\";\n";
                }
            }
        }
        std::cout << "}\n";
        return 0;
    }
    if (format == "mermaid") {
        std::cout << "```mermaid\ngraph LR\n";
        for (const auto& [layer, label] : kLayerLabels) {
            std::vector<const Plugin*> members;
            for (const Plugin* p : selected) {
                if (p->layer == layer) {
                    members.push_back(p);
                }
            }
            if (members.empty()) {
                continue;
            }
            std::cout << "  subgraph " << layerId(layer) << "[\"" << label << "\"]\n";
            for (const Plugin* p : members) {
                std::cout << "    " << p->id << "[\"" << p->nameZh << "\"]\n";
            }
            std::cout << "  end\n";
        }
        for (const Plugin* p : selected) {
            for (const auto& [dep, constraint] : p->dependencies) {
                (void)constraint;
                if (ids.count(dep) != 0) {
                    std::cout << "  " << p->id << " --> " << dep << "\n";
                }
            }
        }
        std::cout << "```\n";
        return 0;
    }

    for (const auto& [layer, label] : kLayerLabels) {
        std::vector<std::vector<std::string>> rows;
        for (const Plugin* p : selected) {
            if (p->layer != layer) {
                continue;
            }
            std::string deps;
            for (const auto& [dep, constraint] : p->dependencies) {
                (void)constraint;
                deps += (deps.empty() ? "" : ", ") + dep;
            }
            rows.push_back({p->id, p->nameZh, deps.empty() ? "—" : deps});
        }
        if (rows.empty()) {
            continue;
        }
        std::cout << label << "\n" << text::renderTable({"插件", "名称", "依赖"}, rows) << "\n";
    }
    return 0;
}

int cmdMatrix(const Repository& repo, const Args& args) {
    std::vector<std::vector<std::string>> rows;
    for (const auto& entry : kLayerLabels) {
        for (const Plugin* p : repo.byLayer(entry.first)) {
            std::string platforms;
            for (const std::string& plat : p->platforms) {
                platforms += (platforms.empty() ? "" : ",") + plat;
            }
            rows.push_back({p->id, p->nameZh, std::to_string(p->dependencies.size()),
                            std::to_string(p->capabilitySets.size()), platforms.empty() ? "all" : platforms});
        }
    }
    if (args.has("--json")) {
        json::Value out = json::Value::array();
        for (const auto& row : rows) {
            json::Value item = json::Value::object();
            item.set("id", row[0]);
            item.set("name", row[1]);
            item.set("deps", row[2]);
            item.set("sets", row[3]);
            item.set("platforms", row[4]);
            out.push(item);
        }
        std::cout << out.dump(2) << "\n";
        return 0;
    }
    std::cout << text::renderTable({"插件", "名称", "依赖数", "功能集", "平台"}, rows);
    return 0;
}

int cmdCoverage(const Repository& repo) {
    struct Stats {
        int required = 0;
        int planned = 0;
        int knownIssue = 0;
        int total() const { return required + planned + knownIssue; }
    };
    std::map<std::string, Stats> stats;
    std::map<std::string, std::string> labels;
    std::vector<std::string> order;
    for (const json::Value& domain : repo.catalog.root["domains"].elements()) {
        if (domain.isObject() && domain["id"].isString()) {
            labels[domain["id"].asString()] = domain["label"].asString();
            order.push_back(domain["id"].asString());
        }
    }
    for (const CatalogSet& set : repo.catalog.sets) {
        for (const json::Value& f : set.features.elements()) {
            const std::string status = f["status"].isString() ? f["status"].asString() : "required";
            Stats& entry = stats[set.domain];
            if (status == "planned") {
                ++entry.planned;
            } else if (status == "known-issue") {
                ++entry.knownIssue;
            } else {
                ++entry.required;
            }
        }
    }
    std::vector<std::vector<std::string>> rows;
    for (const std::string& domain : order) {
        const Stats& s = stats[domain];
        rows.push_back({labels[domain], std::to_string(s.total()), std::to_string(s.required),
                        std::to_string(s.planned), std::to_string(s.knownIssue)});
    }
    std::cout << text::renderTable({"功能域", "合计", "已实现目标", "规划中", "已知问题"}, rows);
    std::cout << "\n合计 " << repo.catalog.featureCount()
              << " 条功能。规划中 / 已知问题项不计入 v0.2 验收范围；"
              << "其余条目的「已覆盖」指有归属插件（清单声明），不代表引擎已实现。\n";
    return 0;
}

int cmdEngines(const Repository& repo) {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> groups;
    for (const Plugin& p : repo.plugins) {
        const json::Value& lsp = p.contributes["lsp"];
        if (lsp.isObject() && !lsp["exclusiveGroup"].asString().empty()) {
            groups[lsp["exclusiveGroup"].asString()].push_back(
                {p.id, lsp["engine"].isString() ? lsp["engine"].asString() : "?"});
        }
    }
    if (groups.empty()) {
        std::cout << "没有声明互斥引擎组\n";
        return 0;
    }
    for (const auto& [group, members] : groups) {
        std::cout << "互斥组 " << group << ":\n";
        for (const auto& [id, engine] : members) {
            std::cout << "  - " << id << " -> " << engine << "\n";
        }
        std::cout << "  同一项目只启用其中一个，另一个保持安装但处于停用状态。\n";
    }
    return 0;
}

int cmdDocs(const Repository& repo, const Args& args) {
    const std::string outDir = args.get("--out");
    const std::string shownDir = outDir.empty() ? std::string("docs") : outDir;
    if (args.has("--check")) {
        std::string hint;
        if (!docs::upToDate(repo, outDir, &hint)) {
            std::cerr << "文档与清单不一致：" << hint << "\n";
            return 1;
        }
        std::cout << "文档与清单一致。\n";
        return 0;
    }
    std::string error;
    if (!docs::write(repo, outDir, &error)) {
        return fail(error);
    }
    std::cout << "已生成 " << shownDir << "/PLUGINS.md 与 " << shownDir << "/FEATURES.md\n";
    return 0;
}

int cmdHost(const Repository& repo, const Args& args) {
    HostOptions options;
    options.pluginsDirectory = fs::join(repo.root, "plugins");
    // 默认构建目录由 CMake 注入（LIGHTC_BUILD_LIB_DIR），独立构建与作为
    // 根项目子目录构建都能找到自己的产物；--lib-dir 可覆盖。
    const std::string libDir = args.get("--lib-dir");
#ifdef LIGHTC_BUILD_LIB_DIR
    const std::string defaultLibDir = LIGHTC_BUILD_LIB_DIR;
#else
    const std::string defaultLibDir = fs::join(fs::join(repo.root, "core/build/bin"), "plugins");
#endif
    options.buildOutputDirectory = libDir.empty() ? defaultLibDir : libDir;
    options.requireNative = args.has("--require-native");
    const std::string only = args.get("--only");
    if (!only.empty()) {
        std::string current;
        for (char c : only) {
            if (c == ',') {
                if (!current.empty()) {
                    options.only.push_back(current);
                }
                current.clear();
            } else if (c != ' ') {
                current.push_back(c);
            }
        }
        if (!current.empty()) {
            options.only.push_back(current);
        }
    }
    options.sink = [](Level level, const char* target, const char* message, void* user) {
        (void)user;
        const char* label = level == Level::Error ? "error"
                            : level == Level::Warn ? "warn"
                            : level == Level::Info ? "info"
                                                   : "debug";
        std::cout << "[" << label << "] " << target << ": " << message << "\n";
    };
    Host host(options);
    const bool ok = host.load(repo);

    std::vector<std::vector<std::string>> rows;
    for (const PluginState& state : host.states()) {
        std::string status = state.activated ? "已激活" : (state.skippedReasons.empty() ? "未激活" : state.skippedReasons.front());
        rows.push_back({state.id, state.version, status,
                        std::to_string(state.contributions.size())});
    }
    std::cout << text::renderTable({"插件", "版本", "状态", "注册贡献点"}, rows);

    for (const auto& type : {"buildSystems", "debuggers", "lsp", "testFramework"}) {
        const std::vector<std::string> ids = host.contributionIds(type);
        if (ids.empty()) {
            continue;
        }
        std::cout << "\n" << type << ": ";
        for (std::size_t i = 0; i < ids.size(); ++i) {
            std::cout << (i == 0 ? "" : ", ") << ids[i];
        }
        std::cout << "\n";
    }
    for (const std::string& warning : host.warnings()) {
        std::cout << "\n提示：" << warning << "\n";
    }
    for (const std::string& error : host.errors()) {
        std::cerr << "错误: " << error << "\n";
    }
    std::vector<std::string> missing;
    for (const PluginState& state : host.states()) {
        if (!state.activated) {
            missing.push_back(state.id);
        }
    }
    if (!missing.empty()) {
        std::cout << "\n未激活（" << missing.size() << "）：";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            std::cout << (i == 0 ? "" : ", ") << missing[i];
        }
        std::cout << "\n";
    }
    std::cout << "\n已激活 " << host.activatedCount() << " / " << host.states().size() << " 个插件。"
              << (options.requireNative ? "（严格：缺少原生实现即失败）" : "（清单级自检模式）") << "\n";
    return ok ? 0 : 1;
}

int cmdList(const Repository& repo, const Args& args) {
    const std::string layerFilter = args.get("--layer");
    Layer wanted = Layer::Tooling;
    const bool filterLayer = !layerFilter.empty();
    if (filterLayer && !parseLayer(layerFilter, &wanted)) {
        return fail("未知层级: " + layerFilter);
    }
    std::vector<std::vector<std::string>> rows;
    for (const Plugin& p : repo.plugins) {
        if (filterLayer && p.layer != wanted) {
            continue;
        }
        rows.push_back({p.id, p.nameZh, layerId(p.layer), p.version});
    }
    std::cout << text::renderTable({"插件", "名称", "层级", "版本"}, rows);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    std::string parseError;
    if (!parseArgs(argc, argv, &args, &parseError)) {
        return fail(parseError);
    }
    // --version 优先于 --help：脚本里常把两者一起写，此时应拿到版本号而不是帮助
    if (args.has("--version") || args.command == "version") {
        std::cout << "lightcpp-pm " << kVersion << "\n";
        return 0;
    }
    if (args.has("--help") || args.command.empty() || args.command == "help") {
        printUsage();
        return args.command.empty() && !args.has("--help") ? 1 : 0;
    }
    const std::string root = args.get("--root", ".");
    const Repository repo = Repository::load(root);
    if (!fs::isDirectory(fs::join(repo.root, "plugins"))) {
        return fail("找不到插件目录：从 " + fs::absolute(root) +
                    " 向上未找到含 plugins/ 与 catalog/ 的仓库根");
    }

    const std::string& command = args.command;
    if (command == "validate") return cmdValidate(repo, args);
    if (command == "resolve") return cmdResolve(repo, args);
    if (command == "info") return cmdInfo(repo, args);
    if (command == "graph") return cmdGraph(repo, args);
    if (command == "matrix") return cmdMatrix(repo, args);
    if (command == "coverage") return cmdCoverage(repo);
    if (command == "engines") return cmdEngines(repo);
    if (command == "docs") return cmdDocs(repo, args);
    if (command == "host") return cmdHost(repo, args);
    if (command == "list") return cmdList(repo, args);

    std::cerr << "未知命令: " << command << "\n\n";
    printUsage();
    return 2;
}
