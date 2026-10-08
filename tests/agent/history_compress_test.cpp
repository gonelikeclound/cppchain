// wfr-㉙ 测试：历史压缩三档（full / drop_round / keep_evidence）。
//
// 这三档是"历史形态"这个自变量的全部取值，用在 chat2 的对照实验里。硬约束：
//   · 压缩是**投影的渲染期变换**，纯函数、无 IO；
//   · drop_round / keep_evidence 都不得破坏消息序列协议（孤儿 ToolMessage → 真实端点 400）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/agent/history.hpp"
#include "cppchain/agent/tokens.hpp"
#include "cppchain/message/message.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::ReasoningBlock;
using cppchain::message::Role;
using cppchain::message::TextBlock;
using cppchain::message::ToolCall;

namespace {

// 探针里那轮「北京天气」的历史形态（run 2 开头 replay 出来的样子）。
std::vector<Message> beijing_sample() {
  std::vector<Message> msgs;
  msgs.push_back(message::human_message("北京天气怎么样？"));

  Message call;
  call.type = Role::Ai;
  call.content.push_back(ReasoningBlock{"用户问北京天气，需要调用 get_weather", std::nullopt});
  call.content.push_back(TextBlock{"", std::nullopt});
  call.tool_calls.push_back(ToolCall{"call_1", "get_weather", nlohmann::json{{"city", "北京"}}});
  msgs.push_back(call);

  msgs.push_back(message::tool_message(R"({"city":"北京","temp":12,"desc":"晴"})", "call_1"));

  Message answer;
  answer.type = Role::Ai;
  answer.content.push_back(ReasoningBlock{"工具返回 12°C 晴，可直接回答", std::nullopt});
  answer.content.push_back(TextBlock{"北京今天 12°C，晴。", std::nullopt});
  msgs.push_back(answer);

  msgs.push_back(message::human_message("那上海呢？"));
  return msgs;
}

std::size_t count_reasoning(std::vector<Message> const& msgs) {
  std::size_t n = 0;
  for (Message const& m : msgs)
    for (auto const& b : m.content)
      if (std::holds_alternative<ReasoningBlock>(b)) ++n;
  return n;
}

std::size_t count_tool_msgs(std::vector<Message> const& msgs) {
  std::size_t n = 0;
  for (Message const& m : msgs)
    if (m.type == Role::Tool) ++n;
  return n;
}

}  // namespace

// ---- 词表 ----

TEST(HistoryMode, parses_the_three_modes) {
  EXPECT_EQ(parse_history_mode("full"), HistoryMode::Full);
  EXPECT_EQ(parse_history_mode("drop_round"), HistoryMode::DropRound);
  EXPECT_EQ(parse_history_mode("keep_evidence"), HistoryMode::KeepEvidence);
  EXPECT_EQ(std::string(history_mode_name(HistoryMode::KeepEvidence)), "keep_evidence");
}

TEST(HistoryMode, rejects_unknown_and_empty) {
  // 调用方靠"是否为空/是否 null"区分「回落默认」与「硬错」，所以解析器一律返回 nullopt。
  EXPECT_FALSE(parse_history_mode("").has_value());
  EXPECT_FALSE(parse_history_mode("null").has_value());
  EXPECT_FALSE(parse_history_mode("FULL").has_value());
  EXPECT_FALSE(parse_history_mode("keep_evidnce").has_value());  // 拼错：调用方须硬错
}

// ---- full：恒等 ----

TEST(HistoryCompress, full_is_identity) {
  auto const original = beijing_sample();
  auto msgs = original;
  auto const stats = compress_history(msgs, HistoryMode::Full);

  ASSERT_EQ(msgs.size(), original.size());
  for (std::size_t i = 0; i < msgs.size(); ++i) EXPECT_EQ(msgs[i], original[i]);
  EXPECT_EQ(stats.mode, HistoryMode::Full);
  EXPECT_EQ(stats.before_msgs, 5u);
  EXPECT_EQ(stats.after_msgs, 5u);
  EXPECT_EQ(stats.before_tokens, stats.after_tokens);
  EXPECT_EQ(stats.dropped_msgs, 0u);
  EXPECT_EQ(stats.rewritten_msgs, 0u);
  EXPECT_EQ(count_reasoning(msgs), 2u);  // reasoning 原样保留
}

// ---- drop_round：丢工具轮整组 ----

