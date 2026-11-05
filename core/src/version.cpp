#include "lightcpp/version.h"

#include <cctype>
#include <sstream>

namespace lightcpp {
namespace {

std::string trim(const std::string& s) {
    const auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t'; };
    std::size_t begin = 0;
    while (begin < s.size() && !notSpace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && !notSpace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

bool allDigits(const std::string& s, std::size_t begin, std::size_t end) {
    if (begin >= end) {
        return false;
    }
    for (std::size_t i = begin; i < end; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

/// 十进制解析并做上界检查；超界返回 false 而不是回绕。
bool parsePart(const std::string& s, std::size_t begin, std::size_t end, int* out) {
    if (!allDigits(s, begin, end)) {
        return false;
    }
    long long value = 0;
    for (std::size_t i = begin; i < end; ++i) {
        value = value * 10 + (s[i] - '0');
        if (value > Version::kMaxPart) {
            return false;
        }
    }
    *out = static_cast<int>(value);
    return true;
}

}  // namespace

bool Version::parse(const std::string& text, Version* out) {
    std::size_t first = text.find('.');
    if (first == std::string::npos) {
        return false;
    }
    std::size_t second = text.find('.', first + 1);
    if (second == std::string::npos) {
        return false;
    }
    if (text.find('.', second + 1) != std::string::npos) {
        return false;
    }
    Version v;
    if (!parsePart(text, 0, first, &v.major) || !parsePart(text, first + 1, second, &v.minor) ||
        !parsePart(text, second + 1, text.size(), &v.patch)) {
        return false;
    }
    *out = v;
    return true;
}

std::string Version::str() const {
    std::ostringstream os;
    os << major << '.' << minor << '.' << patch;
    return os.str();
}

bool parseConstraint(const std::string& text, std::string* error) {
    auto fail = [&](const std::string& reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };
    std::string body = trim(text);
    if (body.empty()) {
        return fail("版本约束为空");
    }
    if (body == "*") {
        return true;
    }
    std::size_t skip = 0;
    if (body.compare(0, 2, ">=") == 0) {
        skip = 2;
    } else if (body[0] == '^' || body[0] == '~') {
        skip = 1;
    } else if (!std::isdigit(static_cast<unsigned char>(body[0]))) {
        return fail("无法识别的约束前缀：" + body);
    }
    Version v;
    if (!Version::parse(body.substr(skip), &v)) {
        return fail("约束中的版本号非法：" + body);
    }
    return true;
}

bool satisfies(const std::string& version, const std::string& constraint, std::string* error) {
    std::string reason;
    if (!parseConstraint(constraint, &reason)) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    }
    Version ver;
    if (!Version::parse(version, &ver)) {
        if (error != nullptr) {
            *error = "版本号非法：" + version;
        }
        return false;
    }
    if (constraint == "*") {
        return true;
    }
    const bool lowerBound = constraint.compare(0, 2, ">=") == 0;
    const char prefix = lowerBound ? '\0' : constraint[0];
    const std::size_t skip = (lowerBound || prefix == '^' || prefix == '~') ? (lowerBound ? 2u : 1u) : 0u;
    Version base;
    Version::parse(constraint.substr(skip), &base);

    if (lowerBound) {
        return ver >= base;
    }
    if (prefix == '^') {
        if (ver < base) {
            return false;
        }
        if (base.major > 0) {
            return ver.major == base.major;
        }
        if (base.minor > 0) {
            return ver.major == 0 && ver.minor == base.minor;
        }
        return ver == base;
    }
    if (prefix == '~') {
        return ver >= base && ver.major == base.major && ver.minor == base.minor;
    }
    return ver == base;
}

}  // namespace lightcpp
