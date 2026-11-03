// LightCpp —— 轻量级 C/C++ 插件的 JSON 值。
//
// 只做插件生态需要的事：解析清单（UTF-8，可直接 BOM）、保留对象键顺序、
// 方便地取字段、序列化回稳定格式。不追求 RFC 8259 之外的扩展（如注释、尾逗号）。
#pragma once

#include <cstddef>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lightcpp::json {

class Value;

/// 对象按插入顺序保存，序列化结果稳定，便于比对与生成文档。
using Object = std::vector<std::pair<std::string, Value>>;
using Array = std::vector<Value>;

enum class Type { Null, Bool, Number, String, List, Map };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(int n) : type_(Type::Number), number_(n) {}
    Value(long long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Value(double n) : type_(Type::Number), number_(n) {}
    Value(const char* s);
    Value(std::string s);
    Value(Array a);
    Value(Object o);

    static Value object() { Value v; v.type_ = Type::Map; return v; }
    static Value array() { Value v; v.type_ = Type::List; return v; }

    Type type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == Type::Null; }
    bool isBool() const noexcept { return type_ == Type::Bool; }
    bool isNumber() const noexcept { return type_ == Type::Number; }
    bool isString() const noexcept { return type_ == Type::String; }
    bool isArray() const noexcept { return type_ == Type::List; }
    bool isObject() const noexcept { return type_ == Type::Map; }

    bool asBool(bool fallback = false) const noexcept;
    double asNumber(double fallback = 0.0) const noexcept;
    /// 非字符串返回 fallback；字符串返回其内容。
    const std::string& asString(const std::string& fallback = emptyString()) const noexcept;

    /// 数组或对象元素个数；其他类型返回 0。
    std::size_t size() const noexcept;
    bool empty() const noexcept { return size() == 0; }

    /// 数组访问；越界或类型不符返回静态 null。
    const Value& at(std::size_t index) const;
    /// 对象访问；不存在返回静态 null。
    const Value& operator[](const std::string& key) const;
    /// 对象访问，不存在时创建（仅当自身为对象或 null）。
    Value& operator[](const std::string& key);
    bool contains(const std::string& key) const;
    /// 键名列表（对象）。
    const Object& members() const noexcept { return object_; }
    /// 数组元素（数组）。
    const Array& elements() const noexcept { return array_; }
    Array& elements() noexcept { return array_; }

    void push(Value v);
    void set(const std::string& key, Value v);
    /// 删除对象中的键，返回是否删除成功。
    bool erase(const std::string& key);

    /// 稳定序列化。indent = 0 输出紧凑格式。
    std::string dump(int indent = 0) const;

    static const Value& null();

private:
    static const std::string& emptyString() noexcept;
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

struct ParseError {
    std::string message;
    std::size_t line = 0;
    std::size_t column = 0;
    /// 便于日志定位：出错行内容。
    std::string snippet;

    std::string what() const;
};

/// 解析失败时 error 被填充，返回 null 值。
Value parse(const std::string& text, ParseError* error = nullptr);
/// 读取文件并解析；失败时 error 填充（含文件 I/O 错误）。
Value parseFile(const std::string& path, ParseError* error = nullptr);

/// 常用断言辅助：类型不符时返回空引用，避免在宿主侧崩溃。
const Array& asArrayOrEmpty(const Value& v);
const Object& asObjectOrEmpty(const Value& v);

}  // namespace lightcpp::json
