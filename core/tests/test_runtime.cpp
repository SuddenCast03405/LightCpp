// 解析器 / 文档生成 / 插件宿主 / 插件内部纯函数测试。
#include <algorithm>
#include <cstdlib>
#include <string>

#include "lightcpp/docgen.h"
#include "lightcpp/fs.h"
#include "lightcpp/json.h"
#include "lightcpp/host.h"
#include "lightcpp/manifest.h"
#include "lightcpp/resolver.h"
#include "test_framework.h"

// 插件侧纯函数（仅用于单测，不经过动态库）
#include "../plugins/BuildSystemCore/BuildSystemFactory.h"
#include "../plugins/BuildSystemCore/BuildSystemSpec.h"
#include "../plugins/ClangCorePlugin/ClangCoreSpec.h"
#include "../plugins/GdbPlugin/GdbBreakpoints.h"
#include "../plugins/TestCorePlugin/TestCoreSpec.h"

using namespace lightcpp;

namespace {

bool hasStep(const Resolution& resolution, const std::string& id) {
    for (const InstallStep& step : resolution.steps) {
        if (step.id == id) {
            return true;
        }
    }
    return false;
}

const InstallStep* stepOf(const Resolution& resolution, const std::string& id) {
    for (const InstallStep& step : resolution.steps) {
        if (step.id == id) {
            return &step;
        }
    }
    return nullptr;
}

}  // namespace

LIGHTC_TEST(resolve_cmake_pulls_ninja) {
    const Repository repo = Repository::load(LIGHTC_ROOT);
    const Resolution r = resolve(repo, {"cmake"});
    CHECK_TRUE(r.ok());
    CHECK_TRUE(hasStep(r, "ninja"));
    const InstallStep* ninja = stepOf(r, "ninja");
    CHECK_TRUE(ninja != nullptr);
    CHECK_TRUE(ninja->origin.find("自动连带安装") != std::string::npos);
    // 依赖必须先于使用者出现
    std::size_t cmakeIndex = 0;
    std::size_t cppBaseIndex = 0;
    for (std::size_t i = 0; i < r.steps.size(); ++i) {
        if (r.steps[i].id == "cmake") {
            cmakeIndex = i;
        }
        if (r.steps[i].id == "cpp-base") {
            cppBaseIndex = i;
        }
    }
    CHECK_TRUE(cppBaseIndex < cmakeIndex);
}

LIGHTC_TEST(resolve_devcpp_pulls_make) {
    const Repository repo = Repository::load(LIGHTC_ROOT);
    const Resolution r = resolve(repo, {"devcpp"});
    CHECK_TRUE(r.ok());
    CHECK_TRUE(hasStep(r, "make"));
}

LIGHTC_TEST(resolve_unknown_plugin_reports_error) {
    const Repository repo = Repository::load(LIGHTC_ROOT);
    const Resolution r = resolve(repo, {"no-such-plugin"});
    CHECK_FALSE(r.ok());
}

LIGHTC_TEST(resolve_pack_notes_exclusive_group) {
    const Repository repo = Repository::load(LIGHTC_ROOT);
    const Resolution r = resolve(repo, {"cpp-extension-pack"});
    CHECK_TRUE(r.ok());
    CHECK_TRUE(!r.notes.empty());
    bool mentioned = false;
    for (const std::string& note : r.notes) {
        if (note.find("cpp-lsp") != std::string::npos) {
            mentioned = true;
        }
    }
    CHECK_TRUE(mentioned);
}

LIGHTC_TEST(docgen_is_up_to_date) {
    const Repository repo = Repository::load(LIGHTC_ROOT);
    std::string hint;
    // 文档由同一份实现生成，因此此处必然一致；若有人手改文档会立刻暴露。
    const bool fresh = docs::upToDate(repo, std::string(), &hint);
    if (!fresh) {
        std::cout << "        文档已过期: " << hint << "\n";
    }
    CHECK_TRUE(fresh);
}

