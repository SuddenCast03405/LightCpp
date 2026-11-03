#include "lightcpp/json.h"

#include <cerrno>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace lightcpp::json {
namespace {

const Value kNull{};

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void escapeTo(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char raw : s) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

std::string formatNumber(double v) {
    if (!std::isfinite(v)) {
        return "null";
    }
    if (v == static_cast<double>(static_cast<long long>(v)) && std::fabs(v) < 1e15) {
        return std::to_string(static_cast<long long>(v));
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

/// 递归下降解析器；错误位置精确到行列，便于直接反馈给插件作者。
class Parser {
public:
    Parser(const std::string& text, ParseError* error) : text_(text), error_(error) {}

    Value run() {
        skipBom();
        Value v = parseValue(0);
        if (!ok_) {
            return Value();
        }
        skipSpace();
        if (pos_ != text_.size()) {
            fail("根值之后存在多余内容");
            return Value();
        }
        return v;
    }

private:
    static constexpr int kMaxDepth = 200;

    void skipBom() {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    void skipSpace() {
        while (pos_ < text_.size() && isSpace(text_[pos_])) {
            if (text_[pos_] == '\n') {
                ++line_;
                lineStart_ = pos_ + 1;
            }
            ++pos_;
        }
    }

    bool fail(const std::string& message) {
        if (ok_) {
            ok_ = false;
            if (error_ != nullptr) {
                error_->message = message;
                error_->line = line_;
                error_->column = pos_ - lineStart_ + 1;
                std::size_t begin = lineStart_;
                std::size_t end = text_.find('\n', lineStart_);
                if (end == std::string::npos) {
                    end = text_.size();
                }
                error_->snippet = text_.substr(begin, end - begin);
            }
        }
        return false;
    }

    bool expect(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return fail(std::string("期望 '") + c + "'");
    }

    Value parseValue(int depth) {
        if (depth > kMaxDepth) {
            fail("嵌套过深（超过 200 层），疑似异常输入");
            return Value();
        }
        skipSpace();
        if (pos_ >= text_.size()) {
            fail("意外的文件结尾");
            return Value();
        }
        switch (text_[pos_]) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': return Value(parseString());
            case 't':
                if (text_.compare(pos_, 4, "true") == 0) {
                    pos_ += 4;
                    return Value(true);
                }
                fail("非法字面量（是否想写 true？）");
                return Value();
            case 'f':
                if (text_.compare(pos_, 5, "false") == 0) {
                    pos_ += 5;
                    return Value(false);
                }
                fail("非法字面量（是否想写 false？）");
                return Value();
            case 'n':
                if (text_.compare(pos_, 4, "null") == 0) {
                    pos_ += 4;
                    return Value();
                }
                fail("非法字面量（是否想写 null？）");
                return Value();
            default: return parseNumber();
        }
    }

    Value parseObject(int depth) {
        ++pos_;  // {
        Value v = Value::object();
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return v;
        }
        while (true) {
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                fail("对象的键必须是字符串");
                return Value();
            }
            std::string key = parseString();
            if (!ok_) {
                return Value();
            }
            skipSpace();
            if (!expect(':')) {
                return Value();
            }
            Value child = parseValue(depth + 1);
            if (!ok_) {
                return Value();
            }
            if (v.contains(key)) {
                // 重复键是常见的手工编辑事故，直接报错而不是静默覆盖。
                fail("对象出现重复键 \"" + key + "\"");
                return Value();
            }
            v.set(key, std::move(child));
            skipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (!expect('}')) {
                return Value();
            }
            return v;
        }
    }

    Value parseArray(int depth) {
        ++pos_;  // [
        Value v = Value::array();
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return v;
        }
        while (true) {
            Value child = parseValue(depth + 1);
            if (!ok_) {
                return Value();
            }
            v.push(std::move(child));
            skipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (!expect(']')) {
                return Value();
            }
            return v;
        }
    }

    unsigned int parseHex4() {
        if (pos_ + 4 > text_.size()) {
            fail("\\u 转义不完整");
            return 0;
        }
        unsigned int value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = text_[pos_ + i];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned int>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned int>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned int>(c - 'A' + 10);
            } else {
                fail("\\u 转义包含非十六进制字符");
                return 0;
            }
        }
        pos_ += 4;
        return value;
    }

    std::string parseString() {
        ++pos_;  // "
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("字符串中出现未转义的控制字符");
                return {};
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                fail("转义序列不完整");
                return {};
            }
            char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned int cp = parseHex4();
                    if (!ok_) {
                        return {};
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned int low = parseHex4();
                        if (!ok_) {
                            return {};
                        }
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            fail("代理对不完整");
                            return {};
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    fail(std::string("非法转义字符 \\") + esc);
                    return {};
            }
        }
        fail("字符串未闭合");
        return {};
    }

    Value parseNumber() {
        std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        while (pos_ < text_.size() &&
               ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.' ||
                text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' || text_[pos_] == '-')) {
            ++pos_;
        }
        if (start == pos_) {
            fail("无法识别的值");
            return Value();
        }
        errno = 0;
        char* end = nullptr;
        const std::string token = text_.substr(start, pos_ - start);
        double value = std::strtod(token.c_str(), &end);
        if (errno == ERANGE || end != token.c_str() + token.size()) {
            pos_ = start;
            fail("非法数字：" + token);
            return Value();
        }
        return Value(value);
    }

    const std::string& text_;
    ParseError* error_ = nullptr;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t lineStart_ = 0;
    bool ok_ = true;
};

}  // namespace

