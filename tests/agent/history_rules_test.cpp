// wfr-㊵ 测试：历史**精简/裁剪**的规则引擎（`plan_history_rules`）。
//
// 规则引擎是**纯函数**：产出"计划"（要删的下标 + 改后的消息），由节点固化成 transform op
// 落进会话日志。所以这里既能断言计划本身，也能把计划应用一遍、断言最终消息序列的**协议
// 合法性**（真实端点会因孤儿 ToolMessage 直接 400 —— 这是最硬的约束）。
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
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

// ---- artifact 落盘模式（wfr-㊵ 补测：这条路径原先只有实现、没有测试）----

namespace {

struct ArtifactDir {
  static std::atomic<unsigned> counter;
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
                              ("wf_artifact_" + std::to_string(counter.fetch_add(1)));
  ArtifactDir() { std::filesystem::create_directories(dir); }
  ~ArtifactDir() { std::filesystem::remove_all(dir); }
  [[nodiscard]] std::string str() const { return dir.string(); }
};
std::atomic<unsigned> ArtifactDir::counter{0};

std::size_t count_files(std::string const& dir) {
  std::size_t n = 0;
  for (auto const& e : std::filesystem::directory_iterator(dir))
    if (e.is_regular_file()) ++n;
  return n;
}

}  // namespace

TEST(HistoryRules, artifact_mode_writes_the_full_text_and_leaves_a_reference_line) {
  ArtifactDir d;
  HistoryRuleParams p;
  p.tool_budget = 20;
  p.artifact_mode = true;
  p.artifact_dir = d.str();
  std::string const long_text(400, 'x');   // ≈100 tokens，超预算

  std::vector<Message> const msgs{human("q"), ai_calling({{"c1", "web_search"}}),
                                  tool(long_text, "c1")};
  auto const plan = run(msgs, rules_of({"clip_tool_results"}), p);
  ASSERT_EQ(plan.rewrite.size(), 1u);
  EXPECT_EQ(plan.stats.artifacts_written, 1u);
  EXPECT_EQ(plan.stats.rewritten_tools, 1u);

  auto const out = apply_plan(msgs, plan);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2].artifact.has_value(), true) << "应设 tool_message.artifact";
  if (out[2].artifact) {
    EXPECT_TRUE(std::filesystem::exists(*out[2].artifact)) << "落盘文件不存在";
    // 全文进文件（不是截断版）
    std::ifstream in(*out[2].artifact);
    std::string const got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(got, long_text) << "文件里应是完整原文";
  }
  // inline 换成引用行：含路径 + 规模标注 + "用文件工具读取"
  std::string const line = out[2].text();
  EXPECT_NE(line.find("完整结果已落盘"), std::string::npos) << line;
  EXPECT_NE(line.find("用文件工具读取"), std::string::npos) << line;
  EXPECT_LT(line.size(), long_text.size());
  EXPECT_EQ(count_files(d.str()), 1u);
  EXPECT_FALSE(message::validate_sequence(out).has_value());
}

TEST(HistoryRules, artifact_mode_degrades_gracefully_when_the_dir_cannot_be_created) {
  // 目标路径上放一个**同名文件** → create_directories 必然失败（确定性，不依赖权限）
  ArtifactDir d;
  std::string const blocked = d.str() + "/not_a_dir";
  { std::ofstream f(blocked); f << "x"; }

  HistoryRuleParams p;
  p.tool_budget = 20;
  p.artifact_mode = true;
  p.artifact_dir = blocked;
  std::vector<Message> const msgs{human("q"), ai_calling({{"c1", "web_search"}}),
                                  tool(std::string(400, 'y'), "c1")};

  auto const plan = run(msgs, rules_of({"clip_tool_results"}), p);
  EXPECT_EQ(plan.stats.artifacts_written, 0u) << "落盘失败不该计数";
  auto const out = apply_plan(msgs, plan);
  EXPECT_NE(out[2].text().find("artifact 落盘失败"), std::string::npos) << out[2].text();
  EXPECT_FALSE(out[2].artifact.has_value());
  EXPECT_FALSE(message::validate_sequence(out).has_value()) << "降级也不能破坏协议";
}

