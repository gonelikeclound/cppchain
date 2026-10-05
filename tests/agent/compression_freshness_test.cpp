// M7+ 测试：压缩 × 时效（边界安全 + 易变结果的三种策略）
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cppchain/agent/stale.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;
using cppchain::message::ToolCall;

namespace {

Message human(std::string t) { return message::human_message(std::move(t)); }

// 一组工具往返：AI(tool_calls) + ToolMessage
std::pair<Message, Message> tool_round(std::string const& id, std::string const& content,
                                       bool volatile_state) {
  Message ai;
  ai.type = Role::Ai;
  ai.tool_calls.push_back(ToolCall{id, "get_balance", nlohmann::json::object()});
  Message tool = message::tool_message(content, id);
  if (volatile_state) {
    // 观测时间用 0 基准：测试里的假时钟从 epoch 起走（两者必须同一时间线，
    // 否则 age 为负会被判成"新鲜"——这个坑本项目踩过三次）
    tool.additional_kwargs[StaleStateGuard::kObservedAt] = std::int64_t{0};
    tool.additional_kwargs[StaleStateGuard::kTtlSeconds] = std::int64_t{3};
  }
  return {ai, tool};
}

// 真实端点会因孤儿 ToolMessage 直接 400，所以这是硬约束
void expect_protocol_valid(std::vector<Message> const& msgs) {
  auto const err = message::validate_sequence(msgs);
  EXPECT_FALSE(err.has_value()) << "协议被破坏: " << *err;
}

}  // namespace

TEST(TrimBoundary, moves_back_when_boundary_would_orphan_a_tool_message) {
  std::vector<Message> msgs{message::system_message("sys"), human("q1")};
  auto const [ai, tool] = tool_round("c1", "balance=100", false);
  msgs.push_back(ai);
  msgs.push_back(tool);
  for (int i = 0; i < 4; ++i) msgs.push_back(human("h" + std::to_string(i)));

  // 朴素边界 = 8 - 5 = 3 → 会保留 tool(3) 却折掉 ai(2) → 孤儿
  EXPECT_EQ(TrimHistory::safe_boundary(msgs, /*keep_from=*/1, /*desired=*/3), 2u);

  TrimHistory trim(5);
  trim.before_model(msgs);
  expect_protocol_valid(msgs);  // ← 修复前这里会是孤儿 ToolMessage
}

// 回归：max_messages=0（"不留历史"）会让 drop_count == msgs.size()，
// 原先 safe_boundary 直接 msgs[b] 越界（本机 dev 配置带 _GLIBCXX_ASSERTIONS → abort；
// 关掉断言则是静默越界读）。
// 期望：不崩，语义为"全部折叠"——只留系统提示 + 一条摘要。
TEST(TrimBoundary, zero_max_messages_folds_everything_without_out_of_bounds) {
  std::vector<Message> msgs{message::system_message("sys"), human("q1")};
  auto const [ai, tool] = tool_round("c1", "balance=100", false);
  msgs.push_back(ai);
  msgs.push_back(tool);

  // 边界落在末尾（全部折叠）：不得越界，原样返回 size
  EXPECT_EQ(TrimHistory::safe_boundary(msgs, /*keep_from=*/1, /*desired=*/msgs.size()),
            msgs.size());

  TrimHistory trim(0);
  trim.before_model(msgs);  // 修复前：这里 abort / 越界读
  expect_protocol_valid(msgs);
  ASSERT_EQ(msgs.size(), 2u);  // 系统提示 + 摘要
  EXPECT_EQ(msgs.front().type, Role::System);
  EXPECT_NE(msgs.back().text().find("历史摘要"), std::string::npos);
  EXPECT_EQ(trim.compressions(), 1u);
}

TEST(TrimBoundary, trimmed_history_still_satisfies_protocol_with_many_rounds) {
  std::vector<Message> msgs{message::system_message("sys")};
  for (int i = 0; i < 4; ++i) {
    msgs.push_back(human("q" + std::to_string(i)));
    auto const [ai, tool] = tool_round("c" + std::to_string(i), "value", false);
    msgs.push_back(ai);
    msgs.push_back(tool);
  }
  ASSERT_EQ(msgs.size(), 13u);  // 1 system + 4 轮 × 3

  TrimHistory trim(6);
  trim.before_model(msgs);

  EXPECT_EQ(trim.compressions(), 1u);
  expect_protocol_valid(msgs);
  ASSERT_FALSE(msgs.empty());
  EXPECT_NE(msgs.front().type, Role::Tool);  // 不能以孤儿工具结果开头
}

