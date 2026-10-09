// wfr-㊵ 测试：历史**精简/裁剪**的规则引擎（`plan_history_rules`）。
//
// 规则引擎是**纯函数**：产出"计划"（要删的下标 + 改后的消息），由节点固化成 transform op
// 落进会话日志。所以这里既能断言计划本身，也能把计划应用一遍、断言最终消息序列的**协议
// 合法性**（真实端点会因孤儿 ToolMessage 直接 400 —— 这是最硬的约束）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/agent/history.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/message/utf8.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using Json = nlohmann::json;
using cppchain::message::Message;
using cppchain::message::ReasoningBlock;
using cppchain::message::Role;
using cppchain::message::TextBlock;
using cppchain::message::ToolCall;

namespace {

Message human(std::string const& t) { return message::human_message(t); }
Message ai(std::string const& t) { return message::ai_message(t); }

Message ai_calling(std::vector<std::pair<std::string, std::string>> calls,
                   std::string const& text = "") {
  Message m = message::ai_message(text);
  for (auto const& [id, name] : calls) m.tool_calls.push_back(ToolCall{id, name, Json::object()});
  return m;
}

Message tool(std::string const& text, std::string const& id, std::string const& status = "success") {
  return message::tool_message(text, id, status);
}

void add_reasoning(Message& m, std::string const& r) {
  m.content.insert(m.content.begin(), ReasoningBlock{r, std::nullopt});
}

// 把计划应用一遍（与 reducer 的 transform 语义一致），用于断言最终序列
std::vector<Message> apply_plan(std::vector<Message> const& msgs, HistoryRulePlan const& plan) {
  std::vector<Message> out;
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    if (std::find(plan.drop.begin(), plan.drop.end(), i) != plan.drop.end()) continue;
    auto const it = std::find_if(plan.rewrite.begin(), plan.rewrite.end(),
                                 [i](auto const& p) { return p.first == i; });
    out.push_back(it != plan.rewrite.end() ? it->second : msgs[i]);
  }
  return out;
}

std::vector<std::string> rules_of(std::initializer_list<char const*> rs) {
  std::vector<std::string> out;
  for (auto const* r : rs) out.push_back(r);
  return out;
}

HistoryRulePlan run(std::vector<Message> const& msgs, std::vector<std::string> const& rules,
                    HistoryRuleParams params = {}) {
  return plan_history_rules(msgs, rules, params);
}

bool drops(HistoryRulePlan const& p, std::size_t i) {
  return std::find(p.drop.begin(), p.drop.end(), i) != p.drop.end();
}

}  // namespace

// ---- 词表与 preset ----

TEST(HistoryRules, vocabulary_and_presets) {
  EXPECT_TRUE(is_known_history_rule("drop_reasoning"));
  EXPECT_TRUE(is_known_history_rule("clip_ai_text"));
  EXPECT_TRUE(is_known_history_rule("drop_category:retrieval"));   // 只校形状
  EXPECT_FALSE(is_known_history_rule("drop_category:"));
  EXPECT_FALSE(is_known_history_rule("drop_evrything"));

  EXPECT_EQ(parse_history_preset("aggressive"), std::optional<std::string_view>("aggressive"));
  EXPECT_FALSE(parse_history_preset("aggresive").has_value());     // 拼错 → 调用方硬错
}

TEST(HistoryRules, preset_plus_static_rules_dedup_and_order_by_priority) {
  auto const rules = resolve_history_rules("lean", rules_of({"drop_error_tools", "drop_reasoning"}));
  // 固定优先级：reasoning → error → category → orphan → clip_tools → clip_ai
  EXPECT_EQ(rules, (std::vector<std::string>{"drop_reasoning", "drop_error_tools",
                                             "clip_tool_results", "clip_ai_text"}));
}