TEST(HistoryRules, artifact_mode_numbers_files_and_skips_short_results) {
  ArtifactDir d;
  HistoryRuleParams p;
  p.tool_budget = 20;
  p.artifact_mode = true;
  p.artifact_dir = d.str();
  std::vector<Message> const msgs{
      human("q"), ai_calling({{"c1", "a"}, {"c2", "b"}, {"c3", "c"}}),
      tool(std::string(400, 'a'), "c1"),   // 超预算 → 落盘
      tool("短结果", "c2"),                 // ≤ 预算 → 不动
      tool(std::string(400, 'b'), "c3")};  // 超预算 → 落盘

  auto const plan = run(msgs, rules_of({"clip_tool_results"}), p);
  EXPECT_EQ(plan.stats.artifacts_written, 2u);
  EXPECT_EQ(plan.stats.rewritten_tools, 2u);
  auto const out = apply_plan(msgs, plan);
  EXPECT_EQ(out[3].text(), "短结果");
  EXPECT_EQ(count_files(d.str()), 2u) << "两条各一个文件（编号不撞）";
  EXPECT_NE(out[2].artifact.value_or(""), out[4].artifact.value_or("")) << "文件名应不同";
}

TEST(HistoryRules, artifact_display_root_rewrites_the_line_to_a_root_relative_path) {
  // wfr-㊶：落盘时给 display_root → 引用行/`artifact` 字段变成**相对该根**的路径（不再带绝对前缀），
  // 与沙箱 file_read 的「只认根内相对路径」对齐。跨半边「真的能读回」在 artifact_fs_test.cpp。
  ArtifactDir root;                      // 沙箱根（= 工作区）
  HistoryRuleParams p;
  p.tool_budget = 20;
  p.artifact_mode = true;
  p.artifact_dir = (std::filesystem::path(root.str()) / "sub" / ".wfr" / "artifacts").string();
  p.artifact_display_root = root.str();  // 展示路径相对此根
  std::string const long_text(400, 'x');
  std::vector<Message> const msgs{human("q"), ai_calling({{"c1", "web_search"}}),
                                  tool(long_text, "c1")};

  auto const plan = run(msgs, rules_of({"clip_tool_results"}), p);
  EXPECT_EQ(plan.stats.artifacts_written, 1u);
  auto const out = apply_plan(msgs, plan);
  ASSERT_TRUE(out[2].artifact.has_value());
  // 展示路径：相对根的 sub/.wfr/artifacts/…，不带绝对前缀
  std::string const disp = *out[2].artifact;
  EXPECT_EQ(disp, "sub/.wfr/artifacts/artifact_1_c1.txt") << disp;
  // 引用行里同样用这个相对路径
  EXPECT_NE(out[2].text().find(disp), std::string::npos) << out[2].text();
  // 物理文件在根内对应位置，且是完整原文
  std::ifstream in(root.str() + "/" + disp, std::ios::binary);
  std::string const got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(got, long_text) << "文件里应是完整原文";
}

TEST(HistoryRules, clip_ai_text_after_tokens_ignores_stale_usage_of_rewritten_messages) {
  // 真机回灌的差：AI 消息带真实 usage（端点实测，total 可能数千）。被裁后 usage 是**旧调用**的，
  // 与 after 状态不符 —— after_tokens 必须按新内容重估，否则"裁回答"永远省不出 token（C 的读数）。
  std::vector<Message> msgs{human("q"), ai_calling({{"c1", "web_search"}}),
                            tool("短结果", "c1"), ai(std::string(60, 'x'))};
  msgs.back().usage = message::UsageMetadata{/*input*/ 100, /*output*/ 6000,
                                             /*total*/ 6100, /*reasoning*/ 0};

  HistoryRuleParams p;
  p.ai_budget = 8;  // 60 ASCII 字符 ≈ 15 token > 8 → 重写
  auto const plan = run(msgs, rules_of({"clip_tool_results", "clip_ai_text"}), p);
  EXPECT_EQ(plan.stats.rewritten_ai, 1u);
  EXPECT_LT(plan.stats.after_tokens, plan.stats.before_tokens)
      << "被重写消息的旧 usage 不得计入 after_tokens（否则反映不出裁剪）";
  auto const out = apply_plan(msgs, plan);
  EXPECT_NE(out.back().text().find("内容已裁剪"), std::string::npos) << out.back().text();
}

// ============================================================
// 2026-10-28 补充：跨领域覆盖 + 机制空白（规则引擎复核）
// ============================================================