TEST(VolatilePolicy, fold_is_default_and_loses_freshness_metadata) {
  std::vector<Message> msgs{message::system_message("sys"), human("余额是多少")};
  auto const [ai, tool] = tool_round("c1", "balance=100", /*volatile_state=*/true);
  msgs.push_back(ai);
  msgs.push_back(tool);
  for (int i = 0; i < 5; ++i) msgs.push_back(human("h" + std::to_string(i)));

  TrimHistory trim(4);  // 默认 Fold
  trim.before_model(msgs);

  // 折叠后：带时效元数据的工具结果不复存在 → 时效守卫再也看不到它（这正是失效路径）
  bool any_metadata = false;
  for (auto const& m : msgs)
    if (m.type == Role::Tool && freshness::has_metadata(m)) any_metadata = true;
  EXPECT_FALSE(any_metadata);
  EXPECT_EQ(trim.preserved_groups(), 0u);
}

TEST(VolatilePolicy, preserve_keeps_volatile_group_and_guard_can_still_mark_it) {
  std::vector<Message> msgs{message::system_message("sys"), human("余额是多少")};
  auto const [ai, tool] = tool_round("c1", "balance=100", /*volatile_state=*/true);
  msgs.push_back(ai);
  msgs.push_back(tool);
  for (int i = 0; i < 5; ++i) msgs.push_back(human("h" + std::to_string(i)));

  TrimHistory::Options opts;
  opts.max_messages = 4;
  opts.volatile_policy = TrimHistory::VolatilePolicy::Preserve;
  TrimHistory trim(opts);
  trim.before_model(msgs);

  EXPECT_EQ(trim.preserved_groups(), 1u);
  expect_protocol_valid(msgs);

  // 保留原文 → 时效守卫仍能识别过期并改写为警示
  bool still_has_metadata = false;
  for (auto const& m : msgs)
    if (m.type == Role::Tool && freshness::has_metadata(m)) still_has_metadata = true;
  ASSERT_TRUE(still_has_metadata);

  StaleStateGuard guard([t = std::chrono::system_clock::time_point{}]() mutable {
    t += std::chrono::seconds(100);  // 每次都推进 → 必然过期
    return t;
  });
  guard.before_model(msgs);
  EXPECT_EQ(guard.invalidations(), 1u);

  bool warned = false;
  for (auto const& m : msgs)
    if (m.type == Role::Tool && m.text().find("不能作为现状使用") != std::string::npos) warned = true;
  EXPECT_TRUE(warned);
}

TEST(VolatilePolicy, drop_removes_volatile_group_and_says_so_in_summary) {
  std::vector<Message> msgs{message::system_message("sys"), human("余额是多少")};
  auto const [ai, tool] = tool_round("c1", "balance=100", /*volatile_state=*/true);
  msgs.push_back(ai);
  msgs.push_back(tool);
  for (int i = 0; i < 5; ++i) msgs.push_back(human("h" + std::to_string(i)));

  TrimHistory::Options opts;
  opts.max_messages = 4;
  opts.volatile_policy = TrimHistory::VolatilePolicy::Drop;
  TrimHistory trim(opts);
  trim.before_model(msgs);

  EXPECT_EQ(trim.dropped_groups(), 1u);

  bool has_tool = false;
  bool mentions_drop = false;
  for (auto const& m : msgs) {
    if (m.type == Role::Tool) has_tool = true;
    if (m.text().find("因时效策略被丢弃") != std::string::npos) mentions_drop = true;
  }
  EXPECT_FALSE(has_tool);       // 整组（含 AI）都被丢弃 → 无孤儿
  EXPECT_TRUE(mentions_drop);   // 明确告知模型"需要时重新查询"
  expect_protocol_valid(msgs);
}

TEST(VolatilePolicy, non_volatile_tools_are_folded_normally_under_preserve) {
  std::vector<Message> msgs{message::system_message("sys"), human("查静态文档")};
  auto const [ai, tool] = tool_round("c1", "静态知识内容", /*volatile_state=*/false);
  msgs.push_back(ai);
  msgs.push_back(tool);
  for (int i = 0; i < 5; ++i) msgs.push_back(human("h" + std::to_string(i)));

  TrimHistory::Options opts;
  opts.max_messages = 4;
  opts.volatile_policy = TrimHistory::VolatilePolicy::Preserve;
  TrimHistory trim(opts);
  trim.before_model(msgs);

  // 非易变结果照常折叠（Preserve 只保护带时效元数据的）
  EXPECT_EQ(trim.preserved_groups(), 0u);
  expect_protocol_valid(msgs);
}