TEST(HistoryCompress, drop_round_removes_the_whole_tool_round) {
  auto msgs = beijing_sample();
  auto const stats = compress_history(msgs, HistoryMode::DropRound);

  ASSERT_EQ(msgs.size(), 3u);
  EXPECT_EQ(msgs[0].type, Role::Human);
  EXPECT_EQ(msgs[0].text(), "北京天气怎么样？");
  EXPECT_EQ(msgs[1].type, Role::Ai);
  EXPECT_EQ(msgs[1].text(), "北京今天 12°C，晴。");
  EXPECT_EQ(msgs[2].type, Role::Human);
  EXPECT_EQ(msgs[2].text(), "那上海呢？");

  EXPECT_EQ(count_tool_msgs(msgs), 0u);
  for (Message const& m : msgs) EXPECT_TRUE(m.tool_calls.empty());
  EXPECT_EQ(count_reasoning(msgs), 0u);
  EXPECT_EQ(stats.dropped_msgs, 2u);  // ai(tool_calls) + tool
  EXPECT_EQ(stats.rewritten_msgs, 0u);
  EXPECT_LT(stats.after_tokens, stats.before_tokens);
  EXPECT_FALSE(message::validate_sequence(msgs).has_value());
}

TEST(HistoryCompress, drop_round_never_orphans_a_tool_message) {
  // ai(tool_calls) 后**没有** tool 消息（截断轮）→ 只删 ai，不留下任何东西。
  std::vector<Message> msgs{message::human_message("q")};
  Message call;
  call.type = Role::Ai;
  call.tool_calls.push_back(ToolCall{"c1", "t", nlohmann::json::object()});
  msgs.push_back(call);
  msgs.push_back(message::human_message("q2"));

  auto const stats = compress_history(msgs, HistoryMode::DropRound);
  ASSERT_EQ(msgs.size(), 2u);
  EXPECT_EQ(stats.dropped_msgs, 1u);
  EXPECT_FALSE(message::validate_sequence(msgs).has_value());
}

TEST(HistoryCompress, drop_round_takes_all_tool_results_of_one_call_together) {
  // 一条 ai 发起两个调用 → 两条 tool 结果 → 三个一起走，绝不拆散。
  std::vector<Message> msgs{message::human_message("q")};
  Message call;
  call.type = Role::Ai;
  call.tool_calls.push_back(ToolCall{"c1", "a", nlohmann::json::object()});
  call.tool_calls.push_back(ToolCall{"c2", "b", nlohmann::json::object()});
  msgs.push_back(call);
  msgs.push_back(message::tool_message("ra", "c1"));
  msgs.push_back(message::tool_message("rb", "c2"));
  msgs.push_back(message::human_message("q2"));

  auto const stats = compress_history(msgs, HistoryMode::DropRound);
  ASSERT_EQ(msgs.size(), 2u);
  EXPECT_EQ(stats.dropped_msgs, 3u);
  EXPECT_FALSE(message::validate_sequence(msgs).has_value());
}

// ---- keep_evidence：不丢消息，只压体积 ----

TEST(HistoryCompress, keep_evidence_keeps_every_message_and_the_call_itself) {
  auto msgs = beijing_sample();
  auto const stats = compress_history(msgs, HistoryMode::KeepEvidence);

  ASSERT_EQ(msgs.size(), 5u);  // 条数不变 —— 与 drop_round 的本质区别
  EXPECT_EQ(count_tool_msgs(msgs), 1u);
  ASSERT_EQ(msgs[1].tool_calls.size(), 1u);  // 发起调用的那条 ai 原样在，模型能看见"我查过"
  EXPECT_EQ(msgs[1].tool_calls[0].name, "get_weather");
  EXPECT_EQ(count_reasoning(msgs), 0u);  // reasoning 全删
  EXPECT_FALSE(message::validate_sequence(msgs).has_value());
  EXPECT_EQ(stats.rewritten_msgs, 0u);  // 本样本结果很短 → 逐字不动

  EXPECT_EQ(msgs[2].text(), R"({"city":"北京","temp":12,"desc":"晴"})");
  EXPECT_EQ(stats.before_tokens, stats.before_tokens);
  EXPECT_LT(stats.after_tokens, stats.before_tokens);  // 省下的全是 reasoning
}