TEST(HistoryRules, clip_scope_gates_only_the_two_clip_rules) {
  auto const only_tools = resolve_history_rules("clip", {}, "tool_results");
  EXPECT_EQ(only_tools, (std::vector<std::string>{"clip_tool_results"}));
  auto const only_ai = resolve_history_rules("clip", {}, "ai_text");
  EXPECT_EQ(only_ai, (std::vector<std::string>{"clip_ai_text"}));
  auto const both = resolve_history_rules("clip", {}, "both");
  EXPECT_EQ(both.size(), 2u);
}

// ---- ① 去思考过程 ----

TEST(HistoryRules, drop_reasoning_rewrites_only_messages_that_have_reasoning) {
  Message a1 = ai("有思考的回答");
  add_reasoning(a1, "想一下");
  Message a2 = ai("没思考的回答");
  std::vector<Message> const msgs{human("q"), a1, a2};

  auto const plan = run(msgs, rules_of({"drop_reasoning"}));
  ASSERT_EQ(plan.rewrite.size(), 1u);
  EXPECT_EQ(plan.rewrite[0].first, 1u);
  EXPECT_TRUE(plan.drop.empty());
  EXPECT_EQ(plan.stats.stripped_reasoning, 1u);

  auto const out = apply_plan(msgs, plan);
  EXPECT_EQ(out[1].text(), "有思考的回答");            // 正文不动
  for (auto const& b : out[1].content) EXPECT_FALSE(std::holds_alternative<ReasoningBlock>(b));
}

TEST(HistoryRules, drop_reasoning_keeps_tool_calls_intact) {
  Message a1 = ai_calling({{"c1", "get_weather"}});
  add_reasoning(a1, "要查天气");
  std::vector<Message> const msgs{human("q"), a1, tool("晴 25°C", "c1")};

  auto const plan = run(msgs, rules_of({"drop_reasoning"}));
  auto const out = apply_plan(msgs, plan);
  ASSERT_EQ(out[1].tool_calls.size(), 1u);              // tool_calls 绝不能被动
  EXPECT_FALSE(message::validate_sequence(out).has_value());
}

// ---- ② 整组删：错误工具轮 ----

TEST(HistoryRules, drop_error_tools_drops_the_whole_group_including_the_ai) {
  std::vector<Message> const msgs{human("q"),
                                  ai_calling({{"c1", "get_weather"}}),      // [1] 组头
                                  tool("boom", "c1", "error"),              // [2] 全错
                                  human("q2")};
  auto const plan = run(msgs, rules_of({"drop_error_tools"}));
  EXPECT_TRUE(drops(plan, 1));
  EXPECT_TRUE(drops(plan, 2));                          // **连发起调用的 AI 一起去**
  EXPECT_FALSE(drops(plan, 0));
  EXPECT_EQ(plan.stats.dropped_error_groups, 1u);

  auto const out = apply_plan(msgs, plan);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_FALSE(message::validate_sequence(out).has_value()) << "不能留下 dangling tool_calls";
}

TEST(HistoryRules, drop_error_tools_keeps_a_group_where_only_some_results_failed) {
  std::vector<Message> const msgs{human("q"),
                                  ai_calling({{"c1", "a"}, {"c2", "b"}}),
                                  tool("ok", "c1"),
                                  tool("boom", "c2", "error")};
  auto const plan = run(msgs, rules_of({"drop_error_tools"}));
  EXPECT_TRUE(plan.drop.empty()) << "混错/混成功 → 整组保留（部分结果仍然有用）";
  EXPECT_EQ(plan.stats.dropped_error_groups, 0u);
}

// ---- ③ 整组删：按工具类别 ----

