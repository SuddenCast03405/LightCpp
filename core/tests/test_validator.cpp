// 校验器测试：用合成仓库构造每一类坏数据，确认错误码命中且不崩溃。
#include <algorithm>
#include <string>

#include "lightcpp/fs.h"
#include "lightcpp/manifest.h"
#include "lightcpp/validator.h"
#include "test_framework.h"

using namespace lightcpp;

namespace {

/// 在临时目录里搭一个最小仓库，用于验证单条规则。
class Fixture {
public:
    explicit Fixture(const std::string& name) {
        root_ = fs::join(fs::join("/tmp", "lightcpp-test"), name);
        fs::makeDirectories(fs::join(root_, "plugins"));
        fs::makeDirectories(fs::join(root_, "catalog"));
        writeFeatures(R"({"domains":[{"id":"build","label":"构建"}],
                          "sets":[{"id":"build.exec","domain":"build","label":"构建执行",
                                   "plugins":["a"],
                                   "features":[{"id":"build.exec.build","label":"构建",
                                                "status":"required"}]}]})");
        writeInvariants(R"({"rules":[]})");
    }

    void writePlugin(const std::string& dir, const std::string& id, const std::string& extra = "") {
        const std::string body =
            "{\"id\":\"" + id + "\",\"name\":\"" + id + "\",\"nameZh\":\"测试插件\",\"version\":\"0.1.0\"," +
            "\"publisher\":\"t\",\"kind\":\"plugin\",\"layer\":\"build-system\"," +
            "\"description\":\"用于测试的插件说明\"" + extra + "}";
        fs::writeFile(fs::join(fs::join(fs::join(root_, "plugins"), dir), "plugin.json"), body);
    }

    void writeRaw(const std::string& dir, const std::string& content) {
        fs::writeFile(fs::join(fs::join(fs::join(root_, "plugins"), dir), "plugin.json"), content);
    }

    void writeFeatures(const std::string& content) {
        fs::writeFile(fs::join(fs::join(root_, "catalog"), "features.json"), content);
    }

    void writeInvariants(const std::string& content) {
        fs::writeFile(fs::join(fs::join(root_, "catalog"), "invariants.json"), content);
    }

    Repository load() const { return Repository::load(root_); }

    const std::string& root() const { return root_; }

private:
    std::string root_;
};

std::vector<std::string> codes(const std::vector<Problem>& problems) {
    std::vector<std::string> out;
    for (const Problem& p : problems) {
        out.push_back(p.code);
    }
    return out;
}

bool hasCode(const std::vector<Problem>& problems, const std::string& code) {
    // 曾经误写成 std::find(codes(problems).begin(), codes(problems).end(), code)
    // != codes(problems).end()：三次调用各建一个临时 vector，begin() 与 end()
    // 来自不同对象，属未定义行为，clang 直接拒绝编译（CI 的 clang++ job 红）。
    const std::vector<std::string> found = codes(problems);
    return std::find(found.begin(), found.end(), code) != found.end();
}

}  // namespace

LIGHTC_TEST(validate_real_repository_is_clean) {
    const Repository repo = Repository::load(LIGHTC_ROOT);
    ValidateOptions options;
    options.strict = true;
    options.platform = "linux";
    const std::vector<Problem> problems = validate(repo, options);
    std::size_t errors = 0;
    for (const Problem& p : problems) {
        if (p.isError()) {
            ++errors;
            std::cout << "        意外的错误: " << p.format() << "\n";
        }
    }
    CHECK_EQ(errors, static_cast<std::size_t>(0));
    CHECK_EQ(repo.plugins.size(), static_cast<std::size_t>(47));
    CHECK_EQ(repo.catalog.sets.size(), static_cast<std::size_t>(40));
    CHECK_EQ(repo.catalog.featureCount(), static_cast<std::size_t>(189));
}

LIGHTC_TEST(validate_requires_build_core_for_build_system) {
    // 不变量要求所有 build-system 依赖 cpp-base + build-core，缺了就报 E103。
    Fixture fixture("missing_build_core");
    fixture.writePlugin("a", "a");
    fixture.writeInvariants(R"({"rules":[
        {"id":"build-system-depends-core","type":"layerDependency","layer":"build-system",
         "on":["cpp-base","build-core"]}]})");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E103"));
}

LIGHTC_TEST(validate_detects_dependency_cycle) {
    Fixture fixture("cycle");
    fixture.writePlugin("a", "a", R"(,"dependencies":{"b":"*"})");
    fixture.writePlugin("b", "b", R"(,"dependencies":{"a":"*"})");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E020"));
}

LIGHTC_TEST(validate_rejects_pack_with_contributes) {
    Fixture fixture("pack_contributes");
    fixture.writeRaw("p", R"({"id":"p","name":"p","nameZh":"包","version":"0.1.0",
        "publisher":"t","kind":"pack","layer":"pack","description":"扩展包不该有业务逻辑",
        "dependencies":{"a":"*"},"contributes":{"commands":[{"id":"x","title":"x"}]}})");
    fixture.writePlugin("a", "a");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E017"));
}

LIGHTC_TEST(validate_layer_escape_is_rejected) {
    // 把调试器插件写成 core 层，曾经能"冒充"核心层通过校验。
    Fixture fixture("layer_escape");
    fixture.writeRaw("fake-core", R"({"id":"fake-core","name":"f","nameZh":"假的","version":"0.1.0",
        "publisher":"t","kind":"plugin","layer":"core","description":"冒充核心层的插件"})");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E028"));
}

