#include "TestCoreSpec.h"

namespace lightcpp::testcore {

bool isFailureLine(const std::string& line) {
    return line.rfind("[  FAILED  ]", 0) == 0 || line.rfind("FAILED", 0) == 0 ||
           line.rfind("*** FAILED ***", 0) == 0 || line.rfind("error:", 0) == 0;
}

TestCase parseFailureLocation(const std::string& line) {
    TestCase tc;
    tc.status = "failed";
    tc.message = line;
    const std::size_t first = line.find(':');
    if (first == std::string::npos) {
        return tc;
    }
    const std::size_t second = line.find(':', first + 1);
    if (second == std::string::npos) {
        return tc;
    }
    const std::string file = line.substr(0, first);
    if (file.size() < 2) {
        return tc;
    }
    int value = 0;
    for (std::size_t i = first + 1; i < second; ++i) {
        const char c = line[i];
        if (c < '0' || c > '9') {
            return tc;
        }
        value = value * 10 + (c - '0');
    }
    if (value <= 0) {
        return tc;
    }
    tc.file = file;
    tc.line = value;
    return tc;
}

Summary summarize(const std::vector<TestCase>& cases, bool timeout) {
    Summary summary;
    for (const TestCase& tc : cases) {
        if (tc.status == "failed") {
            ++summary.failed;
        } else if (tc.status == "skipped") {
            ++summary.skipped;
        } else {
            ++summary.passed;
        }
    }
    summary.timeout = timeout;
    return summary;
}

}  // namespace lightcpp::testcore