TEST(HistoryRules, drop_category_drops_groups_whose_tools_all_match) {
  HistoryRuleParams p;
  p.tool_category = {{"web_search", "retrieval"}, {"calculator", "compute"}};
  std::vector<Message> const msgs{human("q"),
                                  ai_calling({{"c1", "web_search"}}), tool("长文…", "c1"),
                                  ai_calling({{"c2", "calculator"}}), tool("42", "c2"),
                                  human("q2")};
  auto const plan = run(msgs, rules_of({"drop_category:retrieval"}), p);
  EXPECT_TRUE(drops(plan, 1));
  EXPECT_TRUE(drops(plan, 2));
  EXPECT_FALSE(drops(plan, 3));                        // compute 不受影响
  EXPECT_EQ(plan.stats.dropped_category_groups, 1u);

  auto const out = apply_plan(msgs, plan);
  EXPECT_FALSE(message::validate_sequence(out).has_value());
}

TEST(HistoryRules, drop_category_keeps_mixed_category_groups_and_unknown_names) {
  HistoryRuleParams p;
  p.tool_category = {{"web_search", "retrieval"}};
  std::vector<Message> const msgs{human("q"),
                                  ai_calling({{"c1", "web_search"}, {"c2", "calculator"}}),
                                  tool("a", "c1"), tool("b", "c2")};   // 一轮里混了两类
  auto const plan = run(msgs, rules_of({"drop_category:retrieval"}), p);
  EXPECT_TRUE(plan.drop.empty()) << "组内类别不一致 → 整组保留";

  // 名字查不到 → 视为 general，不匹配任何显式类别
  std::vector<Message> const unknown{human("q"), ai_calling({{"c9", "没登记的工具"}}),
                                     tool("x", "c9")};
  EXPECT_TRUE(run(unknown, rules_of({"drop_category:retrieval"}), p).drop.empty());
}

// ---- ④ 孤儿清理 ----

TEST(HistoryRules, drop_tool_results_removes_only_orphans) {
  std::vector<Message> const msgs{human("q"),
                                  ai_calling({{"c1", "a"}}), tool("ok", "c1"),
                                  tool("孤儿", "c9")};                  // 没有对应的调用
  auto const plan = run(msgs, rules_of({"drop_tool_results"}));
  EXPECT_EQ(plan.drop, (std::vector<std::size_t>{3}));
  EXPECT_EQ(plan.stats.dropped_orphan_tools, 1u);
}

// ---- ⑤ 裁工具结果 ----

TEST(HistoryRules, clip_tool_results_touches_only_over_budget_messages) {
  HistoryRuleParams p;
  p.tool_budget = 20;
  std::string const long_text(400, 'x');               // ≈100 tokens
  std::vector<Message> const msgs{human("q"),
                                  ai_calling({{"c1", "a"}, {"c2", "b"}}),
                                  tool(long_text, "c1"), tool("短结果", "c2")};
  auto const plan = run(msgs, rules_of({"clip_tool_results"}), p);
  ASSERT_EQ(plan.rewrite.size(), 1u);
  EXPECT_EQ(plan.rewrite[0].first, 2u);
  EXPECT_EQ(plan.stats.rewritten_tools, 1u);

  auto const out = apply_plan(msgs, plan);
  EXPECT_NE(out[2].text().find("…[内容已裁剪"), std::string::npos) << out[2].text();
  EXPECT_EQ(out[3].text(), "短结果");                   // ≤ 预算的逐字不动
  EXPECT_LT(out[2].text().size(), long_text.size());
}

TEST(HistoryRules, clip_tool_results_never_splits_a_multibyte_character) {
  HistoryRuleParams p;
  p.tool_budget = 3;
  std::string const cjk = "北京今天晴二十五度体感二十四度南风六级";   // 全是多字节字符
  std::vector<Message> const msgs{human("q"), ai_calling({{"c1", "a"}}), tool(cjk, "c1")};
  auto const plan = run(msgs, rules_of({"clip_tool_results"}), p);
  auto const out = apply_plan(msgs, plan);
  std::string const clipped = out[2].text();
  // 不能出现"半个字符"：净化前后应完全相同（sanitize_utf8 会剔掉断字节）
  std::string cleaned = clipped;
  message::sanitize_utf8(cleaned);
  EXPECT_EQ(cleaned.size(), clipped.size()) << "截断切断了多字节字符";
}

