#include "lightcpp/host.h"

#include <algorithm>
#include <map>
#include <sstream>

#include "lightcpp/fs.h"
#include "lightcpp/plugin_api.h"
#include "lightcpp/resolver.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lightcpp {
namespace {

#if defined(_WIN32)
using LibraryHandle = HMODULE;

LibraryHandle openLibrary(const std::string& path, std::string* error) {
    LibraryHandle handle = LoadLibraryA(path.c_str());
    if (handle == nullptr) {
        *error = "LoadLibrary 失败（错误码 " + std::to_string(GetLastError()) + "）";
    }
    return handle;
}

void* lookupSymbol(LibraryHandle handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(handle, name));
}

void closeLibrary(LibraryHandle handle) { FreeLibrary(handle); }

const char* libraryExtension() { return ".dll"; }
#else
using LibraryHandle = void*;

LibraryHandle openLibrary(const std::string& path, std::string* error) {
#if defined(__APPLE__)
    LibraryHandle handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#else
    LibraryHandle handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (handle == nullptr) {
        const char* reason = dlerror();
        *error = reason == nullptr ? "dlopen 失败" : reason;
    }
    return handle;
}

void* lookupSymbol(LibraryHandle handle, const char* name) { return dlsym(handle, name); }

void closeLibrary(LibraryHandle handle) {
    if (handle != nullptr) {
        dlclose(handle);
    }
}

#if defined(__APPLE__)
const char* libraryExtension() { return ".dylib"; }
#else
const char* libraryExtension() { return ".so"; }
#endif
#endif

const char* levelName(int level) {
    switch (level) {
        case 0: return "debug";
        case 1: return "info";
        case 2: return "warn";
        default: return "error";
    }
}

}  // namespace

struct Host::Impl {
    struct Loaded {
        LibraryHandle handle = nullptr;
        const LightCppPluginInfo* info = nullptr;
    };

    HostOptions options;
    std::vector<Loaded> libraries;
    std::vector<ContributionRecord> registry;

    void log(Level level, const std::string& target, const std::string& message) const {
        if (options.sink != nullptr) {
            options.sink(level, target.c_str(), message.c_str(), options.user);
        }
    }
};

namespace {

int apiRegister(void* host, const char* pluginId, const char* type, const char* id) {
    if (host == nullptr || type == nullptr || id == nullptr) {
        return -1;
    }
    static_cast<Host*>(host)->registerContribution(pluginId == nullptr ? "" : pluginId, type, id);
    return 0;
}

void apiLog(void* host, int level, const char* message) {
    if (host == nullptr || message == nullptr) {
        return;
    }
    // 插件日志统一走宿主 sink，避免插件自己写 stdout 打乱 CLI 输出。
    static_cast<Host*>(host)->logFromPlugin(level, message);
}

}  // namespace

