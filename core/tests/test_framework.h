// LightCpp 极简测试框架
// 不依赖任何第三方库，只依赖 C++17 标准库。
// 用法：在任意 .cpp 中写 LIGHTC_TEST(用例名) { CHECK_EQ(1 + 1, 2); }
// 用例通过静态对象在启动时自动注册，由 lightcpp_test_main.cpp 统一调度。

#ifndef LIGHTC_TEST_FRAMEWORK_H
#define LIGHTC_TEST_FRAMEWORK_H

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace lightcpp {
namespace testing {

// ---------------------------------------------------------------------------
// 用例注册
// ---------------------------------------------------------------------------

struct TestCase {
    std::string name;
    std::string file;
    int line;
    std::function<void()> body;
};

// 全局用例表：函数内静态对象，保证跨编译单元初始化顺序安全
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

// 全局失败计数
inline int& failureCount() {
    static int count = 0;
    return count;
}

// 静态注册器：构造时把用例塞进全局表
struct Registrar {
    Registrar(const char* name, const char* file, int line, std::function<void()> body) {
        registry().push_back(TestCase{name, file, line, std::move(body)});
    }
};

// ---------------------------------------------------------------------------
// 断言
// ---------------------------------------------------------------------------

// 断言失败时抛出，由用例执行器捕获后记为一条失败
struct AssertionFailure {
    std::string message;
};

// 把任意值渲染成可读文本：字符串加引号，bool 写 true/false，枚举转整数
template <typename T>
std::string stringify(const T& value) {
    if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
        return "\"" + value + "\"";
    } else if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_same_v<std::decay_t<T>, const char*> ||
                         std::is_same_v<std::decay_t<T>, char*>) {
        return value == nullptr ? "null" : "\"" + std::string(value) + "\"";
    } else if constexpr (std::is_enum_v<std::decay_t<T>>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
        std::ostringstream os;
        os << value;
        return os.str();
    } else {
        return "<不可打印的值>";
    }
}

// 比较两个表达式并给出一行诊断：期望值与实际值都会打印出来
template <typename A, typename B>
std::string detail(const A& expected, const B& actual) {
    return "期望 " + stringify(expected) + "，实际 " + stringify(actual);
}

// 浮点近等比较的辅助
inline bool nearlyEqual(double a, double b, double epsilon) {
    return std::fabs(a - b) <= epsilon;
}

}  // namespace testing
}  // namespace lightcpp

// ---------------------------------------------------------------------------
// 宏（宏体内不得出现 // 注释，否则会吞掉续行符）
// ---------------------------------------------------------------------------

#define LIGHTC_CAT_INNER(a, b) a##b
#define LIGHTC_CAT(a, b) LIGHTC_CAT_INNER(a, b)

#define LIGHTC_TEST(name)                                                       \
    static void LIGHTC_CAT(lightcpp_test_body_, name)();                        \
    static ::lightcpp::testing::Registrar LIGHTC_CAT(lightcpp_test_reg_, name)( \
        #name, __FILE__, __LINE__, &LIGHTC_CAT(lightcpp_test_body_, name));     \
    static void LIGHTC_CAT(lightcpp_test_body_, name)()

#define CHECK_TRUE(cond)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            throw ::lightcpp::testing::AssertionFailure{                          \
                std::string("断言失败: ") + #cond + " 应为真（" + __FILE__ + ":" + \
                std::to_string(__LINE__) + "）"};                                 \
        }                                                                         \
    } while (false)

#define CHECK_FALSE(cond)                                                         \
    do {                                                                          \
        if ((cond)) {                                                             \
            throw ::lightcpp::testing::AssertionFailure{                          \
                std::string("断言失败: ") + #cond + " 应为假（" + __FILE__ + ":" + \
                std::to_string(__LINE__) + "）"};                                 \
        }                                                                         \
    } while (false)

#define CHECK_EQ(expected, actual)                                                \
    do {                                                                          \
        const auto& lightcpp_expected_value = (expected);                         \
        const auto& lightcpp_actual_value = (actual);                             \
        if (!(lightcpp_expected_value == lightcpp_actual_value)) {                \
            throw ::lightcpp::testing::AssertionFailure{                          \
                std::string("断言失败: ") + #expected + " == " + #actual + "，" + \
                ::lightcpp::testing::detail(lightcpp_expected_value,              \
                                            lightcpp_actual_value) +              \
                "（" + __FILE__ + ":" + std::to_string(__LINE__) + "）"};         \
        }                                                                         \
    } while (false)

#define CHECK_NE(unexpected, actual)                                              \
    do {                                                                          \
        const auto& lightcpp_unexpected_value = (unexpected);                     \
        const auto& lightcpp_actual_value = (actual);                             \
        if (lightcpp_unexpected_value == lightcpp_actual_value) {                 \
            throw ::lightcpp::testing::AssertionFailure{                          \
                std::string("断言失败: ") + #unexpected + " != " + #actual +      \
                "，" + ::lightcpp::testing::detail(lightcpp_unexpected_value,     \
                                                   lightcpp_actual_value) +       \
                "（" + __FILE__ + ":" + std::to_string(__LINE__) + "）"};         \
        }                                                                         \
    } while (false)

#define CHECK_THROWS(expr)                                                        \
    do {                                                                          \
        bool lightcpp_threw = false;                                              \
        try {                                                                     \
            (void)(expr);                                                         \
        } catch (...) {                                                           \
            lightcpp_threw = true;                                                \
        }                                                                         \
        if (!lightcpp_threw) {                                                    \
            throw ::lightcpp::testing::AssertionFailure{                          \
                std::string("断言失败: ") + #expr + " 应抛出异常（" + __FILE__ +   \
                ":" + std::to_string(__LINE__) + "）"};                           \
        }                                                                         \
    } while (false)

#endif  // LIGHTC_TEST_FRAMEWORK_H
