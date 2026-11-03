// 按插件 id 返回构建系统描述；未知 id 返回 nullptr。
#pragma once

#include <string>

#include "BuildSystemSpec.h"

namespace lightcpp::build {

const SystemSpec* specFor(const std::string& id);

}  // namespace lightcpp::build