std::string Host::platformTag() {
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

std::string Host::libraryName(const std::string& id) {
    std::string name = id;
    std::replace(name.begin(), name.end(), '-', '_');
#if defined(_WIN32)
    return name + ".dll";
#else
    return "lib" + name + libraryExtension();
#endif
}

Host::Host(HostOptions options) : impl_(new Impl()) { impl_->options = std::move(options); }

void Host::logFromPlugin(int level, const std::string& message) {
    Level mapped = Level::Info;
    switch (level) {
        case 0: mapped = Level::Debug; break;
        case 1: mapped = Level::Info; break;
        case 2: mapped = Level::Warn; break;
        default: mapped = Level::Error; break;
    }
    if (impl_->options.sink != nullptr) {
        impl_->options.sink(mapped, "plugin", message.c_str(), impl_->options.user);
    }
    (void)levelName(level);
}

Host::~Host() {
    // 逆序卸载：后加载的插件可能依赖先加载的符号。
    for (auto it = impl_->libraries.rbegin(); it != impl_->libraries.rend(); ++it) {
        if (it->info != nullptr && it->info->deactivate != nullptr) {
            it->info->deactivate();
        }
        closeLibrary(it->handle);
    }
}

void Host::registerContribution(const std::string& pluginId, const std::string& type, const std::string& id) {
    if (type.empty() || id.empty()) {
        return;
    }
    impl_->registry.push_back({type, id, pluginId});
}

std::size_t Host::activatedCount() const {
    return static_cast<std::size_t>(std::count_if(states_.begin(), states_.end(),
                                                  [](const PluginState& s) { return s.activated; }));
}

std::vector<ContributionRecord> Host::contributions(const std::string& type) const {
    std::vector<ContributionRecord> out;
    for (const ContributionRecord& record : impl_->registry) {
        if (record.type == type) {
            out.push_back(record);
        }
    }
    return out;
}

std::vector<std::string> Host::contributionIds(const std::string& type) const {
    std::vector<std::string> out;
    for (const ContributionRecord& record : impl_->registry) {
        if (record.type == type) {
            out.push_back(record.id);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string Host::activeEngine(const std::string& group) const { return group; }

namespace {

/// 清单里哪些贡献点必须有原生实现。
const std::vector<std::string> kNativeContributions = {
    "buildSystems", "debuggers", "lsp", "testFramework", "packageManager", "testExplorer",
};

bool needsNative(const Plugin& p) {
    for (const std::string& key : kNativeContributions) {
        if (p.hasContribution(key)) {
            return true;
        }
    }
    return false;
}

/// 从 lsp 贡献点里取引擎名，用于运行时互斥选择。
std::string exclusiveGroupOf(const Plugin& p) {
    const json::Value& lsp = p.contributes["lsp"];
    return lsp.isObject() ? lsp["exclusiveGroup"].asString() : std::string();
}

}  // namespace

bool Host::load(const Repository& repo) {
    errors_.clear();
    warnings_.clear();
    states_.clear();
    impl_->registry.clear();

    // 1. 依赖顺序决定加载顺序：被依赖的先加载，卸载时逆序。
    std::vector<std::string> candidates;
    for (const Plugin& p : repo.plugins) {
        if (p.isPack()) {
            continue;  // 扩展包没有运行时
        }
        if (!impl_->options.only.empty() &&
            std::find(impl_->options.only.begin(), impl_->options.only.end(), p.id) == impl_->options.only.end()) {
            continue;
        }
        candidates.push_back(p.id);
    }
    std::string orderError;
    const std::vector<std::string> order = topologicalOrder(repo, candidates, &orderError);
    if (!orderError.empty()) {
        errors_.push_back(orderError);
        return false;
    }

    // 2. 逐插件加载。
    for (const std::string& id : order) {
        const Plugin* p = repo.find(id);
        if (p == nullptr) {
            continue;
        }
        PluginState state;
        state.id = p->id;
        state.version = p->version;

        const bool nativeRequired = needsNative(*p);
        // 实现库查找顺序：插件目录 -> 插件目录/lib -> 插件目录/bin -> 构建产物目录。
        // 内置插件编译在 core/build/bin/plugins，第三方插件放在自己的目录里，两种都支持。
        std::vector<std::string> libraryCandidates;
        const std::string pluginDir = fs::join(impl_->options.pluginsDirectory, p->directory);
        const std::string name = libraryName(p->id);
        libraryCandidates.push_back(fs::join(pluginDir, name));
        libraryCandidates.push_back(fs::join(fs::join(pluginDir, "lib"), name));
        libraryCandidates.push_back(fs::join(fs::join(pluginDir, "bin"), name));
        if (!impl_->options.buildOutputDirectory.empty()) {
            libraryCandidates.push_back(fs::join(impl_->options.buildOutputDirectory, name));
        }
        std::string libPath;
        for (const std::string& candidate : libraryCandidates) {
            if (fs::isRegularFile(candidate)) {
                libPath = candidate;
                break;
            }
        }
        state.libraryPath = libPath;
        const bool hasLibrary = !libPath.empty();
        if (!hasLibrary) {
            if (nativeRequired && impl_->options.requireNative) {
                errors_.push_back(p->id + " 声明了 " + "原生贡献点，但未找到实现库 " + libraryName(p->id));
                state.skippedReasons.push_back("缺少实现库");
            } else if (nativeRequired) {
                state.skippedReasons.push_back("缺少实现库（当前为清单级自检）");
            } else {
                state.skippedReasons.push_back("纯清单插件，无原生实现");
            }
            states_.push_back(std::move(state));
            continue;
        }

        std::string error;
        LibraryHandle handle = openLibrary(fs::absolute(libPath), &error);
        if (handle == nullptr) {
            errors_.push_back(p->id + " 加载失败：" + error);
            state.skippedReasons.push_back("dlopen 失败");
            states_.push_back(std::move(state));
            continue;
        }

        LightCppEntryFn entry =
            reinterpret_cast<LightCppEntryFn>(lookupSymbol(handle, LIGHTC_PLUGIN_SYMBOL));
        if (entry == nullptr) {
            errors_.push_back(p->id + " 缺少入口符号 " + LIGHTC_PLUGIN_SYMBOL);
            closeLibrary(handle);
            state.skippedReasons.push_back("缺少入口符号");
            states_.push_back(std::move(state));
            continue;
        }

        const LightCppPluginInfo* info = entry();
        if (info == nullptr || info->apiVersion != LIGHTC_API_VERSION) {
            errors_.push_back(p->id + " 的 ABI 版本不匹配（期望 " + std::to_string(LIGHTC_API_VERSION) +
                              "）");
            closeLibrary(handle);
            state.skippedReasons.push_back("ABI 不匹配");
            states_.push_back(std::move(state));
            continue;
        }
        if (info->id != nullptr && p->id != info->id) {
            errors_.push_back(p->id + " 的实现自称 " + info->id + "，与清单不一致");
            closeLibrary(handle);
            state.skippedReasons.push_back("实现与清单 id 不一致");
            states_.push_back(std::move(state));
            continue;
        }

        LightCppApi api{};
        api.apiVersion = LIGHTC_API_VERSION;
        api.host = this;
        api.registerContribution = &apiRegister;
        api.log = &apiLog;

        // 先清掉该插件可能残留的注册，保证重复 load 幂等。
        const std::size_t before = impl_->registry.size();
        const int rc = info->activate == nullptr ? 0 : info->activate(&api);
        if (rc != 0) {
            errors_.push_back(p->id + " 激活返回错误码 " + std::to_string(rc));
            if (info->deactivate != nullptr) {
                info->deactivate();
            }
            closeLibrary(handle);
            state.skippedReasons.push_back("activate 失败");
            states_.push_back(std::move(state));
            continue;
        }

        for (std::size_t i = before; i < impl_->registry.size(); ++i) {
            if (impl_->registry[i].provider.empty()) {
                impl_->registry[i].provider = p->id;
            }
            state.contributions.push_back(impl_->registry[i]);
        }
        state.nativeLoaded = true;
        state.activated = true;
        impl_->libraries.push_back({handle, info});
        states_.push_back(std::move(state));
    }

    // 3. 互斥组：同一项目只允许启用一个引擎，后出现的给告警而不是错误。
    std::map<std::string, std::string> groupEngine;
    for (const Plugin& p : repo.plugins) {
        const std::string group = exclusiveGroupOf(p);
        if (group.empty()) {
            continue;
        }
        // 只有该插件真的激活了才参与"谁被启用"的仲裁。
        const auto state = std::find_if(states_.begin(), states_.end(),
                                        [&](const PluginState& s) { return s.id == p.id && s.activated; });
        if (state == states_.end()) {
            continue;
        }
        const std::string engine = p.contributes["lsp"]["engine"].asString();
        if (groupEngine.count(group) == 0) {
            groupEngine[group] = engine;
        } else if (groupEngine[group] != engine) {
            warnings_.push_back("互斥组 " + group + " 中 " + p.id + "（" + engine + "）不会被启用，当前使用 " +
                                groupEngine[group]);
        }
    }

    return errors_.empty();
}

}  // namespace lightcpp