// ③ 按类别删：财务域（get_stock / pay 属 finance，get_weather 属 weather）——
// 验证"删 finance 类轮、天气轮幸存"在业务域上成立，且删除后协议仍合法。
TEST(HistoryRules, drop_category_works_across_finance_and_weather_domains) {
  HistoryRuleParams p;
  p.tool_category = {{"get_stock", "finance"}, {"pay", "finance"}, {"get_weather", "weather"}};
  std::vector<Message> const msgs{
      human("看看这只股票，顺便把账单结了"),
      ai_calling({{"c1", "get_stock"}}), tool("AAPL $212.3", "c1"),
      ai_calling({{"c2", "pay"}}), tool("已支付", "c2"),
      ai_calling({{"c3", "get_weather"}}), tool("晴 20~25°C", "c3")};
  auto const plan = run(msgs, rules_of({"drop_category:finance"}), p);
  EXPECT_TRUE(drops(plan, 1));
  EXPECT_TRUE(drops(plan, 2));
  EXPECT_TRUE(drops(plan, 3));
  EXPECT_TRUE(drops(plan, 4));
  EXPECT_FALSE(drops(plan, 5));         // 天气轮幸存
  EXPECT_FALSE(drops(plan, 6));
  EXPECT_EQ(plan.stats.dropped_category_groups, 2u);
  auto const out = apply_plan(msgs, plan);
  ASSERT_EQ(out.size(), 3u);            // human + 天气轮两组
  ASSERT_EQ(out[1].tool_calls.size(), 1u);
  EXPECT_EQ(out[1].tool_calls[0].name, "get_weather");   // 幸存的是天气轮
  EXPECT_FALSE(message::validate_sequence(out).has_value());
}

// ③ 按类别删：代码/运维域（git_status / file_read 属 code）——
// 组内**全部工具同属 code** → 整组删；混入 weather → 整组保留（混类纪律跨域生效）。
TEST(HistoryRules, drop_category_code_domain_git_and_file_read) {
  HistoryRuleParams p;
  p.tool_category = {{"git_status", "code"}, {"file_read", "code"}, {"get_weather", "weather"}};
  std::vector<Message> const msgs{
      human("看下工作区改动，再读一下 config"),
      ai_calling({{"c1", "git_status"}, {"c2", "file_read"}}), tool("M src\nA dst", "c1"),
      tool("timeout: 30\nretry: 2", "c2"),
      ai_calling({{"c3", "get_weather"}}), tool("晴", "c3"), human("再查一个 git 分支")};
  std::vector<Message> const more{
      ai_calling({{"c4", "git_status"}, {"c5", "get_weather"}}), tool("* main", "c4"),
      tool("晴 22°C", "c5")};
  std::vector<Message> all = msgs;
  all.insert(all.end(), more.begin(), more.end());

  auto const plan = run(all, rules_of({"drop_category:code"}), p);
  EXPECT_TRUE(drops(plan, 1));
  EXPECT_TRUE(drops(plan, 2));
  EXPECT_TRUE(drops(plan, 3));
  EXPECT_FALSE(drops(plan, 4));        // 纯 code 组已算，[4] 是 weather 轮头
  EXPECT_EQ(plan.stats.dropped_category_groups, 1u);  // 只有 [1,2,3] 那组全 code → 删
  auto const out = apply_plan(all, plan);
  EXPECT_FALSE(message::validate_sequence(out).has_value());
}

// ② 删错误工具轮：客服/工单域 —— 全错的一轮整组删、正常的一轮保留、终态回答不被误伤。
TEST(HistoryRules, drop_error_tools_in_crm_ticket_domain) {
  std::vector<Message> const msgs{
      human("客户投诉订单没发货，查一下工单"),
      ai_calling({{"c1", "fetch_ticket"}}), tool("boom", "c1", "error"),
      ai_calling({{"c2", "ticket_status"}}), tool("IN_PROGRESS", "c2"),
      ai("我先看下了：工单号 T-1024 处理中。")};
  auto const plan = run(msgs, rules_of({"drop_error_tools"}));
  EXPECT_TRUE(drops(plan, 1));
  EXPECT_TRUE(drops(plan, 2));
  EXPECT_FALSE(drops(plan, 3));
  EXPECT_FALSE(drops(plan, 4));
  EXPECT_FALSE(drops(plan, 5));
  auto const out = apply_plan(msgs, plan);
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out.back().text(), "我先看下了：工单号 T-1024 处理中。");
  EXPECT_FALSE(message::validate_sequence(out).has_value());
}

