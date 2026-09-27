// tests/wf —— 数据流引用解析（workflow/refs）
#include <gtest/gtest.h>

#include <string>

#include "cppchain/workflow/refs.hpp"

using namespace cppchain::workflow;

namespace {

Json state = Json{{"config", Json{{"product_id", "P1001"}}},
                  {"results", Json{{"fetch", Json{{"price", 799}, {"title", "耳机"}}}}},
                  {"_last_exec", "fetch"}};

}  // namespace

TEST(WfRefs, parse_full_string) {
  auto last = parse_ref("$_.result.price");
  ASSERT_TRUE(last);
  EXPECT_EQ(last->kind, RefKind::LastResult);
  EXPECT_EQ(last->field, "price");

  auto named = parse_ref("$fetch.result.price");
  ASSERT_TRUE(named);
  EXPECT_EQ(named->kind, RefKind::NamedResult);
  EXPECT_EQ(named->node, "fetch");

  auto cfg = parse_ref("@config.product_id");
  ASSERT_TRUE(cfg);
  EXPECT_EQ(cfg->kind, RefKind::Config);

  // 非整串引用 → 不是"整值引用"
  EXPECT_FALSE(parse_ref("价格：$fetch.result.price").has_value());
  EXPECT_FALSE(parse_ref("plain text").has_value());
  EXPECT_FALSE(parse_ref("$fetch.price").has_value());  // 少了 .result.
}

TEST(WfRefs, resolve_against_state) {
  EXPECT_EQ(resolve_ref(*parse_ref("$fetch.result.price"), state).get<int>(), 799);
  EXPECT_EQ(resolve_ref(*parse_ref("$_.result.title"), state).get<std::string>(), "耳机");
  EXPECT_EQ(resolve_ref(*parse_ref("@config.product_id"), state).get<std::string>(), "P1001");
  // 缺失 → null（宽松）
  EXPECT_TRUE(resolve_ref(*parse_ref("$fetch.result.nope"), state).is_null());
  EXPECT_TRUE(resolve_ref(*parse_ref("$ghost.result.price"), state).is_null());
}

TEST(WfRefs, parameter_resolution) {
  // 整值引用：保留 JSON 类型（数字不变字符串）
  EXPECT_EQ(resolve_parameter(Json("$fetch.result.price"), state).get<int>(), 799);
  // 模板替换：字符串不带引号；数字用 dump()
  auto tmpl = resolve_parameter(Json("价格：$fetch.result.price 元"), state);
  ASSERT_TRUE(tmpl.is_string());
  EXPECT_EQ(tmpl.get<std::string>(), "价格：799 元");
  // 无引用原样
  EXPECT_EQ(resolve_parameter(Json("hello"), state).get<std::string>(), "hello");
  // 非字符串原样
  EXPECT_EQ(resolve_parameter(Json(42), state).get<int>(), 42);
  // 损坏片段：原样保留（validator 编译期兜住）
  auto bad = resolve_parameter(Json("$broken"), state);
  ASSERT_TRUE(bad.is_string());
  EXPECT_EQ(bad.get<std::string>(), "$broken");
}

TEST(WfRefs, syntax_problem_detection) {
  EXPECT_FALSE(ref_syntax_problem("$ok.result.x && @config.k").has_value());
  EXPECT_TRUE(ref_syntax_problem("$broken").has_value());
  EXPECT_TRUE(ref_syntax_problem("@foo").has_value());  // @ 后不是 config.
  EXPECT_TRUE(ref_syntax_problem("$fetch.price").has_value());
}