#include "ClangCoreSpec.h"

namespace lightcpp::clangcore {

LspLaunch launch(const std::string& executable, const std::string& compileCommandsDir,
                 bool enableTidy, const std::string& tidyChecks) {
    LspLaunch out;
    out.program = executable.empty() ? "clangd" : executable;
    if (!compileCommandsDir.empty()) {
        out.arguments.push_back("--compile-commands-dir=" + compileCommandsDir);
    }
    // clang-tidy 与 background-index 互斥地占用索引线程，二者择一。
    out.arguments.push_back(enableTidy ? "--clang-tidy" : "--background-index");
    if (enableTidy && !tidyChecks.empty()) {
        out.arguments.push_back("--tidy-checks=" + tidyChecks);
    }
    return out;
}

std::vector<std::string> formatArguments(bool wholeProject, const std::string& file,
                                         const std::string& style) {
    std::vector<std::string> args;
    if (wholeProject) {
        args.push_back("-i");
    } else {
        args.push_back("--style=" + (style.empty() ? std::string("file") : style));
    }
    if (!file.empty()) {
        args.push_back(file);
    }
    return args;
}

}  // namespace lightcpp::clangcore