LIGHTC_TEST(validate_unknown_field_is_reported_with_hint) {
    Fixture fixture("typo_field");
    fixture.writePlugin("a", "a", R"(,"dependecies":{"b":"*"})");
    fixture.writePlugin("b", "b");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "LOAD"));
    bool hinted = false;
    for (const Problem& p : problems) {
        if (p.message.find("dependecies") != std::string::npos &&
            p.message.find("dependencies") != std::string::npos) {
            hinted = true;
        }
    }
    CHECK_TRUE(hinted);
}

LIGHTC_TEST(validate_short_description) {
    Fixture fixture("short_description");
    fixture.writeRaw("a", R"({"id":"a","name":"a","nameZh":"a","version":"0.1.0","publisher":"t",
        "kind":"plugin","layer":"tooling","description":"短"})");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E027"));
}

LIGHTC_TEST(validate_dependency_type_confusion) {
    // dependencies 写成数组：旧实现直接 AttributeError，这里应当报错而不是崩溃。
    Fixture fixture("deps_array");
    fixture.writePlugin("a", "a", R"(,"dependencies":["b"])");
    fixture.writePlugin("b", "b");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "LOAD"));
}

LIGHTC_TEST(validate_capability_set_as_string) {
    // capabilitySets 写成字符串：旧实现按字符遍历并 KeyError。
    Fixture fixture("sets_string");
    fixture.writePlugin("a", "a", R"(,"capabilitySets":"build.exec")");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "LOAD"));
}

LIGHTC_TEST(validate_uncovered_required_feature) {
    Fixture fixture("uncovered");
    fixture.writePlugin("a", "a");
    fixture.writeFeatures(R"({"domains":[{"id":"build","label":"构建"}],
        "sets":[{"id":"build.exec","domain":"build","label":"构建执行","plugins":[],
                 "features":[{"id":"build.exec.build","label":"构建","status":"required"}]}]})");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E023"));
}

LIGHTC_TEST(validate_duplicate_feature_set) {
    Fixture fixture("dup_set");
    fixture.writePlugin("a", "a", R"(,"capabilitySets":["build.exec"])");
    fixture.writeFeatures(R"({"domains":[{"id":"build","label":"构建"}],
        "sets":[{"id":"build.exec","domain":"build","label":"构建执行","plugins":["a"],
                 "features":[{"id":"f1","label":"x","status":"required"}]},
                {"id":"build.exec","domain":"build","label":"构建执行","plugins":["a"],
                 "features":[{"id":"f2","label":"y","status":"required"}]}]})");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E029"));
}

LIGHTC_TEST(validate_except_points_to_missing_plugin) {
    Fixture fixture("bad_except");
    fixture.writePlugin("a", "a", R"(,"capabilitySets":["build.exec"])");
    fixture.writeFeatures(R"({"domains":[{"id":"build","label":"构建"}],
        "sets":[{"id":"build.exec","domain":"build","label":"构建执行","plugins":["a"],
                 "features":[{"id":"f1","label":"x","status":"required","except":["ghost"]}]}]})");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E032"));
}

LIGHTC_TEST(validate_broken_json_is_reported_not_crash) {
    Fixture fixture("broken_json");
    fixture.writeRaw("a", "{not json");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "LOAD"));
}

LIGHTC_TEST(validate_invariant_auto_install) {
    Fixture fixture("autoinstall");
    fixture.writePlugin("cmake", "cmake", R"(,"recommends":{"ninja":{"version":"^0.1.0"}},
        "dependencies":{"cpp-base":"*","build-core":"*"})");
    fixture.writePlugin("ninja", "ninja");
    fixture.writePlugin("cpp-base", "cpp-base");
    fixture.writePlugin("build-core", "build-core");
    fixture.writeInvariants(R"({"rules":[{"id":"cmake-pulls-ninja","type":"autoInstall",
        "plugin":"cmake","target":"ninja"}]})");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E101"));
}

LIGHTC_TEST(validate_invariant_unknown_type) {
    Fixture fixture("unknown_invariant");
    fixture.writePlugin("a", "a");
    fixture.writeInvariants(R"({"rules":[{"id":"x","type":"nonsense"}]})");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E105"));
}

LIGHTC_TEST(validate_lsp_exclusive_group) {
    Fixture fixture("lsp_group");
    fixture.writePlugin("clang-core", "clang-core",
                        R"(,"contributes":{"lsp":{"engine":"clangd","exclusiveGroup":"cpp-lsp"}})");
    fixture.writePlugin("ccls", "ccls",
                        R"(,"contributes":{"lsp":{"engine":"clangd","exclusiveGroup":"cpp-lsp"}})");
    const std::vector<Problem> problems = validate(fixture.load());
    // 同组两个成员 engine 重名 -> E025
    CHECK_TRUE(hasCode(problems, "E025"));
}

LIGHTC_TEST(validate_version_mismatch_and_bad_constraint) {
    Fixture fixture("version_mismatch");
    fixture.writePlugin("a", "a", R"(,"dependencies":{"b":"^2.0.0","c":"abc"})");
    fixture.writePlugin("b", "b");
    fixture.writePlugin("c", "c");
    const std::vector<Problem> problems = validate(fixture.load());
    CHECK_TRUE(hasCode(problems, "E011"));
    CHECK_TRUE(hasCode(problems, "E012"));
}