LIGHTC_TEST(docgen_lists_all_components) {
    const Repository repo = Repository::load(LIGHTC_ROOT);
    const std::string text = docs::renderPlugins(repo);
    CHECK_TRUE(text.find("`cpp-base`") != std::string::npos);
    CHECK_TRUE(text.find("`cpp-extension-pack`") != std::string::npos);
    const std::string features = docs::renderFeatures(repo);
    CHECK_TRUE(features.find("build.exec.remote") != std::string::npos);
    CHECK_TRUE(features.find("合计 **189** 条功能") != std::string::npos);
}

LIGHTC_TEST(host_loads_built_plugins) {
    // 只有在构建器里编译过插件（提供了 LIGHTC_PLUGIN_DIR）时才可测。
#ifdef LIGHTC_PLUGIN_DIR
    const Repository repo = Repository::load(LIGHTC_ROOT);
    HostOptions options;
    options.pluginsDirectory = fs::join(repo.root, "plugins");
    options.buildOutputDirectory = LIGHTC_PLUGIN_DIR;
    options.requireNative = false;
    Host host(options);
    host.load(repo);
    const std::vector<std::string> systems = host.contributionIds("buildSystems");
    CHECK_TRUE(std::find(systems.begin(), systems.end(), "cmake") != systems.end());
    CHECK_TRUE(std::find(systems.begin(), systems.end(), "ninja") != systems.end());
    const std::vector<std::string> debuggers = host.contributionIds("debuggers");
    CHECK_TRUE(std::find(debuggers.begin(), debuggers.end(), "gdb") != debuggers.end());
    const std::vector<std::string> languages = host.contributionIds("languages");
    CHECK_TRUE(std::find(languages.begin(), languages.end(), "cpp") != languages.end());
    CHECK_TRUE(host.activatedCount() >= 8);
#endif
}

LIGHTC_TEST(host_strict_mode_fails_without_native) {
#ifdef LIGHTC_PLUGIN_DIR
    const Repository repo = Repository::load(LIGHTC_ROOT);
    HostOptions options;
    options.pluginsDirectory = fs::join(repo.root, "plugins");
    options.buildOutputDirectory = "/nonexistent/plugins";
    options.requireNative = true;
    Host host(options);
    const bool ok = host.load(repo);
    CHECK_FALSE(ok);
    CHECK_TRUE(!host.errors().empty());
#endif
}

LIGHTC_TEST(build_system_detect_and_plan) {
    using namespace lightcpp::build;
    const SystemSpec* cmake = specFor("cmake");
    CHECK_TRUE(cmake != nullptr);
    CHECK_FALSE(detect(*cmake, "/tmp"));

    const std::vector<Command> commands = plan(*cmake, "/src", "/src/build", 8, true);
    CHECK_EQ(commands.size(), static_cast<std::size_t>(3));
    CHECK_EQ(commands[0].program, std::string("cmake"));
    // 必须固定 Ninja 生成器并导出 compile_commands
    bool hasGenerator = false;
    bool hasCompileCommands = false;
    for (const std::string& arg : commands[0].arguments) {
        if (arg == "Ninja") {
            hasGenerator = true;
        }
        if (arg.find("CMAKE_EXPORT_COMPILE_COMMANDS=ON") != std::string::npos) {
            hasCompileCommands = true;
        }
    }
    CHECK_TRUE(hasGenerator);
    CHECK_TRUE(hasCompileCommands);

    const SystemSpec* ninja = specFor("ninja");
    CHECK_TRUE(ninja != nullptr);
    const std::vector<Command> ninjaCommands = plan(*ninja, "/src", "/src/build", 0, false);
    CHECK_EQ(ninjaCommands.size(), static_cast<std::size_t>(1));
    CHECK_EQ(ninjaCommands[0].program, std::string("ninja"));

    CHECK_TRUE(specFor("nope") == nullptr);
}

LIGHTC_TEST(gdb_breakpoint_commands) {
    using namespace lightcpp::gdb;
    Breakpoint bp;
    bp.file = "main.cpp";
    bp.line = 42;
    CHECK_EQ(breakpointCommand(bp), std::string("-break-insert main.cpp:42"));

    bp.condition = "i == 3";
    bp.hitCount = 5;
    bp.temporary = true;
    CHECK_EQ(breakpointCommand(bp),
             std::string("-break-insert -t -i 5 -c \"i == 3\" main.cpp:42"));

    bp.address = "0x400000";
    CHECK_TRUE(breakpointCommand(bp).find("*0x400000") != std::string::npos);
}

