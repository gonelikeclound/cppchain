// M3 测试：calculator 安全计算器工具（递归下降解析 + 求值）
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cppchain/tool/calculator.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;

namespace {

ToolContext make_ctx() {
  ToolContext c;
  c.tool_call_id = "call_calc";
  return c;
}

ToolSpec make_spec() { return make_calculator_tool(); }

}  // namespace

TEST(Calculator, arithmetic_and_precedence) {
  EXPECT_EQ(*calculate("1+2*3"), 7.0);
  EXPECT_EQ(*calculate("(1+2)*3"), 9.0);
  EXPECT_EQ(*calculate("10/4"), 2.5);
  EXPECT_EQ(*calculate("10%3"), 1.0);
  EXPECT_DOUBLE_EQ(*calculate("1+2-3*4/2"), -3.0);
  EXPECT_EQ(*calculate("2 + 2 * 2"), 6.0);
}

TEST(Calculator, power_right_assoc_and_unary_binds_loose) {
  EXPECT_EQ(*calculate("2^10"), 1024.0);
  EXPECT_EQ(*calculate("2**3"), 8.0);
  EXPECT_EQ(*calculate("-2^2"), -4.0);   // Python 语义：-(2^2) = -4
  EXPECT_EQ(*calculate("2^3^2"), 512.0); // 右结合：2^(3^2)
  EXPECT_EQ(*calculate("2*-3"), -6.0);
  EXPECT_DOUBLE_EQ(*calculate("2^-3"), 0.125);
  EXPECT_EQ(*calculate("-(2+1)"), -3.0);
}

TEST(Calculator, constants) {
  EXPECT_NEAR(*calculate("pi"), 3.141592653589793, 1e-12);
  EXPECT_NEAR(*calculate("e"), 2.718281828459045, 1e-12);
  EXPECT_DOUBLE_EQ(*calculate("pi*2/2"), *calculate("pi"));
}

TEST(Calculator, unary_and_binary_functions) {
  EXPECT_EQ(*calculate("sqrt(16)"), 4.0);
  EXPECT_EQ(*calculate("abs(-3.5)"), 3.5);
  EXPECT_EQ(*calculate("pow(2,10)"), 1024.0);
  EXPECT_EQ(*calculate("min(3,7)"), 3.0);
  EXPECT_EQ(*calculate("max(3,7)"), 7.0);
  EXPECT_NEAR(*calculate("log(e)"), 1.0, 1e-12);
  EXPECT_EQ(*calculate("log10(1000)"), 3.0);
  EXPECT_EQ(*calculate("round(2.5)"), 3.0);
  EXPECT_EQ(*calculate("floor(2.9)"), 2.0);
  EXPECT_EQ(*calculate("ceil(2.1)"), 3.0);
  EXPECT_NEAR(*calculate("sin(pi/2)"), 1.0, 1e-9);
  EXPECT_NEAR(*calculate("exp(0)"), 1.0, 1e-12);
  EXPECT_EQ(*calculate("max(min(1,5),3)"), 3.0);  // 嵌套调用
}

TEST(Calculator, scientific_notation_and_whitespace) {
  EXPECT_EQ(*calculate("1e6"), 1000000.0);
  EXPECT_DOUBLE_EQ(*calculate("2.5e-3"), 0.0025);
  EXPECT_EQ(*calculate("1 + 2 * ( 3 )"), 7.0);
  EXPECT_EQ(*calculate("  42  "), 42.0);
}

TEST(Calculator, malformed_expressions_are_errors) {
  EXPECT_FALSE(calculate("").has_value());
  EXPECT_FALSE(calculate("   ").has_value());
  EXPECT_FALSE(calculate("1+").has_value());
  EXPECT_FALSE(calculate("(1").has_value());
  EXPECT_FALSE(calculate("1/0").has_value());
  EXPECT_FALSE(calculate("1%0").has_value());
  EXPECT_FALSE(calculate("2 + * 3").has_value());
  EXPECT_FALSE(calculate("2 3").has_value());
  EXPECT_FALSE(calculate("2)(3").has_value());
  EXPECT_FALSE(calculate("unknown(2)").has_value());
  EXPECT_FALSE(calculate("sqrt(-1)").has_value());
  EXPECT_FALSE(calculate("log(0)").has_value());
}

TEST(Calculator, error_messages_are_actionable) {
  auto r = calculate("1/0");
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().find("除"), std::string::npos);

  r = calculate("foo(2)");
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().find("未知函数"), std::string::npos);

  r = calculate("1+");
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().find("位置"), std::string::npos);

  r = calculate(std::string(300, '1'));  // 超长表达式被上限拦截
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().find("过长"), std::string::npos);
}

TEST(Calculator, format_number) {
  EXPECT_EQ(format_number(2.0), "2");
  EXPECT_EQ(format_number(-3.0), "-3");
  EXPECT_EQ(format_number(0.5), "0.5");
  EXPECT_EQ(format_number(123456789012345.0), "123456789012345");
  EXPECT_EQ(format_number(-0.0), "0");
}

TEST(Calculator, handler_round_trip) {
  auto spec = make_spec();
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"expression", "6*7"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("expression").get<std::string>(), "6*7");
  EXPECT_EQ(r->at("result").get<std::string>(), "42");
}

TEST(Calculator, handler_failure_is_fed_back) {
  auto spec = make_spec();
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"expression", "1/0"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("除"), std::string::npos);
}

TEST(Calculator, missing_expression_is_arg_error) {
  auto spec = make_spec();
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("expression"), std::string::npos);
}