// ⑤ 裁工具结果：结构化 JSON（嵌套 key + 中文长值）也是领域内容形态——
// 截断带注释、<= 预算不动、多字节安全。
TEST(HistoryRules, clip_tool_results_on_long_structured_json_result) {
  HistoryRuleParams p;
  p.tool_budget = 20;
  std::string const json =
      R"({"code":0,"data":{"items":[{"id":1,"name":"订单A","金额":"壹仟贰佰元整","remark":"客户地址：上海市浦东新区世纪大道，电话 138 0000 0000，发票后续补寄"},{"id":2,"name":"订单B","金额":"陆佰元整","remark":"配送中，预计明天上午送达，联系人王师傅"}],"page":1},"msg":"ok"})";
  std::vector<Message> const msgs{human("查订单详情"), ai_calling({{"c1", "order_query"}}),
                                  tool(json, "c1")};
  auto const plan = run(msgs, rules_of({"clip_tool_results"}), p);
  ASSERT_EQ(plan.rewrite.size(), 1u);
  auto const out = apply_plan(msgs, plan);
  EXPECT_NE(out[2].text().find("…[内容已裁剪"), std::string::npos) << out[2].text();
  std::string cleaned = out[2].text();
  message::sanitize_utf8(cleaned);
  EXPECT_EQ(cleaned.size(), out[2].text().size()) << "JSON 截断切坏了多字节字符";
  EXPECT_LT(out[2].text().size(), json.size());
}

// 多语言（中英混排）历史 × 组删 × 剥思考的组合：规则跨语言仍然只动"该动的壳"，
// 幸存消息的原样保真（中英文本不丢、不裁错人）。
TEST(HistoryRules, mixed_language_history_survivors_keep_both_scripts) {
  HistoryRuleParams p;
  p.tool_category = {{"web_search", "retrieval"}};
  Message search = ai_calling({{"c1", "web_search"}}, "我来查一下：天气 + 苹果股价。");
  add_reasoning(search, "先搜天气，再搜 AAPL 的英文新闻。");
  std::vector<Message> msgs{
      human("帮我查一下明天的天气, and also the AAPL stock price?"),
      search,
      tool("Weather: Shanghai tomorrow sunny 20~25°C\nAAPL $212.34", "c1"),
      ai_calling({{"c2", "get_weather"}}), tool("后天 上海 多云 22°C", "c2")};
  Message final = ai("记住了：后天上海多云 22°C。The day after tomorrow 24°C.");
  add_reasoning(final, "收尾提醒：明天没雨，可放心。");
  msgs.push_back(final);

  auto const plan =
      run(msgs, rules_of({"drop_reasoning", "drop_category:retrieval"}), p);
  EXPECT_TRUE(drops(plan, 1));
  EXPECT_TRUE(drops(plan, 2));                    // 检索轮整组删
  EXPECT_EQ(plan.stats.dropped_category_groups, 1u);
  // ① 对"被整组删的 search"不算（dropped 优先跳过）；对幸存终态回答计数 1。
  EXPECT_EQ(plan.stats.stripped_reasoning, 1u);
  auto const out = apply_plan(msgs, plan);
  ASSERT_EQ(out.size(), 4u);           // 检索轮整组删后剩 human + 天气轮 + 终态回答
  for (auto const& m : out)
    for (auto const& b : m.content)
      EXPECT_FALSE(std::holds_alternative<ReasoningBlock>(b));
  EXPECT_NE(out[3].text().find("后天上海多云 22°C"), std::string::npos);
  EXPECT_NE(out[3].text().find("day after tomorrow"), std::string::npos);
  EXPECT_FALSE(message::validate_sequence(out).has_value());
}

// lean 预设（第 4 个 preset，文档未讲）：剥思考 + 裁工具结果 + 裁终态长答 ——
// 一轮里工具与回答都被裁、协议不破。
TEST(HistoryRules, lean_preset_clips_tool_results_and_terminal_answers) {
  HistoryRuleParams p;
  p.tool_budget = 20;
  p.ai_budget = 20;
  std::string const long_text(400, 'x');          // ≈100 tokens
  std::vector<Message> const msgs{human("q"), ai_calling({{"c1", "a"}}),
                                  tool(long_text, "c1"), ai(long_text)};
  std::vector<std::string> const rules = resolve_history_rules("lean", {});
  auto const plan = run(msgs, rules, p);
  EXPECT_EQ(plan.stats.rewritten_tools, 1u);
  EXPECT_EQ(plan.stats.rewritten_ai, 1u);
  auto const out = apply_plan(msgs, plan);
  EXPECT_NE(out[2].text().find("内容已裁剪"), std::string::npos);
  EXPECT_NE(out[3].text().find("内容已裁剪"), std::string::npos);
  EXPECT_FALSE(message::validate_sequence(out).has_value());
}

