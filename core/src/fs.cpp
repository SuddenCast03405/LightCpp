#include "lightcpp/fs.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lightcpp::fs {
namespace {

bool isSep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

std::string normalizeSlashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

/// 去掉多余的尾随 "/." 与重复分隔符：`/a/b/.` -> `/a/b`。
/// 只做纯字符串层面的收尾，不解析 `..`（那会改变语义，也不是本函数该管的事）。
std::string trimDotSegments(std::string path) {
    if (path.empty()) {
        return path;
    }
    // 反复剥掉尾部 "/."。
    while (path.size() > 2 && path.compare(path.size() - 2, 2, "/.") == 0) {
        path.erase(path.size() - 2);
    }
    if (path.size() > 1 && path.back() == '/' && path[path.size() - 2] != ':') {
        path.pop_back();
    }
    // 根目录别被削成空串。
    return path.empty() ? std::string("/") : path;
}

}  // namespace

char separator() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    if (isSep(a.back()) || (b.size() > 1 && b[1] == ':')) {
        return a + b;
    }
    return a + separator() + b;
}

std::string absolute(const std::string& path) {
    if (path.empty() || path == ".") {
        return currentDirectory();
    }
#ifdef _WIN32
    char buffer[MAX_PATH];
    if (_fullpath(buffer, path.c_str(), MAX_PATH) != nullptr) {
        return trimDotSegments(normalizeSlashes(buffer));
    }
    return path;
#else
    if (path[0] == '/') {
        return trimDotSegments(path);
    }
    char buffer[4096];
    if (getcwd(buffer, sizeof(buffer)) == nullptr) {
        return path;
    }
    return trimDotSegments(join(buffer, path));
#endif
}

std::string parent(const std::string& path) {
    const std::string p = normalizeSlashes(path);
    std::size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return "/";
    }
    std::string head = p.substr(0, pos);
    if (head.size() == 2 && head[1] == ':') {
        head += '/';
    }
    return head;
}

std::string filename(const std::string& path) {
    const std::string p = normalizeSlashes(path);
    std::size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string extension(const std::string& path) {
    const std::string name = filename(path);
    std::size_t pos = name.find_last_of('.');
    if (pos == std::string::npos || pos == 0) {
        return {};
    }
    return name.substr(pos);
}

std::string stem(const std::string& path) {
    const std::string name = filename(path);
    std::size_t pos = name.find_last_of('.');
    return pos == std::string::npos ? name : name.substr(0, pos);
}

std::string currentDirectory() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    if (_getcwd(buffer, MAX_PATH) != nullptr) {
        return buffer;
    }
    return ".";
#else
    char buffer[4096];
    if (getcwd(buffer, sizeof(buffer)) != nullptr) {
        return buffer;
    }
    return ".";
#endif
}

std::string homeDirectory() {
#ifdef _WIN32
    const char* profile = std::getenv("USERPROFILE");
    return profile == nullptr ? std::string(".") : std::string(profile);
#else
    const char* home = std::getenv("HOME");
    return home == nullptr ? std::string("~") : std::string(home);
#endif
}

bool exists(const std::string& path) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
#endif
}

bool isDirectory(const std::string& path) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool isRegularFile(const std::string& path) {
#ifndef _WIN32
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#else
    return exists(path) && !isDirectory(path);
#endif
}

std::vector<std::string> listDirectory(const std::string& path) {
    std::vector<std::string> names;
    if (!isDirectory(path)) {
        return names;
    }
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA((path + "\\*").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return names;
    }
    do {
        const std::string name = data.cFileName;
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    } while (FindNextFileA(handle, &data) != 0);
    FindClose(handle);
#else
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return names;
    }
    while (struct dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }
    closedir(dir);
#endif
    std::sort(names.begin(), names.end());
    return names;
}

namespace {

void listRecursive(const std::string& root, const std::string& rel, const std::string& suffix, int depth,
                   int maxDepth, std::vector<std::string>* out) {
    if (depth > maxDepth) {
        return;
    }
    const std::string dir = rel.empty() ? root : join(root, rel);
    for (const std::string& name : listDirectory(dir)) {
        const std::string childRel = rel.empty() ? name : rel + "/" + name;
        if (isDirectory(join(root, childRel))) {
            listRecursive(root, childRel, suffix, depth + 1, maxDepth, out);
        } else if (suffix.empty() ||
                   (childRel.size() >= suffix.size() &&
                    childRel.compare(childRel.size() - suffix.size(), suffix.size(), suffix) == 0)) {
            out->push_back(childRel);
        }
    }
}

}  // namespace

std::vector<std::string> listFilesRecursive(const std::string& root, const std::string& suffix, int maxDepth) {
    std::vector<std::string> out;
    listRecursive(root, "", suffix, 0, maxDepth, &out);
    std::sort(out.begin(), out.end());
    return out;
}

bool readFile(const std::string& path, std::string* content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    *content = buffer.str();
    return true;
}

bool writeFile(const std::string& path, const std::string& content) {
    const std::string dir = parent(path);
    if (dir != "." && !dir.empty() && !isDirectory(dir)) {
        makeDirectories(dir);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

bool makeDirectories(const std::string& path) {
    if (path.empty() || isDirectory(path)) {
        return true;
    }
    const std::string dir = parent(path);
    if (!dir.empty() && dir != "." && dir != path) {
        makeDirectories(dir);
    }
#ifdef _WIN32
    return _mkdir(path.c_str()) == 0 || isDirectory(path);
#else
    return ::mkdir(path.c_str(), 0775) == 0 || isDirectory(path);
#endif
}

bool looksExecutable(const std::string& name) {
#ifdef _WIN32
    const std::string ext = extension(name);
    return ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".com";
#else
    (void)name;
    return true;
#endif
}

}  // namespace lightcpp::fs
