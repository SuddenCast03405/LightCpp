// test-core 的纯函数：失败位置解析与结果汇总。
//
// 规划里 gtest / catch2 / boost / ctest 都只是"解析器插件"，公共的超时、
// 重跑、失败跳转由 test-core 统一提供 —— 这里就是统一的那部分。
#pragma once

#include <string>
#include <vector>

namespace lightcpp::testcore {

struct TestCase {
    std::string suite;
    std::string name;
    std::string status;  // passed / failed / skipped
    std::string file;
    int line = 0;
    std::string message;
};

struct Summary {
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    bool timeout = false;

    int total() const { return passed + failed + skipped; }
    bool green() const { return failed == 0 && !timeout && passed > 0; }
};

/// 解析 "file:line: message" 形式的失败信息；无法解析时 line 为 0。
TestCase parseFailureLocation(const std::string& line);

/// 判断一行输出是否表示失败。
bool isFailureLine(const std::string& line);

/// 汇总结果；timeout 为 true 时整体判红。
Summary summarize(const std::vector<TestCase>& cases, bool timeout);

}  // namespace lightcpp::testcore
