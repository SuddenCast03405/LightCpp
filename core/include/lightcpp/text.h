// 终端表格：按显示宽度对齐（CJK 记 2 列），自动转义 Markdown 表格分隔符。
#pragma once

#include <string>
#include <vector>

namespace lightcpp::text {

/// UTF-8 显示宽度；东亚宽字符按 2 计。
std::size_t displayWidth(const std::string& text);

std::string padRight(const std::string& text, std::size_t width);

/// 把 | 与换行转义，避免描述里的分隔符拆坏表格。
std::string escapeCell(const std::string& text);

/// 渲染 Markdown 表格（首行为表头）。
std::string renderTable(const std::vector<std::string>& headers,
                        const std::vector<std::vector<std::string>>& rows);

/// 截断到指定显示宽度，超出部分以 … 结尾。
std::string truncate(const std::string& text, std::size_t width);

}  // namespace lightcpp::text
