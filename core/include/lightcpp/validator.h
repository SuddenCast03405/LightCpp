// 语义校验：把"规划里的硬条款"变成可执行的规则。
//
// 每条规则都有稳定的错误码，CI 与文档按错误码引用；新增规则只追加码，不复用。
// 规则与 tools/lightcpp_pm.py 的 validate 对齐，两边输出的码表必须一致。
#pragma once

#include <string>
#include <vector>

#include "lightcpp/manifest.h"

namespace lightcpp {

enum class Severity { Error, Warning };

struct Problem {
    Severity severity = Severity::Error;
    std::string code;
    std::string message;

    bool isError() const { return severity == Severity::Error; }
    std::string format() const;
};

struct ValidateOptions {
    /// 严格模式：保留 W002 等提示级告警；CI 使用严格模式。
    bool strict = false;
    /// 目标平台（linux/darwin/win32）；为空时按当前编译平台推断，用于平台可见性提示。
    std::string platform;
};

std::vector<Problem> validate(const Repository& repo, const ValidateOptions& options = {});

/// 依赖环列表，每个环以重复的起点收尾（a -> b -> a）。
std::vector<std::vector<std::string>> findCycles(const Repository& repo);

/// 单项规则检查（供测试与 docs 复用）。
std::vector<Problem> checkInvariants(const Repository& repo);

/// 功能归属：功能级 plugins 覆盖功能集级，except 做排除。
std::vector<std::string> featureOwners(const CatalogSet& set, const json::Value& feature);

}  // namespace lightcpp
