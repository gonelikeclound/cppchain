// M3 测试：工具检索（keyword 评分 / selector 行为）
#include <gtest/gtest.h>

#include <algorithm>

#include "cppchain/tool/selector.hpp"

using namespace cppchain;
using namespace cppchain::tool;

namespace {

ToolSpec simple(std::string name, std::string desc) {
  return make_tool(std::move(name), std::move(desc), nlohmann::json::object(),
                   [](nlohmann::json const&, ToolContext&) {
                     return std::expected<nlohmann::json, ToolError>{nlohmann::json("ok")};
                   });
}

void fill(ToolRegistry& r) {
  r.add(simple("get_weather", "查询指定城市的当前天气"));
  r.add(simple("get_stock_price", "查询股票价格"));
  r.add(simple("send_email", "发送邮件给指定收件人"));
  r.add(simple("search_web", "在互联网上搜索信息"));
  r.add(simple("read_file", "读取本地文件内容"));
}

bool has(std::vector<std::string> const& v, std::string const& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST(Keyword, tokenize_splits_ascii_and_cjk_bigrams) {
  auto const t = KeywordSelector::tokenize("get_weather 查询天气");
  EXPECT_TRUE(has(t, "get"));
  EXPECT_TRUE(has(t, "weather"));
  // CJK 用 2-gram：天气 是一个 token，而不是 天 + 气
  EXPECT_TRUE(has(t, "查询"));
  EXPECT_TRUE(has(t, "天气"));
  EXPECT_FALSE(has(t, "天"));
  EXPECT_FALSE(has(t, "g"));  // 单字母不成 token
}

TEST(Keyword, tokenize_drops_cjk_stop_chars) {
  auto const t = KeywordSelector::tokenize("天气的查询");
  EXPECT_TRUE(has(t, "天气"));
  EXPECT_FALSE(has(t, "的"));      // 停用字被剔除
  EXPECT_FALSE(has(t, "气的"));    // 停用字不参与组 gram
}

TEST(Keyword, searchable_description_includes_parameters) {
  SchemaBuilder sb;
  sb.prop("city", "string", "要查询的城市名称");
  auto const spec = make_tool("weather_api", "获取气象数据", sb.build(),
                              [](nlohmann::json const&, ToolContext&) {
                                return std::expected<nlohmann::json, ToolError>{
                                    nlohmann::json("ok")};
                              });
  auto const text = KeywordSelector::searchable_description(spec);
  EXPECT_NE(text.find("城市名称"), std::string::npos);  // 参数描述进入可检索文本
  EXPECT_NE(text.find("city"), std::string::npos);
}

TEST(Keyword, score_weights_name_higher_than_description) {
  KeywordSelector sel;
  ToolSpec const name_hit = simple("weather_tool", "无关描述");
  ToolSpec const desc_hit = simple("unrelated", "get weather info");

  double const a = sel.score(name_hit, "weather");
  double const b = sel.score(desc_hit, "weather");
  EXPECT_GT(a, b);
  EXPECT_DOUBLE_EQ(a, 3.0);  // name_weight（查询非完整工具名 → 无精确奖励）
  EXPECT_DOUBLE_EQ(b, 1.0);  // description_weight
}

TEST(Keyword, score_exact_name_bonus) {
  KeywordSelector sel;
  ToolSpec const spec = simple("get_weather", "查询天气");
  double const with_bonus = sel.score(spec, "get_weather");
  double const without = sel.score(spec, "weather");
  EXPECT_GT(with_bonus, without);
}

TEST(Selector, all_returns_everything) {
  ToolRegistry reg;
  fill(reg);
  AllToolsSelector sel;
  auto picked = sel.select(reg, "任意查询", 2);  // 忽略 query 与 k
  EXPECT_EQ(picked.size(), 5u);
}

TEST(Selector, keyword_picks_relevant_subset) {
  ToolRegistry reg;
  fill(reg);
  KeywordSelector sel;

  auto picked = sel.select(reg, "今天天气如何", 3);
  ASSERT_EQ(picked.size(), 1u);
  EXPECT_EQ(picked[0]->name, "get_weather");

  auto multi = sel.select(reg, "查询", 5);
  EXPECT_EQ(multi.size(), 2u);  // get_weather + get_stock_price
}

TEST(Selector, keyword_require_match_filters_unrelated) {
  ToolRegistry reg;
  fill(reg);
  KeywordSelector strict;
  EXPECT_TRUE(strict.select(reg, "zzzz", 5).empty());

  KeywordSelector::Options loose_opts;
  loose_opts.require_match = false;
  KeywordSelector loose(loose_opts);
  EXPECT_EQ(loose.select(reg, "zzzz", 5).size(), 5u);
}

TEST(Selector, keyword_topk_and_stable_tie_order) {
  ToolRegistry reg;
  reg.add(simple("alpha_tool", "同样的描述"));
  reg.add(simple("beta_tool", "同样的描述"));
  reg.add(simple("gamma_tool", "同样的描述"));
  KeywordSelector sel;

  // 三个工具描述相同 → 查询 "同样的描述" 命中分数相同 → 名次按注册顺序（稳定排序）
  auto picked = sel.select(reg, "同样的描述", 2);
  ASSERT_EQ(picked.size(), 2u);
  EXPECT_EQ(picked[0]->name, "alpha_tool");
  EXPECT_EQ(picked[1]->name, "beta_tool");
}

TEST(Selector, idf_downweights_common_words) {
  // 回归：无 IDF 时 "get" 这类高频词会把无关工具顶上来（实测 precision 0.38 → IDF 后 1.00）
  ToolRegistry reg;
  for (int i = 0; i < 50; ++i)
    reg.add(simple("get_thing_" + std::to_string(i), "获取某个通用资源"));
  reg.add(simple("stock_price", "查询股票价格"));

  KeywordSelector with_idf;  // 默认 use_idf = true
  auto picked = with_idf.select(reg, "get stock", 3);
  ASSERT_FALSE(picked.empty());
  EXPECT_EQ(picked[0]->name, "stock_price");

  // 关掉 IDF：高频词 "get" 主导 → 目标工具不一定排第一
  KeywordSelector::Options naive_opts;
  naive_opts.use_idf = false;
  KeywordSelector naive(naive_opts);
  auto naive_picked = naive.select(reg, "get stock", 3);
  ASSERT_FALSE(naive_picked.empty());
  EXPECT_NE(naive_picked[0]->name, "stock_price");
}