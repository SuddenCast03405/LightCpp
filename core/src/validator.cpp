#include "lightcpp/validator.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "lightcpp/version.h"

namespace lightcpp {
namespace {

const std::vector<std::string> kRequiredFields = {"id",     "name",    "nameZh", "version",
                                                  "publisher", "kind", "layer", "description"};
const std::vector<std::string> kOptionalFields = {"platforms",   "engines",       "dependencies",
                                                  "recommends",  "capabilitySets", "activation",
                                                  "contributes", "tags",          "entry",
                                                  "apiVersion",  "permissions"};
const std::vector<std::string> kPlatforms = {"linux", "darwin", "win32"};
const std::vector<std::string> kStatuses = {"required", "planned", "known-issue"};

bool isBlank(const std::string& s) {
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool validId(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (i == 0 && !(lower || digit)) {
            return false;
        }
        if (!(lower || digit || c == '-')) {
            return false;
        }
    }
    return true;
}

/// UTF-8 字符数（描述长度按字符而不是字节计）。
std::size_t utf8Length(const std::string& s) {
    std::size_t count = 0;
    for (char raw : s) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += items[i];
    }
    return out;
}

struct Collector {
    std::vector<Problem>* out;
    void error(const std::string& code, const std::string& msg) {
        out->push_back({Severity::Error, code, msg});
    }
    void warn(const std::string& code, const std::string& msg) {
        out->push_back({Severity::Warning, code, msg});
    }
};

std::string quote(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

std::string Problem::format() const {
    return std::string(isError() ? "error  " : "warning") + "  [" + code + "] " + message;
}

std::vector<std::string> featureOwners(const CatalogSet& set, const json::Value& feature) {
    std::vector<std::string> owners;
    const json::Value& own = feature["plugins"];
    if (own.isArray()) {
        for (const json::Value& item : own.elements()) {
            if (item.isString()) {
                owners.push_back(item.asString());
            }
        }
    } else {
        owners = set.plugins;
    }
    std::set<std::string> blocked;
    for (const json::Value& item : feature["except"].elements()) {
        if (item.isString()) {
            blocked.insert(item.asString());
        }
    }
    std::vector<std::string> result;
    for (const std::string& owner : owners) {
        if (blocked.count(owner) == 0) {
            result.push_back(owner);
        }
    }
    return result;
}

std::vector<std::vector<std::string>> findCycles(const Repository& repo) {
    std::map<std::string, int> color;  // 0 未访问 1 在当前栈上 2 完成
    std::vector<std::string> stack;
    std::vector<std::vector<std::string>> cycles;

    // 递归深度受依赖层数限制（插件生态不超过 10 层），此处无需显式栈。
    std::function<void(const std::string&)> visit = [&](const std::string& node) {
        color[node] = 1;
        stack.push_back(node);
        const Plugin* p = repo.find(node);
        if (p != nullptr) {
            for (const auto& [dep, constraint] : p->dependencies) {
                (void)constraint;
                if (repo.find(dep) == nullptr) {
                    continue;
                }
                const int state = color.count(dep) != 0 ? color[dep] : 0;
                if (state == 1) {
                    auto it = std::find(stack.begin(), stack.end(), dep);
                    std::vector<std::string> cycle(it, stack.end());
                    cycle.push_back(dep);
                    cycles.push_back(cycle);
                } else if (state == 0) {
                    visit(dep);
                }
            }
        }
        stack.pop_back();
        color[node] = 2;
    };

    for (const Plugin& p : repo.plugins) {
        if (color[p.id] == 0) {
            visit(p.id);
        }
    }
    return cycles;
}

std::vector<Problem> checkInvariants(const Repository& repo) {
    std::vector<Problem> problems;

    for (const Invariant& rule : repo.invariants) {
        const json::Value& body = rule.body;
        const std::string rid = rule.id.empty() ? "<无 id>" : rule.id;
        auto log = [&](Severity sev, const std::string& code, const std::string& msg) {
            problems.push_back({sev, code, "[" + rid + "] " + msg});
        };

        if (rule.type == "dependency") {
            const std::string plugin = body["plugin"].asString();
            const Plugin* p = repo.find(plugin);
            if (p == nullptr) {
                log(Severity::Error, "E100", "不变量引用的插件不存在：" + plugin);
                continue;
            }
            for (const json::Value& target : body["on"].elements()) {
                if (target.isString() && p->dependencyVersion(target.asString()).empty()) {
                    log(Severity::Error, "E100", plugin + " 必须依赖 " + target.asString());
                }
            }
        } else if (rule.type == "autoInstall") {
            const std::string plugin = body["plugin"].asString();
            const std::string target = body["target"].asString();
            const Plugin* p = repo.find(plugin);
            if (p == nullptr) {
                log(Severity::Error, "E101", "不变量引用的插件不存在：" + plugin);
                continue;
            }
            auto it = p->recommends.find(target);
            if (it == p->recommends.end() || !it->second.autoInstall) {
                log(Severity::Error, "E101", plugin + " 必须自动连带安装 " + target);
            }
        } else if (rule.type == "packMembers") {
            const std::string packId = body["pack"].asString();
            const Plugin* p = repo.find(packId);
            if (p == nullptr) {
                log(Severity::Error, "E102", "不变量引用的扩展包不存在：" + packId);
                continue;
            }
            std::set<std::string> actual;
            for (const auto& [dep, constraint] : p->dependencies) {
                (void)constraint;
                actual.insert(dep);
            }
            std::set<std::string> expected;
            for (const json::Value& item : body["members"].elements()) {
                if (item.isString()) {
                    expected.insert(item.asString());
                }
            }
            if (actual != expected) {
                std::vector<std::string> missing;
                std::vector<std::string> extra;
                for (const std::string& id : expected) {
                    if (actual.count(id) == 0) {
                        missing.push_back(id);
                    }
                }
                for (const std::string& id : actual) {
                    if (expected.count(id) == 0) {
                        extra.push_back(id);
                    }
                }
                log(Severity::Error, "E102",
                    packId + " 成员不符：缺少 [" + joinList(missing) + "]，多余 [" + joinList(extra) + "]");
            }
        } else if (rule.type == "layerDependency") {
            const std::string layerIdText = body["layer"].asString();
            Layer layer = Layer::Tooling;
            if (!parseLayer(layerIdText, &layer)) {
                log(Severity::Error, "E105", "不变量中的层级非法：" + layerIdText);
                continue;
            }
            for (const Plugin* p : repo.byLayer(layer)) {
                for (const json::Value& target : body["on"].elements()) {
                    if (target.isString() && p->dependencyVersion(target.asString()).empty()) {
                        log(Severity::Error, "E103", p->id + " 必须依赖 " + target.asString());
                    }
                }
            }
        } else if (rule.type == "exclusiveGroup") {
            const std::string group = body["group"].asString();
            std::set<std::string> members;
            for (const Plugin& p : repo.plugins) {
                const json::Value& lsp = p.contributes["lsp"];
                if (lsp.isObject() && lsp["exclusiveGroup"].asString() == group) {
                    members.insert(p.id);
                }
            }
            std::set<std::string> expected;
            for (const json::Value& item : body["members"].elements()) {
                if (item.isString()) {
                    expected.insert(item.asString());
                }
            }
            if (members != expected) {
                std::vector<std::string> diff(members.begin(), members.end());
                log(Severity::Error, "E104", "互斥组 " + group + " 成员不符: [" + joinList(diff) + "]");
            }
        } else {
            log(Severity::Error, "E105", "未知的不变量类型 " + rule.type);
        }
    }
    return problems;
}

std::vector<Problem> validate(const Repository& repo, const ValidateOptions& options) {
    std::vector<Problem> problems;
    Collector c{&problems};

    for (const std::string& message : repo.loadErrors) {
        problems.push_back({Severity::Error, "LOAD", message});
    }

    // ---- 单插件字段与依赖
    for (const Plugin& p : repo.plugins) {
        const std::string& where = p.directory;
        auto missing = [](const std::string& value) { return isBlank(value); };
        if (missing(p.id)) c.error("E001", where + ": 缺少必填字段 id");
        if (missing(p.name)) c.error("E001", where + ": 缺少必填字段 name");
        if (missing(p.nameZh)) c.error("E001", where + ": 缺少必填字段 nameZh");
        if (missing(p.version)) c.error("E001", where + ": 缺少必填字段 version");
        if (missing(p.publisher)) c.error("E001", where + ": 缺少必填字段 publisher");
        if (missing(p.description)) c.error("E001", where + ": 缺少必填字段 description");
        if (p.kind == Kind::Pack && p.layer != Layer::Pack) {
            c.error("E006", where + ": 扩展包的 layer 必须为 pack");
        }
        if (p.kind == Kind::Plugin && p.layer == Layer::Pack) {
            c.error("E007", where + ": 非扩展包不能使用 pack 层级");
        }
        if (p.id != where) {
            c.error("E002", where + ": id (" + p.id + ") 与目录名不一致");
        }
        if (!validId(p.id)) {
            c.error("E003", where + ": id 必须匹配 ^[a-z0-9][a-z0-9-]*$");
        }
        if (p.layer == Layer::Core && p.id != "cpp-base") {
            c.error("E028", where + ": core 层只允许 [cpp-base]");
        }
        if (p.layer == Layer::BuildFramework && p.id != "build-core") {
            c.error("E028", where + ": build-framework 层只允许 [build-core]");
        }
        Version version;
        if (!p.version.empty() && !Version::parse(p.version, &version)) {
            c.error("E008", where + ": version 需为 x.y.z 且每段不超过 65535");
        }
        for (const std::string& plat : p.platforms) {
            if (std::find(kPlatforms.begin(), kPlatforms.end(), plat) == kPlatforms.end()) {
                c.error("E009", where + ": 未知平台 " + plat);
            }
        }
        if (!p.description.empty() && utf8Length(p.description) < 8) {
            c.error("E027", where + ": description 至少 8 个字符");
        }

        for (const auto& [dep, constraint] : p.dependencies) {
            const Plugin* target = repo.find(dep);
            if (target == nullptr) {
                c.error("E010", where + ": 依赖的插件不存在: " + dep);
                continue;
            }
            std::string reason;
            if (!satisfies(target->version, constraint, &reason)) {
                if (reason.empty()) {
                    c.error("E011", where + ": 依赖 " + dep + " 的版本 " + target->version +
                                        " 不满足约束 " + constraint);
                } else {
                    c.error("E012", where + ": 依赖 " + dep + " 的版本约束非法 (" + reason + ")");
                }
            }
            if (target->isPack()) {
                c.error("E013", where + ": 不能把扩展包 " + dep + " 声明为依赖");
            }
        }

        for (const auto& [dep, rec] : p.recommends) {
            if (repo.find(dep) == nullptr) {
                c.error("E014", where + ": 推荐的插件不存在: " + dep);
                continue;
            }
            if (rec.autoInstall && isBlank(rec.reason)) {
                c.error("E015", where + ": autoInstall 推荐 " + dep + " 必须说明 reason");
            }
            if (!p.dependencyVersion(dep).empty()) {
                c.warn("W001", where + ": " + dep + " 同时出现在 dependencies 与 recommends 中");
            }
        }

        for (const std::string& sid : p.capabilitySets) {
            if (repo.catalog.findSet(sid) == nullptr) {
                c.error("E016", where + ": 未知功能集 " + sid);
            }
        }

        if (p.isPack()) {
            if (p.contributes.isObject() && !p.contributes.members().empty()) {
                c.error("E017", where + ": 扩展包不得声明 contributes（不含业务逻辑）");
            }
            if (p.dependencies.empty()) {
                c.error("E018", where + ": 扩展包必须至少包含一个成员");
            }
            if (!p.capabilitySets.empty()) {
                c.error("E019", where + ": 扩展包不得声明 capabilitySets");
            }
        }
    }

    for (const Plugin& p : repo.plugins) {
        const json::Value& contributes = p.contributes;
        for (const auto& [key, value] : contributes.members()) {
            (void)value;
            static const std::map<std::string, bool> kShapes = {  // true = 数组
                {"commands", true},        {"languages", true},     {"fileAssociations", true},
                {"buildSystems", true},    {"debuggers", true},     {"toolchain", true},
                {"compilers", true},       {"settings", true},      {"knownIssues", true},
                {"limitations", true},     {"packageManager", false}, {"testFramework", false},
                {"testExplorer", false},   {"buildTaskEngine", false}, {"debugSupport", false},
                {"scm", false},            {"lsp", false},         {"toolchainProviders", true},
            };
            auto shape = kShapes.find(key);
            if (shape == kShapes.end()) {
                c.warn("W004", p.directory + ": 未知贡献点 contributes." + key +
                                    "（宿主会忽略；如需新增请在宿主注册表登记）");
                continue;
            }
            const bool wantArray = shape->second;
            // limitations 允许字符串或数组：规划原文就是一句话说明。
            const bool allowString = key == "limitations";
            const bool shapeOk = wantArray ? (value.isArray() || (allowString && value.isString()))
                                           : value.isObject();
            if (!shapeOk) {
                c.error("E008", p.directory + ": contributes." + key + " 必须是" +
                                    (wantArray ? "数组" : "对象"));
            }
        }
    }

    for (const std::vector<std::string>& cycle : findCycles(repo)) {
        c.error("E020", "存在依赖环: " + joinList(cycle));
    }

    // ---- 功能目录
    std::set<std::string> usedSets;
    for (const Plugin& p : repo.plugins) {
        usedSets.insert(p.capabilitySets.begin(), p.capabilitySets.end());
    }
    for (const CatalogSet& set : repo.catalog.sets) {
        if (usedSets.count(set.id) == 0) {
            c.error("E021", "功能集 " + set.id + " 没有任何插件声明");
        }
    }

    std::map<std::string, int> setCount;
    for (const CatalogSet& set : repo.catalog.sets) {
        ++setCount[set.id];
    }
    for (const auto& [id, count] : setCount) {
        if (count > 1) {
            c.error("E029", "功能集 id " + id + " 重复出现 " + std::to_string(count) + " 次（复制粘贴？）");
        }
    }

    std::map<std::string, int> featureCount;
    for (const CatalogSet& set : repo.catalog.sets) {
        if (!repo.catalog.hasDomain(set.domain)) {
            c.error("E030", "功能集 " + set.id + ": 未知功能域 " + quote(set.domain));
        }
        if (set.features.empty()) {
            c.error("E031", "功能集 " + set.id + " 没有任何功能");
        }
        for (const json::Value& f : set.features.elements()) {
            if (!f.isObject()) {
                c.error("E031", "功能集 " + set.id + " 中存在非对象的功能条目");
                continue;
            }
            const std::string fid = f["id"].asString();
            if (fid.empty()) {
                c.error("E031", "功能集 " + set.id + " 中有功能缺少 id");
                continue;
            }
            ++featureCount[fid];
            const std::string status = f["status"].isString() ? f["status"].asString() : "required";
            auto it = std::find(kStatuses.begin(), kStatuses.end(), status);
            if (it == kStatuses.end()) {
                c.error("E031", "功能 " + fid + " 的 status 非法：" + status);
            }
            for (const json::Value& name : f["except"].elements()) {
                if (name.isString() && repo.find(name.asString()) == nullptr) {
                    c.error("E032", "功能 " + fid + ": except 指向不存在的插件 " + name.asString());
                }
            }
            const std::vector<std::string> owners = featureOwners(set, f);
            for (const std::string& owner : owners) {
                if (repo.find(owner) == nullptr) {
                    c.error("E022", "功能 " + fid + " 归属的插件不存在: " + owner);
                }
            }
            if (status == "required" && owners.empty()) {
                c.error("E023", "必需功能 " + fid + " 没有归属插件");
            }
            if (status != "required" && isBlank(f["note"].asString())) {
                c.warn("W002", "功能 " + fid + " 状态为 " + status + " 但缺少 note 说明");
            }
        }
    }
    for (const auto& [fid, count] : featureCount) {
        if (count > 1) {
            c.error("E029", "功能 id " + fid + " 重复出现 " + std::to_string(count) + " 次（复制粘贴？）");
        }
    }

    // ---- LSP 互斥组
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> groups;
    for (const Plugin& p : repo.plugins) {
        const json::Value& lsp = p.contributes["lsp"];
        if (!lsp.isObject()) {
            continue;
        }
        const std::string group = lsp["exclusiveGroup"].asString();
        if (!group.empty()) {
            const std::string engine = lsp["engine"].isString() ? lsp["engine"].asString() : "?";
            groups[group].emplace_back(p.id, engine);
        }
    }
    for (const auto& [group, members] : groups) {
        if (members.size() < 2) {
            c.error("E024", "互斥组 " + group + " 只有 " + std::to_string(members.size()) + " 个成员");
        }
        std::set<std::string> engines;
        for (const auto& [id, engine] : members) {
            if (!engines.insert(engine).second) {
                c.error("E025", "互斥组 " + group + " 内 engine 重名: " + engine);
            }
        }
    }

    // ---- 平台可见性：插件声明了平台，但当前目标平台不在其中，给出提示而非错误。
    const std::string platform = options.platform.empty() ? std::string(
#if defined(_WIN32)
        "win32"
#elif defined(__APPLE__)
        "darwin"
#else
        "linux"
#endif
        ) : options.platform;
    for (const Plugin& p : repo.plugins) {
        if (!p.platforms.empty() && !p.hasPlatform(platform)) {
            c.warn("W003", p.id + " 声明平台 [" + joinList(p.platforms) + "]，当前目标平台 " + platform +
                                " 不可用（不影响校验结果）");
        }
    }

    std::vector<Problem> invariants = checkInvariants(repo);
    problems.insert(problems.end(), invariants.begin(), invariants.end());

    if (!options.strict) {
        std::vector<Problem> filtered;
        for (const Problem& p : problems) {
            if (p.isError() || p.code != "W002") {
                filtered.push_back(p);
            }
        }
        problems.swap(filtered);
    }
    return problems;
}

}  // namespace lightcpp
