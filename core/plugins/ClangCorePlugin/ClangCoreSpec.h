// clang-core 的纯函数：LSP 启动参数与格式化参数。
// 抽成头文件是为了让测试能直接验证逻辑，而不必加载动态库。
#pragma once

#include <string>
#include <vector>

namespace lightcpp::clangcore {

struct LspLaunch {
    std::string program;
    std::vector<std::string> arguments;
};

/// clangd 启动参数。compile_commands 目录直接影响索引质量，必须显式传入。
LspLaunch launch(const std::string& executable, const std::string& compileCommandsDir,
                 bool enableTidy, const std::string& tidyChecks);

/// 格式化参数：项目级用 clang-format -i，单文件优先交给 clangd 内置能力。
std::vector<std::string> formatArguments(bool wholeProject, const std::string& file,
                                         const std::string& style);

}  // namespace lightcpp::clangcore
