// 单元测试入口：按名称排序后逐个执行，支持用第一个参数做名字过滤。
// 例如：lightcpp_tests json     只跑名字含 "json" 的用例

#include "test_framework.h"

#include <algorithm>
#include <exception>

int main(int argc, char** argv) {
    using namespace lightcpp::testing;
    const std::string filter = argc > 1 ? argv[1] : "";

    std::vector<TestCase>& cases = registry();
    std::stable_sort(cases.begin(), cases.end(),
                     [](const TestCase& a, const TestCase& b) { return a.name < b.name; });

    int ran = 0;
    int failed = 0;
    for (const TestCase& test : cases) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) {
            continue;
        }
        ++ran;
        const int before = failureCount();
        try {
            test.body();
        } catch (const AssertionFailure& failure) {
            ++failureCount();
            std::cout << "  [FAIL] " << test.name << "\n         " << failure.message << "\n";
        } catch (const std::exception& e) {
            ++failureCount();
            std::cout << "  [FAIL] " << test.name << "\n         未捕获异常: " << e.what() << "\n";
        } catch (...) {
            ++failureCount();
            std::cout << "  [FAIL] " << test.name << "\n         未捕获的非标准异常\n";
        }
        if (failureCount() == before) {
            std::cout << "  [ ok ] " << test.name << "\n";
        } else {
            ++failed;
        }
    }
    std::cout << "\n" << ran << " 个用例，失败 " << failed << " 个\n";
    return failed == 0 && ran > 0 ? 0 : 1;
}
