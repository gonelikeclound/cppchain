// M9 测试：TrimHistory token 阈值触发（条数未超但估算超限 → 也压缩；边界安全沿用）
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/agent/agent.hpp"
#include "cppchain/agent/tokens.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;
using cppchain::message::ToolCall;

namespace {

Message human(std::string t) { return message::human_message(std::move(t)); }

// 长中文消息：制造 token 压力（估算口径：一字≈1 token）
Message long_zh(int seed) {
  return human("这是一段用来制造 token 压力的中文内容，字数较多便于压缩触发" + std::to_string(seed) +
               "，请记住其中的细节比如 编号=" + std::to_string(seed));
}

void expect_protocol_valid(std::vector<Message> const& msgs) {
  auto const err = message::validate_sequence(msgs);
  EXPECT_FALSE(err.has_value()) << "协议被破坏: " << (err ? *err : "");
}

}  // namespace

TEST(TrimToken, fires_when_under_count_but_over_tokens) {
  std::vector<Message> msgs;
  for (int i = 0; i < 5; ++i) msgs.push_back(long_zh(i));
  std::size_t const before = tokens::estimate_msgs(msgs);
  ASSERT_GT(before, 40u);  // 确保测试成立：5 条确实超预算

  TrimHistory::Options o;
  o.max_messages = 20;   // 条数阈值不触发（5 < 20）
  o.max_tokens = 40;     // token 阈值触发
  TrimHistory trim(o);
  trim.before_model(msgs);

  EXPECT_EQ(trim.compressions(), 1u);
  EXPECT_LT(msgs.size(), 5u);                  // 确实折叠了
  EXPECT_LT(tokens::estimate_msgs(msgs), before);  // 压后估算 < 压前
}

TEST(TrimToken, does_not_trigger_within_budget) {
  std::vector<Message> msgs{long_zh(0), long_zh(1)};
  TrimHistory::Options o;
  o.max_messages = 20;
  o.max_tokens = 1000;
  TrimHistory trim(o);
  trim.before_model(msgs);
  EXPECT_EQ(trim.compressions(), 0u);
  EXPECT_EQ(msgs.size(), 2u);
}

TEST(TrimToken, records_tokens_saved_when_summarizer_present) {
  std::vector<Message> msgs;
  for (int i = 0; i < 6; ++i) msgs.push_back(long_zh(i));

  TrimHistory::Options o;
  o.max_messages = 3;
  o.summarizer = [](std::vector<Message> const&) -> std::expected<std::string, model::ModelError> {
    return "（摘要：用户提了几个带编号的问题）";
  };
  TrimHistory trim(o);
  trim.before_model(msgs);
  EXPECT_EQ(trim.summaries(), 1u);
  EXPECT_GT(trim.estimated_tokens_saved(), 0u);  // 被折叠的 3 条 > 摘要文本
}

TEST(TrimToken, token_fold_does_not_orphan_tool_messages) {
  // 工具调用组混在大段长消息中间：token 触发的折叠也必须保全"AI(tool_calls)+ToolMessage"
  std::vector<Message> msgs{message::system_message("sys")};
  for (int i = 0; i < 4; ++i) {
    Message ai;
    ai.type = Role::Ai;
    ai.tool_calls.push_back(ToolCall{"c" + std::to_string(i), "calc", nlohmann::json::object()});
    msgs.push_back(ai);
    msgs.push_back(message::tool_message("结果 " + std::to_string(i), "c" + std::to_string(i)));
  }
  for (int i = 0; i < 3; ++i) msgs.push_back(long_zh(100 + i));  // 制造 token 压力

  TrimHistory::Options o;
  o.max_messages = 20;  // 条数不触发
  o.max_tokens = 60;    // 必须折叠
  TrimHistory trim(o);
  trim.before_model(msgs);
  EXPECT_EQ(trim.compressions(), 1u);
  expect_protocol_valid(msgs);  // 修复前这里会是孤儿 ToolMessage
}