// wfr-㊵ 测试：工具类别（`ToolSpec::category`）与词表。
//
// 类别是"历史精简按用途过滤"的依据（如丢掉联网取数的原始长文）。三条要守住的性质：
//   1. 词表封闭，`is_known_category` 是 validator 在编译期判字面量的依据；
//   2. 默认值是 `general`（未归类 → 不匹配任何显式类别）；
//   3. **不进 to_openai()** —— 类别是库内分类，不是 function schema 的一部分；真端点对
//      未知字段敏感（tool.cpp 头注释记过一次 schema 导致真机失败的教训）。
#include <gtest/gtest.h>

#include <string>

#include "cppchain/tool/calculator.hpp"
#include "cppchain/tool/datetime.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/tool/tool.hpp"

using namespace cppchain;
using namespace cppchain::tool;

TEST(ToolCategory, vocabulary_is_closed) {
  EXPECT_TRUE(is_known_category("retrieval"));
  EXPECT_TRUE(is_known_category("compute"));
  EXPECT_TRUE(is_known_category("action"));
  EXPECT_TRUE(is_known_category("model"));
  EXPECT_TRUE(is_known_category("time"));
  EXPECT_TRUE(is_known_category("general"));
  EXPECT_FALSE(is_known_category("retrival"));   // 拼错
  EXPECT_FALSE(is_known_category(""));           // 空
  EXPECT_FALSE(is_known_category("retrieval ")); // 带空格
}

TEST(ToolCategory, default_is_general_and_chainable) {
  ToolSpec s = make_tool("t", "d", nlohmann::json::object(),
                         [](nlohmann::json const&, ToolContext&) {
                           return std::expected<nlohmann::json, ToolError>{nlohmann::json::object()};
                         });
  EXPECT_EQ(s.category, "general");  // 不显式设置 → general（不匹配任何显式类别）
  s.with_category("retrieval");
  EXPECT_EQ(s.category, "retrieval");
}

TEST(ToolCategory, category_never_leaks_into_the_openai_schema) {
  ToolSpec s = make_tool("t", "d", nlohmann::json::object(),
                         [](nlohmann::json const&, ToolContext&) {
                           return std::expected<nlohmann::json, ToolError>{nlohmann::json::object()};
                         }).with_category("retrieval");
  auto const j = s.to_openai();
  ASSERT_TRUE(j.contains("function"));
  EXPECT_FALSE(j["function"].contains("category"));
  EXPECT_FALSE(j.contains("category"));
  EXPECT_EQ(j.dump().find("category"), std::string::npos) << "类别不能出现在出站 schema 里";
}

TEST(ToolCategory, builtin_factories_declare_their_category) {
  // 内置工具的归属是"按类别过滤"的真实依据（hist_rules 的 aggressive preset 会丢掉 retrieval）
  EXPECT_EQ(make_datetime_tool().category, "time");
  EXPECT_EQ(make_calculator_tool().category, "compute");

  ToolRegistry reg;
  reg.add(make_datetime_tool());
  reg.add(make_calculator_tool());
  ASSERT_NE(reg.find("get_datetime"), nullptr);
  EXPECT_EQ(reg.find("get_datetime")->category, "time");
  EXPECT_EQ(reg.find("calculator")->category, "compute");
}
