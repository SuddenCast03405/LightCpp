// 构建系统适配器的公共契约。
//
// 规划要求每个构建系统插件"强依赖 build-core"，但运行时没有统一下沉点；
// 这里定义最小契约：宿主按 buildSystems 贡献点找到插件，插件提供
//   1. detect：判断目录是否是它的工程
//   2. plan：生成"配置 / 构建 / 安装"命令行
// 两者都是纯函数，没有真实工具链的机器上也能验证逻辑。
#pragma once

#include <string>
#include <vector>

#include "lightcpp/fs.h"

namespace lightcpp::build {

/// 一次构建动作。
struct Command {
    std::string program;
    std::vector<std::string> arguments;
};

/// 构建系统描述。新增构建系统只需在 BuildSystemFactory.cpp 里补一条。
struct SystemSpec {
    std::string id;
    /// 探测工程根目录用的特征文件名；空表示不探测。
    std::string buildFile;
    /// 配置阶段工具名；空表示无需独立配置阶段。
    std::string configureTool;
    /// 构建阶段工具名。
    std::string buildTool;
    bool supportsInstall = false;
};

/// 目录中是否存在该构建系统的特征文件。
bool detect(const SystemSpec& spec, const std::string& directory);

/// 生成命令行。jobs <= 0 表示使用工具默认并行度。
std::vector<Command> plan(const SystemSpec& spec, const std::string& sourceDir,
                          const std::string& buildDir, int jobs, bool install);

}  // namespace lightcpp::build
