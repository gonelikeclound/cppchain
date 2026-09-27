// tests/wf —— 丰富工具库 + tool_search 能力检索（demo::richtools，见 examples/support/richtools.hpp）
#include <gtest/gtest.h>

#include <expected>
#include <string>
#include <vector>

#include "cppchain/tool/registry.hpp"
#include "cppchain/tool/tool.hpp"
#include "support/fakes.hpp"
#include "support/richtools.hpp"

using namespace cppchain::tool;
using namespace demo::fakes;      // Json（= cppchain::workflow::Json）/ FakeToolSinks
using namespace demo::richtools;

namespace {

// tool_search 的 handler 捕获注册表的 shared_ptr → 注册表必须 static 长活
 ToolRegistry& rich_reg() {
  static FakeToolSinks sinks;
  static RichSinks rsinks;
  static auto reg = make_rich_registry(sinks, rsinks);
  return *reg;
}

std::expected<Json,  ToolError> do_search(Json const& args) {
   ToolSpec const* ts = rich_reg().find("tool_search");
  EXPECT_NE(ts, nullptr);
   ToolContext ctx;
  return ts->handler(args, ctx);
}

std::vector<std::string> hit_names(Json const& hits) {
  std::vector<std::string> out;
  for (Json const& h : hits) out.push_back(h.value("name", ""));
  return out;
}

bool contains(std::vector<std::string> const& v, std::string_view s) {
  for (auto const& e : v)
    if (e == s) return true;
  return false;
}

}  // namespace

TEST(WfRichtools, registry_has_search_and_rich) {
  auto const& reg = rich_reg();
  EXPECT_TRUE(reg.find("tool_search"));  // 检索工具注册
  EXPECT_TRUE(reg.find("settings_get"));
  EXPECT_TRUE(reg.find("sms_send"));
  EXPECT_TRUE(reg.find("email_inbox"));
  EXPECT_TRUE(reg.find("todo_add"));
  EXPECT_TRUE(reg.find("fetch_price"));  // baseline 目录仍在
}

TEST(WfRichtools, tool_search_by_group) {
  auto r = do_search(Json{{"groups", Json::array({"sms"})}});
  ASSERT_TRUE(r);
  Json const out = *r;
  EXPECT_EQ(out["count"].get<std::size_t>(), 3u);
  auto names = hit_names(out["hits"]);
  EXPECT_TRUE(contains(names, "sms_send"));
  EXPECT_TRUE(contains(names, "sms_inbox"));
  EXPECT_TRUE(contains(names, "sms_reply"));
  EXPECT_FALSE(contains(names, "email_send"));  // group 过滤生效
}

TEST(WfRichtools, tool_search_keywords_and_semantics) {
  // keywords 需全部命中（AND）："发送"+"短信" 只命中 sms_send；email_send 描述无"短信"
  auto r = do_search(Json{{"keywords", Json::array({"发送", "短信"})}});
  ASSERT_TRUE(r);
  auto names = hit_names((*r)["hits"]);
  EXPECT_TRUE(contains(names, "sms_send"));
  EXPECT_FALSE(contains(names, "email_send"));
  // 关键词可跨 baseline 命中（取价目录）
  auto r2 = do_search(Json{{"keywords", Json::array({"价格"})}});
  ASSERT_TRUE(r2);
  EXPECT_TRUE(contains(hit_names((*r2)["hits"]), "fetch_price"));
}

TEST(WfRichtools, tool_search_group_and_keyword_combined) {
  auto r = do_search(Json{{"groups", Json::array({"email"})},
                          {"keywords", Json::array({"收"})}});
  ASSERT_TRUE(r);
  auto names = hit_names((*r)["hits"]);
  EXPECT_TRUE(contains(names, "email_inbox"));
  EXPECT_FALSE(contains(names, "email_send"));  // keywords AND：email_send 描述不含"收"
}

TEST(WfRichtools, tool_search_cap_one_to_three) {
  // 超过 3 → 报错（"不能太多了"）
  auto r = do_search(Json{{"groups", Json::array({"a", "b", "c", "d"})}});
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("3"), std::string::npos);
  auto rk = do_search(Json{{"keywords", Json::array({"a", "b", "c", "d"})}});
  ASSERT_FALSE(rk);
  // 两者全空 → 报错
  auto r2 = do_search(Json{{"groups", Json::array()}, {"keywords", Json::array()}});
  ASSERT_FALSE(r2);
}