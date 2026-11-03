#include "BuildSystemSpec.h"

namespace lightcpp::build {

bool detect(const SystemSpec& spec, const std::string& directory) {
    if (spec.buildFile.empty()) {
        return false;
    }
    return fs::isRegularFile(fs::join(directory, spec.buildFile));
}

std::vector<Command> plan(const SystemSpec& spec, const std::string& sourceDir,
                          const std::string& buildDir, int jobs, bool install) {
    std::vector<Command> commands;
    const std::string jobFlag = jobs > 0 ? std::to_string(jobs) : std::string();

    if (!spec.configureTool.empty()) {
        Command configure{spec.configureTool, {}};
        if (spec.configureTool == "cmake") {
            // 固定 Ninja 生成器，与清单里 cmake -> ninja 的自动连带安装保持一致。
            configure.arguments = {"-S", sourceDir, "-B", buildDir, "-G", "Ninja",
                                   "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
        } else if (spec.configureTool == "meson") {
            configure.arguments = {"setup", buildDir, sourceDir};
        }
        commands.push_back(configure);
    }

    Command build{spec.buildTool, {}};
    if (spec.buildTool == "cmake") {
        build.arguments = {"--build", buildDir};
        if (!jobFlag.empty()) {
            build.arguments.insert(build.arguments.end(), {"--parallel", jobFlag});
        }
    } else if (spec.buildTool == "ninja" || spec.buildTool == "make") {
        build.arguments = {"-C", buildDir};
        if (!jobFlag.empty()) {
            build.arguments.insert(build.arguments.end(), {"-j", jobFlag});
        }
    } else if (spec.buildTool == "meson") {
        build.arguments = {"compile", "-C", buildDir};
    }
    commands.push_back(build);

    if (install && spec.supportsInstall) {
        Command step{spec.buildTool, {}};
        if (spec.buildTool == "cmake") {
            step.arguments = {"--install", buildDir};
        } else if (spec.buildTool == "meson") {
            step.arguments = {"install", "-C", buildDir};
        } else {
            step.arguments = {"-C", buildDir, "install"};
        }
        commands.push_back(step);
    }
    return commands;
}

}  // namespace lightcpp::build