// ---- ⑥ 裁终态 AI 回答 ----

TEST(HistoryRules, clip_ai_text_skips_transitional_answers_with_tool_calls) {
  HistoryRuleParams p;
  p.ai_budget = 20;
  std::string const long_text(400, 'y');
  std::vector<Message> const msgs{human("q"),
                                  ai_calling({{"c1", "a"}}, long_text),   // 过渡回答（带调用）
                                  tool("ok", "c1"),
                                  ai(long_text)};                        // 终态回答
  auto const plan = run(msgs, rules_of({"clip_ai_text"}), p);
  ASSERT_EQ(plan.rewrite.size(), 1u);
  EXPECT_EQ(plan.rewrite[0].first, 3u) << "带 tool_calls 的过渡回答不该被裁";

  auto const out = apply_plan(msgs, plan);
  EXPECT_NE(out[3].text().find("…[内容已裁剪"), std::string::npos);
  EXPECT_EQ(out[1].text(), long_text);                 // 过渡回答原样
  EXPECT_EQ(out[1].tool_calls.size(), 1u);
}

TEST(HistoryRules, clip_ai_text_keeps_reasoning_blocks_alone) {
  // reasoning 归 ① 管；⑥ 只换 TextBlock
  HistoryRuleParams p;
  p.ai_budget = 5;
  Message a = ai(std::string(400, 'z'));
  add_reasoning(a, "思考过程");
  std::vector<Message> const msgs{human("q"), a};
  auto const plan = run(msgs, rules_of({"clip_ai_text"}), p);
  auto const out = apply_plan(msgs, plan);
  bool has_reasoning = false;
  for (auto const& b : out[1].content)
    if (std::holds_alternative<ReasoningBlock>(b)) has_reasoning = true;
  EXPECT_TRUE(has_reasoning);
}

// ---- 组合与统计 ----

TEST(HistoryRules, combined_rules_produce_a_protocol_valid_result_and_stats) {
  HistoryRuleParams p;
  p.tool_budget = 30;
  p.ai_budget = 30;
  p.tool_category = {{"web_search", "retrieval"}};
  std::string const long_text(400, 'x');
  Message a1 = ai("最终回答" + std::string(400, 'a'));  // 超 ai 预算
  std::vector<Message> const msgs{
      human("q1"),
      ai_calling({{"c1", "web_search"}}), tool(long_text, "c1"),        // 类别 retrieval → 整组删
      ai_calling({{"c2", "calculator"}}), tool("boom", "c2", "error"),   // 错误组 → 整组删
      a1, human("q2")};

  auto const rules = resolve_history_rules("aggressive", {});
  auto const plan = run(msgs, rules, p);
  auto const out = apply_plan(msgs, plan);

  EXPECT_FALSE(message::validate_sequence(out).has_value()) << "组合规则也不得破坏协议";
  EXPECT_EQ(plan.stats.before_msgs, 7u);
  EXPECT_EQ(out.size(), plan.stats.after_msgs);
  EXPECT_LT(out.size(), msgs.size());
  EXPECT_GE(plan.stats.dropped_groups, 2u);
  EXPECT_GE(plan.stats.dropped_error_groups, 1u);
  EXPECT_GE(plan.stats.dropped_category_groups, 1u);
  EXPECT_EQ(plan.stats.rewritten_ai, 1u);
  EXPECT_LE(plan.stats.after_tokens, plan.stats.before_tokens);
}

TEST(HistoryRules, no_rules_is_identity) {
  std::vector<Message> const msgs{human("q"), ai("a"), human("q2")};
  auto const plan = run(msgs, {});
  EXPECT_TRUE(plan.drop.empty());
  EXPECT_TRUE(plan.rewrite.empty());
  EXPECT_EQ(plan.stats.before_msgs, plan.stats.after_msgs);
  EXPECT_EQ(plan.stats.before_tokens, plan.stats.after_tokens);
}
