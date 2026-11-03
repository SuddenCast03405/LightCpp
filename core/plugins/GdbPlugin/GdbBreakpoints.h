// gdb 断点的命令生成与 MI 输出解析。
//
// 这里刻意只做"命令生成 / 输出解析"，不拉进程、不碰终端：
// 真正的调试会话由宿主按 DAP 驱动，插件只提供平台相关知识，
// 因此这段逻辑在任何平台上都能单测。
#pragma once

#include <string>
#include <vector>

namespace lightcpp::gdb {

struct Breakpoint {
    std::string file;
    std::string function;
    std::string address;
    std::string condition;
    int line = 0;
    int hitCount = 0;
    bool temporary = false;
};

/// 生成 -break-insert 的 MI 命令。
std::string breakpointCommand(const Breakpoint& bp);

/// 解析 MI 输出中的断点编号：^done,bkpt={number="3",...}；失败返回 -1。
int parseBreakpointNumber(const std::string& miOutput);

/// 日志断点在 gdb 里没有原生支持，展开成 commands / printf / continue。
std::vector<std::string> logpointCommands(int number, const std::string& message);

}  // namespace lightcpp::gdb
