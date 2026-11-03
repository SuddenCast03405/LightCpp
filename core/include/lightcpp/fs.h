// 文件系统的最小封装：只提供内核需要的几个动作。
//
// 不引入 std::filesystem，是为了兼容老 GCC/Clang 与 Windows 上 C++17 之前的工具链，
// 同时让移植到其他平台时只需改这一个文件。
#pragma once

#include <string>
#include <vector>

namespace lightcpp::fs {

/// 目录分隔符：'/' 或 '\\'。
char separator();

std::string join(const std::string& a, const std::string& b);
std::string absolute(const std::string& path);
std::string parent(const std::string& path);
std::string filename(const std::string& path);
std::string stem(const std::string& path);
std::string extension(const std::string& path);
std::string currentDirectory();
std::string homeDirectory();

bool exists(const std::string& path);
bool isDirectory(const std::string& path);
bool isRegularFile(const std::string& path);

/// 列出目录中的条目名（不含路径），排序后返回；目录不存在返回空。
std::vector<std::string> listDirectory(const std::string& path);

/// 递归列出匹配后缀名的文件（相对于 root 的路径，使用 '/'），排序稳定。
std::vector<std::string> listFilesRecursive(const std::string& root, const std::string& suffix,
                                            int maxDepth = 6);

bool readFile(const std::string& path, std::string* content);
bool writeFile(const std::string& path, const std::string& content);
bool makeDirectories(const std::string& path);

/// Windows 上是否为可执行文件（扩展名 .exe/.bat/.cmd/.com），其他平台一律 true。
bool looksExecutable(const std::string& name);

}  // namespace lightcpp::fs
