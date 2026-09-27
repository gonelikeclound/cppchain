// tests/wf —— 受限表达式（workflow/expr）
#include <gtest/gtest.h>

#include <string>

#include "cppchain/workflow/expr.hpp"

using namespace cppchain::workflow;

namespace {

Json state_with(Json results, std::string last) {
  return Json{{"results", std::move(results)}, {"_last_exec", std::move(last)}};
}

}  // namespace

TEST(WfExpr, literal_truthiness) {
  EXPECT_TRUE(Expr::parse("true")->eval(Json::object()));
  EXPECT_FALSE(Expr::parse("false")->eval(Json::object()));
  EXPECT_FALSE(Expr::parse("null")->eval(Json::object()));
  EXPECT_TRUE(Expr::parse("1")->eval(Json::object()));
  EXPECT_FALSE(Expr::parse("0")->eval(Json::object()));
  EXPECT_FALSE(Expr::parse("\"\"")->eval(Json::object()));
  EXPECT_TRUE(Expr::parse("\"x\"")->eval(Json::object()));
}

TEST(WfExpr, precedence_and_parens) {
  // ! 高于 &&，&& 高于 ||
  EXPECT_TRUE(Expr::parse("true || false && false")->eval(Json::object()));
  EXPECT_FALSE(Expr::parse("(false || true) && false")->eval(Json::object()));
  EXPECT_TRUE(Expr::parse("!false")->eval(Json::object()));
  EXPECT_TRUE(Expr::parse("!(false || true) == false")->eval(Json::object()));
}

TEST(WfExpr, comparisons_loose) {
  EXPECT_TRUE(Expr::parse("5 > 3")->eval(Json::object()));
  EXPECT_FALSE(Expr::parse("3 >= 5")->eval(Json::object()));
  EXPECT_TRUE(Expr::parse("\"apple\" < \"banana\"")->eval(Json::object()));
  // 数字字符串宽松比较（已解析过的字符串 == 数字）
  EXPECT_TRUE(Expr::parse("\"800\" == 800")->eval(Json::object()));
  EXPECT_TRUE(Expr::parse("\"799\" < 800")->eval(Json::object()));
  EXPECT_TRUE(Expr::parse("1 != 2")->eval(Json::object()));
  // 不可比 → false（不抛）
  auto const null_cmp = Expr::parse("\"abc\" > 5");
  ASSERT_TRUE(null_cmp);
  EXPECT_FALSE(null_cmp->eval(Json::object()));
}

TEST(WfExpr, refs_resolve_in_state) {
  Json state = state_with(Json{{"fetch", Json{{"price", 799}}}} , "fetch");
  auto e = Expr::parse("$_.result.price != null && $_.result.price < 800");
  ASSERT_TRUE(e);
  EXPECT_TRUE(e->eval(state));
  state["results"]["fetch"]["price"] = 850;
  EXPECT_FALSE(e->eval(state));

  // 点名帧
  auto n = Expr::parse("$fetch.result.in_stock == true");
  ASSERT_TRUE(n);
  Json s2 = state_with(Json{{"fetch", Json{{"in_stock", true}}}}, "fetch");
  EXPECT_TRUE(n->eval(s2));
}

TEST(WfExpr, contains_and_in) {
  EXPECT_TRUE(Expr::parse("\"已到达\" contains \"到达\"")->eval(Json::object()));
  EXPECT_FALSE(Expr::parse("\"已到达\" contains \"未达\"")->eval(Json::object()));
  // in：右侧为数组（来自 node 输出的动态数据）
  Json st = state_with(Json{{"lst", Json{{"items", Json::array({"a", "b", "c"})}}}}, "lst");
  auto i = Expr::parse("\"b\" in $lst.result.items");
  ASSERT_TRUE(i);
  EXPECT_TRUE(i->eval(st));
  EXPECT_FALSE(Expr::parse("\"z\" in $lst.result.items")->eval(st));
  // 字段缺失 → resolve 到 null → 宽松 in 直接 false（不抛）
  Json st2 = state_with(Json{{"lst", Json{{"items2", Json::array({"a"})}}}}, "lst");
  EXPECT_FALSE(Expr::parse("\"b\" in $_.result.items")->eval(st2));
}

TEST(WfExpr, parse_errors) {
  EXPECT_FALSE(Expr::parse("len(x)").has_value());   // 函数调用 → 错
  EXPECT_FALSE(Expr::parse("[1, 2]").has_value());   // 数组 → 错
  EXPECT_FALSE(Expr::parse("x.length").has_value()); // 裸标识符/成员访问 → 错
  EXPECT_FALSE(Expr::parse("a &&").has_value());     // 悬空 && → 错
  EXPECT_FALSE(Expr::parse("a == b == c").has_value());  // 一次比较一个运算符
  EXPECT_FALSE(Expr::parse("$fetch.result").has_value());  // 缺少 .字段
  EXPECT_FALSE(Expr::parse("").has_value());         // 空表达式
}

TEST(WfExpr, refs_collection) {
  auto e = Expr::parse("$a.result.x > 1 && $b.result.y < 2 || @config.t == \"1\"");
  ASSERT_TRUE(e);
  ASSERT_EQ(e->refs().size(), 3u);
}