LIGHTC_TEST(gdb_parse_breakpoint_number) {
    using namespace lightcpp::gdb;
    CHECK_EQ(parseBreakpointNumber("^done,bkpt={number=\"3\",type=\"breakpoint\"}"), 3);
    CHECK_EQ(parseBreakpointNumber("^error,msg=\"no symbol\""), -1);
    CHECK_EQ(parseBreakpointNumber(""), -1);
}

LIGHTC_TEST(gdb_logpoint_expands_to_commands) {
    using namespace lightcpp::gdb;
    const std::vector<std::string> commands = logpointCommands(7, "x=%d\\n");
    CHECK_EQ(commands.size(), static_cast<std::size_t>(5));
    CHECK_EQ(commands[0], std::string("commands 7"));
    CHECK_TRUE(logpointCommands(0, "x").empty());
}

LIGHTC_TEST(clang_core_lsp_launch) {
    using namespace lightcpp::clangcore;
    const LspLaunch lsp = launch("", "/proj/build", true, "modernize-*");
    CHECK_EQ(lsp.program, std::string("clangd"));
    CHECK_TRUE(std::find(lsp.arguments.begin(), lsp.arguments.end(),
                         "--compile-commands-dir=/proj/build") != lsp.arguments.end());
    CHECK_TRUE(std::find(lsp.arguments.begin(), lsp.arguments.end(), "--clang-tidy") !=
               lsp.arguments.end());
    const auto formatArgs = clangcore::formatArguments(false, "a.cpp", "LLVM");
    CHECK_EQ(formatArgs[0], std::string("--style=LLVM"));
}

LIGHTC_TEST(test_core_parses_failure_location) {
    using namespace lightcpp::testcore;
    const TestCase tc = parseFailureLocation("tests/foo_test.cpp:128: expected 1, got 2");
    CHECK_EQ(tc.file, std::string("tests/foo_test.cpp"));
    CHECK_EQ(tc.line, 128);
    CHECK_EQ(tc.status, std::string("failed"));

    const TestCase bad = parseFailureLocation("no location here");
    CHECK_EQ(bad.line, 0);

    std::vector<TestCase> cases(3);
    cases[0].status = "passed";
    cases[1].status = "failed";
    cases[2].status = "skipped";
    const Summary summary = summarize(cases, false);
    CHECK_EQ(summary.passed, 1);
    CHECK_EQ(summary.failed, 1);
    CHECK_EQ(summary.skipped, 1);
    CHECK_FALSE(summary.green());
    CHECK_TRUE(summarize({}, false).green() == false);
}

// ---------------------------------------------------------------------------
// CLI 契约：这些断言保证脚本调用 CLI 时拿到的输出形状稳定
// ---------------------------------------------------------------------------

LIGHTC_TEST(cli_json_output_is_parseable) {
    // validate --json 必须能被自家 JSON 解析器吃下去，否则集成方无法消费
    const std::string path = fs::join("/tmp", "lightcpp-validate.json");
    const std::string command =
        std::string(LIGHTC_CLI) + " validate --json --root \"" + LIGHTC_ROOT + "\" > \"" + path + "\"";
    CHECK_EQ(0, std::system(command.c_str()));
    const json::Value report = json::parseFile(path);
    CHECK_TRUE(report.isObject());
    CHECK_EQ(47, static_cast<int>(report["plugins"].asNumber(0)));
    CHECK_EQ(189, static_cast<int>(report["features"].asNumber(0)));
    CHECK_EQ(0, static_cast<int>(report["errors"].asNumber(-1)));
    CHECK_TRUE(report["problems"].isArray());
}

LIGHTC_TEST(cli_version_flag) {
    const std::string command = std::string(LIGHTC_CLI) + " --version > /tmp/lightcpp-version.txt";
    CHECK_EQ(0, std::system(command.c_str()));
    std::string text;
    CHECK_TRUE(fs::readFile("/tmp/lightcpp-version.txt", &text));
    CHECK_TRUE(text.find("lightcpp-pm ") == 0);
}