// clip_scope=ai_text 在运行级也绝不碰工具结果（resolve 级的门控在规则表已测，这里测计划本身）。
TEST(HistoryRules, clip_scope_ai_text_never_rewrites_tool_messages) {
  HistoryRuleParams p;
  p.tool_budget = 3;
  p.ai_budget = 3;
  std::string const long_text(400, 'z');
  std::vector<Message> const msgs{human("q"), ai_calling({{"c1", "a"}}),
                                  tool(long_text, "c1"), ai(long_text)};
  std::vector<std::string> const rules = resolve_history_rules("clip", {}, "ai_text");
  EXPECT_EQ(rules, (std::vector<std::string>{"clip_ai_text"}));
  auto const plan = run(msgs, rules, p);
  EXPECT_EQ(plan.stats.rewritten_tools, 0u);
  EXPECT_EQ(plan.stats.rewritten_ai, 1u);
  auto const out = apply_plan(msgs, plan);
  EXPECT_EQ(out[2].text(), long_text);            // 工具结果原样
  EXPECT_NE(out[3].text().find("内容已裁剪"), std::string::npos);
}

// 实现复核（2026-10-28）：**drop_reasoning × clip_ai_text 同一条终态 AI 的合成**。
// aggressive 预设同时带这两条，而真机最常见的"带思考的终态长答"正好同时命中——
// 初版只生效一条（取决于消费方语义）→ 已修为**顺序合成**：剥思考与裁长答同时成立。
TEST(HistoryRules, aggressive_composes_drop_reasoning_with_clip_ai_text) {
  HistoryRuleParams p;
  p.ai_budget = 40;                                // 让 400 字符（≈100 token）超预算
  Message a = ai("结论写在这。\n" + std::string(400, 'z'));
  add_reasoning(a, "这是给长回答配的思考，必须随 ① 剥掉。");
  std::vector<Message> const msgs{human("q"), a};

  std::vector<std::string> const rules = resolve_history_rules("aggressive", {});
  auto const plan = run(msgs, rules, p);

  // 同一下标只产出一条 rewrite（⑥ 覆盖 ① 的占位，reducer/本地模拟一致）。
  std::size_t idx_ai = 1;
  std::size_t entry_count = 0;
  for (auto const& [i, m] : plan.rewrite)
    if (i == idx_ai) ++entry_count;
  EXPECT_EQ(entry_count, 1u) << "同一终态回答只该有一条 rewrite（剥思考 + 裁剪合成）";

  EXPECT_EQ(plan.stats.stripped_reasoning, 1u);
  EXPECT_EQ(plan.stats.rewritten_ai, 1u);

  auto const out = apply_plan(msgs, plan);
  // ① + ⑥ 都生效：正文被裁、思考也不在。
  EXPECT_NE(out[1].text().find("内容已裁剪"), std::string::npos) << out[1].text();
  for (auto const& b : out[1].content)
    EXPECT_FALSE(std::holds_alternative<ReasoningBlock>(b)) << "带思考的终态长答必须思考+正文一起处理";
  EXPECT_LT(out[1].text().size(), a.text().size());
  EXPECT_LT(plan.stats.after_tokens, plan.stats.before_tokens);
  EXPECT_FALSE(message::validate_sequence(out).has_value());
}

// 实现复核修订（2026-10-29）：孤儿清理升级为**无条件安全网**——任何**非空**规则集都兜底
// （不再只绑定 drop_tool_results 规则）；规则集为空 → 无规则恒等（孤儿也不删）。
TEST(HistoryRules, orphan_safety_net_applies_with_any_nonempty_rule_set) {
  std::vector<Message> const msgs{human("q"), ai_calling({{"c1", "a"}}), tool("ok", "c1"),
                                  tool("孤儿", "c9")};   // 没有对应调用
  // 带删除算子规则 → 孤儿被清。
  auto const with = run(msgs, rules_of({"drop_tool_results"}));
  EXPECT_EQ(with.stats.dropped_orphan_tools, 1u);
  // **升级后**：哪怕只有剥思考（minimal 预设等价）也照样清孤儿——安全网不挑业务规则。
  auto const reason_only = run(msgs, rules_of({"drop_reasoning"}));
  EXPECT_EQ(reason_only.stats.dropped_orphan_tools, 1u);
  auto const out = apply_plan(msgs, reason_only);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2].text(), "ok");                        // 孤儿没了、正常的还在
  // 规则集为空 → 无规则恒等（孤儿也不动）。
  auto const none = run(msgs, {});
  EXPECT_EQ(none.stats.dropped_orphan_tools, 0u);
  EXPECT_EQ(none.stats.dropped_msgs, 0u);
}
