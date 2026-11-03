// 解析：把一次安装请求展开成"安装闭包 + 安装顺序"。
//
// 规则来自清单，不在这里硬编码：
//   * dependencies  必装
//   * recommends.autoInstall 为 true 时连带安装（如 CMake -> Ninja）
//   * 扩展包（pack）只贡献成员，自身不产生副作用
#pragma once

#include <string>
#include <vector>

#include "lightcpp/manifest.h"

namespace lightcpp {

struct InstallStep {
    std::string id;
    std::string layer;
    std::string version;
    /// 直接安装 / 由 X 引入 / 自动连带安装（X）
    std::string origin;
};

struct Resolution {
    std::vector<InstallStep> steps;
    /// 互斥组提示，例如 cpp-lsp 内同时装了 clangd 与 ccls。
    std::vector<std::string> notes;
    std::vector<std::string> errors;
    bool ok() const { return errors.empty(); }
};

Resolution resolve(const Repository& repo, const std::vector<std::string>& roots);

/// 拓扑排序安装闭包；存在环时返回空并填充 error。
std::vector<std::string> topologicalOrder(const Repository& repo, const std::vector<std::string>& ids,
                                          std::string* error);

}  // namespace lightcpp
