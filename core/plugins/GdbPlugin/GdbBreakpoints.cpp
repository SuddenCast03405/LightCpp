#include "GdbBreakpoints.h"

#include <sstream>

namespace lightcpp::gdb {

std::string breakpointCommand(const Breakpoint& bp) {
    std::ostringstream os;
    os << "-break-insert";
    if (bp.temporary) {
        os << " -t";
    }
    if (bp.hitCount > 0) {
        os << " -i " << bp.hitCount;
    }
    if (!bp.condition.empty()) {
        os << " -c \"" << bp.condition << "\"";
    }
    if (!bp.address.empty()) {
        os << " *" << bp.address;
    } else if (!bp.function.empty()) {
        os << " " << bp.function;
    } else {
        os << " " << bp.file << ":" << bp.line;
    }
    return os.str();
}

int parseBreakpointNumber(const std::string& miOutput) {
    const std::string key = "number=\"";
    const std::size_t pos = miOutput.find(key);
    if (pos == std::string::npos) {
        return -1;
    }
    const std::size_t begin = pos + key.size();
    const std::size_t end = miOutput.find('"', begin);
    if (end == std::string::npos || end == begin) {
        return -1;
    }
    int value = 0;
    for (std::size_t i = begin; i < end; ++i) {
        if (miOutput[i] < '0' || miOutput[i] > '9') {
            return -1;
        }
        value = value * 10 + (miOutput[i] - '0');
    }
    return value;
}

std::vector<std::string> logpointCommands(int number, const std::string& message) {
    if (number <= 0 || message.empty()) {
        return {};
    }
    const std::string n = std::to_string(number);
    return {"commands " + n, "silent", "printf \"" + message + "\"", "continue", "end"};
}

}  // namespace lightcpp::gdb
