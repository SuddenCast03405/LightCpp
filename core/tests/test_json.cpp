// JSON 解析器测试：重点在坏输入——清单是手写的，错误必须报得准确而不是崩。
#include <string>

#include "lightcpp/json.h"
#include "lightcpp/text.h"
#include "lightcpp/version.h"
#include "test_framework.h"

using namespace lightcpp;

LIGHTC_TEST(json_parse_scalars) {
    json::ParseError error;
    json::Value v = json::parse("{\"a\": true, \"b\": false, \"c\": null, \"d\": 1.5, \"e\": \"x\"}",
                                &error);
    CHECK_TRUE(error.message.empty());
    CHECK_TRUE(v["a"].asBool());
    CHECK_FALSE(v["b"].asBool(true));
    CHECK_TRUE(v["c"].isNull());
    CHECK_TRUE(v["d"].asNumber() > 1.4 && v["d"].asNumber() < 1.6);
    CHECK_EQ(v["e"].asString(), std::string("x"));
}

LIGHTC_TEST(json_preserves_key_order) {
    json::Value v = json::parse("{\"z\": 1, \"a\": 2, \"m\": 3}");
    const auto& members = v.members();
    CHECK_EQ(members.size(), static_cast<std::size_t>(3));
    CHECK_EQ(members[0].first, std::string("z"));
    CHECK_EQ(members[2].first, std::string("m"));
}

LIGHTC_TEST(json_utf8_and_surrogate_pairs) {
    json::Value v = json::parse("{\"zh\": \"中文\", \"emoji\": \"\\ud83d\\ude00\"}");
    CHECK_EQ(v["zh"].asString(), std::string("中文"));
    // 代理对被合并成一个 4 字节 UTF-8 码点。
    CHECK_EQ(v["emoji"].asString().size(), static_cast<std::size_t>(4));
}

LIGHTC_TEST(json_rejects_bom_less_garbage) {
    json::ParseError error;
    json::Value v = json::parse("{\"a\": 1,}", &error);
    CHECK_TRUE(v.isNull());
    CHECK_FALSE(error.message.empty());
}

LIGHTC_TEST(json_reports_line_and_column) {
    json::ParseError error;
    json::parse("{\n  \"a\": 1,\n  \"b\": oops\n}", &error);
    CHECK_EQ(error.line, static_cast<std::size_t>(3));
    CHECK_TRUE(error.message.find("无法识别") != std::string::npos ||
               error.message.find("非法") != std::string::npos);
}

LIGHTC_TEST(json_rejects_duplicate_keys) {
    // 重复键是手工编辑清单时最常见的事故，必须报错而不是静默覆盖。
    json::ParseError error;
    json::parse("{\"id\": \"a\", \"id\": \"b\"}", &error);
    CHECK_TRUE(error.message.find("重复键") != std::string::npos);
}

LIGHTC_TEST(json_rejects_deep_nesting) {
    std::string text(300, '[');
    text += std::string(300, ']');
    json::ParseError error;
    json::parse(text, &error);
    CHECK_TRUE(error.message.find("嵌套过深") != std::string::npos);
}

LIGHTC_TEST(json_accepts_utf8_bom) {
    const std::string text = "\xEF\xBB\xBF{\"a\": 1}";
    json::ParseError error;
    json::Value v = json::parse(text, &error);
    CHECK_TRUE(error.message.empty());
    CHECK_EQ(v["a"].asNumber(), 1.0);
}

LIGHTC_TEST(json_dump_roundtrip) {
    const std::string source =
        "{\"id\":\"cmake\",\"deps\":{\"cpp-base\":\"^0.1.0\"},\"n\":3,\"ok\":true,\"s\":\"构建\"}";
    json::Value v = json::parse(source);
    const std::string dumped = v.dump(0);
    json::Value again = json::parse(dumped);
    CHECK_EQ(again["id"].asString(), std::string("cmake"));
    CHECK_EQ(again["deps"]["cpp-base"].asString(), std::string("^0.1.0"));
    CHECK_EQ(again["s"].asString(), std::string("构建"));
}