Value::Value(const char* s) : type_(Type::String), string_(s == nullptr ? "" : s) {}
Value::Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
Value::Value(Array a) : type_(Type::List), array_(std::move(a)) {}
Value::Value(Object o) : type_(Type::Map), object_(std::move(o)) {}

const std::string& Value::emptyString() noexcept {
    static const std::string kEmpty;
    return kEmpty;
}

const Value& Value::null() { return kNull; }

bool Value::asBool(bool fallback) const noexcept {
    return type_ == Type::Bool ? bool_ : fallback;
}

double Value::asNumber(double fallback) const noexcept {
    return type_ == Type::Number ? number_ : fallback;
}

const std::string& Value::asString(const std::string& fallback) const noexcept {
    return type_ == Type::String ? string_ : fallback;
}

std::size_t Value::size() const noexcept {
    if (type_ == Type::List) {
        return array_.size();
    }
    if (type_ == Type::Map) {
        return object_.size();
    }
    return 0;
}

const Value& Value::at(std::size_t index) const {
    if (type_ != Type::List || index >= array_.size()) {
        return kNull;
    }
    return array_[index];
}

const Value& Value::operator[](const std::string& key) const {
    if (type_ != Type::Map) {
        return kNull;
    }
    for (const auto& [k, v] : object_) {
        if (k == key) {
            return v;
        }
    }
    return kNull;
}

Value& Value::operator[](const std::string& key) {
    if (type_ == Type::Null) {
        type_ = Type::Map;
    }
    for (auto& [k, v] : object_) {
        if (k == key) {
            return v;
        }
    }
    object_.emplace_back(key, Value());
    return object_.back().second;
}

bool Value::contains(const std::string& key) const {
    if (type_ != Type::Map) {
        return false;
    }
    for (const auto& [k, v] : object_) {
        if (k == key) {
            return true;
        }
    }
    return false;
}

void Value::push(Value v) {
    if (type_ == Type::Null) {
        type_ = Type::List;
    }
    if (type_ == Type::List) {
        array_.push_back(std::move(v));
    }
}

void Value::set(const std::string& key, Value v) {
    (*this)[key] = std::move(v);
}

bool Value::erase(const std::string& key) {
    if (type_ != Type::Map) {
        return false;
    }
    for (auto it = object_.begin(); it != object_.end(); ++it) {
        if (it->first == key) {
            object_.erase(it);
            return true;
        }
    }
    return false;
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
    const std::string pad(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth + 1), ' ');
    const std::string padEnd(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
    const char* nl = indent > 0 ? "\n" : "";
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: out += formatNumber(number_); break;
        case Type::String: escapeTo(out, string_); break;
        case Type::List: {
            if (array_.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            out += nl;
            for (std::size_t i = 0; i < array_.size(); ++i) {
                out += pad;
                array_[i].dumpTo(out, indent, depth + 1);
                if (i + 1 != array_.size()) {
                    out += ',';
                }
                out += nl;
            }
            out += padEnd;
            out += ']';
            break;
        }
        case Type::Map: {
            if (object_.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            out += nl;
            for (std::size_t i = 0; i < object_.size(); ++i) {
                out += pad;
                escapeTo(out, object_[i].first);
                out += indent > 0 ? ": " : ":";
                object_[i].second.dumpTo(out, indent, depth + 1);
                if (i + 1 != object_.size()) {
                    out += ',';
                }
                out += nl;
            }
            out += padEnd;
            out += '}';
            break;
        }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

std::string ParseError::what() const {
    std::ostringstream os;
    os << "第 " << line << " 行第 " << column << " 列：" << message;
    if (!snippet.empty()) {
        os << "\n    " << snippet;
    }
    return os.str();
}

Value parse(const std::string& text, ParseError* error) {
    return Parser(text, error).run();
}

Value parseFile(const std::string& path, ParseError* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) {
            error->message = "无法打开文件（" + path + "）";
            error->line = 0;
            error->column = 0;
        }
        return Value();
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse(buffer.str(), error);
}

const Array& asArrayOrEmpty(const Value& v) {
    static const Array kEmpty;
    return v.isArray() ? v.elements() : kEmpty;
}

const Object& asObjectOrEmpty(const Value& v) {
    static const Object kEmpty;
    return v.isObject() ? v.members() : kEmpty;
}

}  // namespace lightcpp::json