TEST(HistoryCompress, keep_evidence_rewrites_only_results_over_budget) {
  std::string const long_result(400, 'x');  // 400 ASCII ≈ 100 token
  std::vector<Message> msgs{message::human_message("q")};
  Message call;
  call.type = Role::Ai;
  call.tool_calls.push_back(ToolCall{"c1", "get_weather", nlohmann::json{{"city", "北京"}}});
  call.tool_calls.push_back(ToolCall{"c2", "tiny", nlohmann::json::object()});
  msgs.push_back(call);
  msgs.push_back(message::tool_message(long_result, "c1"));
  msgs.push_back(message::tool_message("短结果", "c2"));

  auto const stats = compress_history(msgs, HistoryMode::KeepEvidence, /*budget_tokens=*/20);
  EXPECT_EQ(stats.rewritten_msgs, 1u);  // 只有超预算的那条被改写

  std::string const rewritten = msgs[2].text();
  EXPECT_NE(rewritten, long_result);
  EXPECT_EQ(rewritten.rfind("get_weather({\"city\":\"北京\"}) → ", 0), 0u)  // 凭据行带真实工具名+参数
      << rewritten;
  EXPECT_NE(rewritten.find("[历史工具结果已压缩"), std::string::npos) << rewritten;
  EXPECT_NE(rewritten.find("完整内容仍在会话日志中"), std::string::npos) << rewritten;
  EXPECT_EQ(msgs[2].tool_call_id.value_or(""), "c1");  // 配对键不动
  EXPECT_EQ(msgs[3].text(), "短结果");                  // ≤ budget：逐字保留

  EXPECT_FALSE(message::validate_sequence(msgs).has_value());
}

TEST(HistoryCompress, keep_evidence_credential_line_names_the_source_when_given) {
  std::vector<Message> msgs{message::human_message("q")};
  Message call;
  call.type = Role::Ai;
  call.tool_calls.push_back(ToolCall{"c1", "web_search", nlohmann::json{{"query", "news"}}});
  msgs.push_back(call);
  msgs.push_back(message::tool_message(std::string(400, 'y'), "c1"));

  compress_history(msgs, HistoryMode::KeepEvidence, 20, ".wfr/sessions/session-cmp.log.jsonl");
  EXPECT_NE(msgs[2].text().find("在 .wfr/sessions/session-cmp.log.jsonl 里"), std::string::npos)
      << msgs[2].text();
}

TEST(HistoryCompress, keep_evidence_falls_back_when_the_calling_ai_is_gone) {
  // 孤儿 tool 消息（历史被外部裁剪过）：没有可 join 的调用 → 用中性标签，不崩。
  std::vector<Message> msgs{message::tool_message(std::string(400, 'z'), "ghost")};
  auto const stats = compress_history(msgs, HistoryMode::KeepEvidence, 20);
  EXPECT_EQ(stats.rewritten_msgs, 1u);
  EXPECT_EQ(msgs[0].text().rfind("工具结果 → ", 0), 0u) << msgs[0].text();
}

// ---- 边界 ----

TEST(HistoryCompress, empty_and_system_only_are_identity_in_every_mode) {
  for (HistoryMode const mode :
       {HistoryMode::Full, HistoryMode::DropRound, HistoryMode::KeepEvidence}) {
    std::vector<Message> empty;
    auto s = compress_history(empty, mode);
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(s.before_msgs, 0u);
    EXPECT_EQ(s.after_msgs, 0u);

    std::vector<Message> sys{message::system_message("sys")};
    s = compress_history(sys, mode, 200);
    ASSERT_EQ(sys.size(), 1u);
    EXPECT_EQ(sys[0].text(), "sys");
    EXPECT_EQ(s.after_msgs, 1u);
  }
}

TEST(HistoryCompress, stats_tokens_match_the_official_estimator) {
  auto const original = beijing_sample();
  auto msgs = original;
  auto const stats = compress_history(msgs, HistoryMode::KeepEvidence);
  EXPECT_EQ(stats.before_tokens, tokens::estimate_msgs(original));
  EXPECT_EQ(stats.after_tokens, tokens::estimate_msgs(msgs));
}

TEST(HistoryCompress, stats_json_exposes_the_fields_the_workflow_refs_use) {
  auto msgs = beijing_sample();
  auto const stats = compress_history(msgs, HistoryMode::DropRound);
  auto const j = history_stats_json(stats);
  EXPECT_EQ(j.at("mode"), "drop_round");
  EXPECT_EQ(j.at("before_msgs"), 5);
  EXPECT_EQ(j.at("after_msgs"), 3);
  EXPECT_EQ(j.at("dropped_msgs"), 2);
  EXPECT_EQ(j.at("rewritten_msgs"), 0);
  EXPECT_TRUE(j.contains("before_tokens"));
  EXPECT_TRUE(j.contains("after_tokens"));
  EXPECT_TRUE(j.contains("budget"));
}
