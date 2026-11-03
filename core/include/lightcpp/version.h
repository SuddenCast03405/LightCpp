// 版本号与约束求解。
//
// 语义与 docs/PLUGIN-AUTHORING.md 第 3 节一致，并与 tools/lightcpp_pm.py 保持同步：
//   *           任意版本
//   x.y.z       精确匹配
//   ^x.y.z      x>0: >=x.y.z <(x+1).0.0；x==0,y>0: 锁次版本；x==y==0: 精确匹配
//   ~x.y.z      >=x.y.z <x.(y+1).0
//   >=x.y.z     下限（含）
#pragma once

#include <string>
#include <tuple>

namespace lightcpp {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    /// 版本号每段上限 65535，超出视为非法，避免后续比较溢出。
    static constexpr int kMaxPart = 65535;

    static bool parse(const std::string& text, Version* out);
    std::string str() const;

    bool operator==(const Version& other) const { return key() == other.key(); }
    bool operator!=(const Version& other) const { return !(*this == other); }
    bool operator<(const Version& other) const { return key() < other.key(); }
    bool operator<=(const Version& other) const { return key() <= other.key(); }
    bool operator>(const Version& other) const { return key() > other.key(); }
    bool operator>=(const Version& other) const { return key() >= other.key(); }

    std::tuple<int, int, int> key() const { return {major, minor, patch}; }
};

/// 解析约束表达式。非法表达式返回 false，error 填写原因。
bool parseConstraint(const std::string& text, std::string* error = nullptr);

/// 判断版本是否满足约束。非法约束返回 false 并填写 error。
bool satisfies(const std::string& version, const std::string& constraint,
               std::string* error = nullptr);

}  // namespace lightcpp