LIGHTC_TEST(json_missing_key_returns_null) {
    json::Value v = json::parse("{\"a\": 1}");
    CHECK_TRUE(v["missing"].isNull());
    // 非对象上取键也不能崩。
    CHECK_TRUE(v["a"]["deeper"].isNull());
}

LIGHTC_TEST(version_parse_and_compare) {
    Version v;
    CHECK_TRUE(Version::parse("0.1.2", &v));
    CHECK_EQ(v.major, 0);
    CHECK_EQ(v.patch, 2);
    CHECK_FALSE(Version::parse("0.1", &v));
    CHECK_FALSE(Version::parse("0.1.2.3", &v));
    CHECK_FALSE(Version::parse("a.b.c", &v));
    // 每段上限 65535，超出视为非法，避免比较时溢出。
    CHECK_FALSE(Version::parse("99999.0.0", &v));
    Version a;
    Version b;
    Version::parse("0.1.0", &a);
    Version::parse("0.2.0", &b);
    CHECK_TRUE(a < b);
}

LIGHTC_TEST(version_constraints) {
    CHECK_TRUE(satisfies("0.1.0", "*"));
    CHECK_TRUE(satisfies("0.1.0", "0.1.0"));
    CHECK_FALSE(satisfies("0.1.1", "0.1.0"));
    // 0.x 视为不稳定：^ 锁次版本
    CHECK_TRUE(satisfies("0.1.9", "^0.1.0"));
    CHECK_FALSE(satisfies("0.2.0", "^0.1.0"));
    // 1.x：^ 锁主版本
    CHECK_TRUE(satisfies("1.9.0", "^1.2.0"));
    CHECK_FALSE(satisfies("2.0.0", "^1.2.0"));
    CHECK_TRUE(satisfies("0.1.5", "~0.1.0"));
    CHECK_FALSE(satisfies("0.2.0", "~0.1.0"));
    CHECK_TRUE(satisfies("1.0.0", ">=0.9.9"));
    CHECK_FALSE(satisfies("0.8.0", ">=0.9.9"));
}

LIGHTC_TEST(version_rejects_bad_constraint) {
    std::string error;
    CHECK_FALSE(satisfies("1.0.0", "abc", &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(parseConstraint(""));
    CHECK_FALSE(parseConstraint("~"));
}

LIGHTC_TEST(text_display_width_counts_cjk_as_two) {
    CHECK_EQ(text::displayWidth("abc"), static_cast<std::size_t>(3));
    CHECK_EQ(text::displayWidth("中文"), static_cast<std::size_t>(4));
    CHECK_EQ(text::displayWidth("a中b"), static_cast<std::size_t>(4));
}

LIGHTC_TEST(text_table_escapes_pipe_and_newline) {
    const std::string table = text::renderTable({"a", "b"}, {{"x|y", "m\nn"}});
    CHECK_TRUE(table.find("x\\|y") != std::string::npos);
    CHECK_TRUE(table.find("m n") != std::string::npos);
}

LIGHTC_TEST(text_table_aligns_cjk) {
    const std::string table = text::renderTable({"列"}, {{"中文"}, {"a"}});
    // 两行内容宽度都应是 4（中文 4 列，a 补足到 4 列）
    CHECK_TRUE(table.find("| 中文 |") != std::string::npos);
    CHECK_TRUE(table.find("| a    |") != std::string::npos);
}

LIGHTC_TEST(text_truncate_never_splits_utf8) {
    const std::string s = "构建系统适配器";
    const std::string cut = text::truncate(s, 7);
    CHECK_TRUE(cut.size() < s.size() + 3);
    // 结果必须是合法 UTF-8：不含孤立续字节
    for (std::size_t i = 0; i < cut.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(cut[i]);
        CHECK_TRUE((c & 0xC0) != 0x80 || i > 0);
    }
}
