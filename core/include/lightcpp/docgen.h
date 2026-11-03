// 文档生成：docs/PLUGINS.md 与 docs/FEATURES.md 由清单派生。
//
// 这两个文件不允许手改：CI 跑 `--check` 比对，改了清单忘了重新生成会直接失败。
#pragma once

#include <string>

#include "lightcpp/manifest.h"

namespace lightcpp::docs {

std::string renderPlugins(const Repository& repo);
std::string renderFeatures(const Repository& repo);

/// 生成结果与磁盘内容一致返回 true；不一致时 diffHint 说明哪个文件过期。
bool upToDate(const Repository& repo, std::string* diffHint);

bool write(const Repository& repo, std::string* error);

}  // namespace lightcpp::docs
