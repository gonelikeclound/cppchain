// M9 测试：ToolApproval 审批记忆化（RememberDeny/RememberAll/Off + FIFO 有界 + 只读免审）
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cppchain/agent/agent.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::ToolCall;
using MemoPolicy = ToolApproval::MemoPolicy;

namespace {

ToolCall call(std::string name, std::string args) {
  return ToolCall{"id1", std::move(name), nlohmann::json::parse(std::move(args))};
}

tool::ToolRegistry read_only_reg() {
  tool::ToolRegistry reg;
  reg.add(tool::make_tool("get_weather", "查天气", nlohmann::json::object(),
                          [](nlohmann::json const&, tool::ToolContext&) {
                            return std::expected<nlohmann::json, tool::ToolError>{
                                nlohmann::json("晴")};
                          },
                          /*read_only=*/true));
  return reg;
}

}  // namespace

TEST(ApprovalMemo, remember_deny_skips_second_same_fingerprint) {
  ToolApproval app(std::vector<std::string>{"send_email"});  // 默认 RememberDeny

  std::string why;
  EXPECT_FALSE(app.approve_tool_call(call("send_email", R"({"to":"a","body":"b"})"), why));
  EXPECT_EQ(app.denials(), 1u);

  // 同指纹 → 记忆命中，不再走决策（denials 不再增加）
  EXPECT_FALSE(app.approve_tool_call(call("send_email", R"({"to":"a","body":"b"})"), why));
  EXPECT_EQ(app.denials(), 1u);
  EXPECT_EQ(app.memo_hits(), 1u);

  // 参数键序不同 ≠ 不同指纹（canonical_json 保证）
  EXPECT_FALSE(app.approve_tool_call(call("send_email", R"({"body":"b","to":"a"})"), why));
  EXPECT_EQ(app.memo_hits(), 2u);

  // 参数真不同 → 新指纹 → 重新决策（denials 增加）
  EXPECT_FALSE(app.approve_tool_call(call("send_email", R"({"to":"c","body":"b"})"), why));
  EXPECT_EQ(app.denials(), 2u);
}

TEST(ApprovalMemo, remember_all_caches_allow_too) {
  std::size_t decisions = 0;
  ToolApproval::Decider decider = [&decisions](ToolCall const& c, std::string&) {
    ++decisions;
    return c.args.value("to", "").starts_with("safe");  // safe* 收件人均放行
  };
  ToolApproval app(decider, ToolApproval::Options{MemoPolicy::RememberAll, 128, nullptr});

  std::string why;
  EXPECT_TRUE(app.approve_tool_call(call("send_email", R"({"to":"safe"})"), why));
  EXPECT_TRUE(app.approve_tool_call(call("send_email", R"({"to":"safe"})"), why));
  EXPECT_EQ(decisions, 1u);        // 第二次走记忆（won't 再调 decider）
  EXPECT_EQ(app.memo_hits(), 1u);

  // 不同收件人 → 重新决策
  EXPECT_TRUE(app.approve_tool_call(call("send_email", R"({"to":"safe2"})"), why));
  EXPECT_EQ(decisions, 2u);
}

TEST(ApprovalMemo, off_disables_memory) {
  ToolApproval app(std::vector<std::string>{"send_email"},
                   ToolApproval::Options{MemoPolicy::Off, 128, nullptr});
  std::string why;
  EXPECT_FALSE(app.approve_tool_call(call("send_email", R"({"to":"a"})"), why));
  EXPECT_FALSE(app.approve_tool_call(call("send_email", R"({"to":"a"})"), why));
  EXPECT_EQ(app.denials(), 2u);   // 每次都重新决策
  EXPECT_EQ(app.memo_hits(), 0u);
  EXPECT_EQ(app.memo_size(), 0u);
}

TEST(ApprovalMemo, fifo_bounded_by_max_entries) {
  ToolApproval app(std::vector<std::string>{"send_email"},
                   ToolApproval::Options{MemoPolicy::RememberDeny, /*max_entries=*/1, nullptr});
  std::string why;
  EXPECT_FALSE(app.approve_tool_call(call("send_email", R"({"to":"a"})"), why));  // 记 a，size1
  EXPECT_FALSE(app.approve_tool_call(call("send_email", R"({"to":"b"})"), why));  // 逐出 a，记 b
  EXPECT_EQ(app.memo_size(), 1u);
  EXPECT_FALSE(app.approve_tool_call(call("send_email", R"({"to":"a"})"), why));  // a 已被逐出 → 重新决策
  EXPECT_EQ(app.denials(), 3u);
}

TEST(ApprovalMemo, read_only_tool_skipped_when_registry_provided) {
  tool::ToolRegistry reg = read_only_reg();
  ToolApproval app(std::vector<std::string>{"get_weather"},
                   ToolApproval::Options{MemoPolicy::RememberDeny, 128, &reg});
  std::string why;
  EXPECT_TRUE(app.approve_tool_call(call("get_weather", R"({"city":"gz"})"), why));  // 只读免审
  EXPECT_EQ(app.denials(), 0u);
  EXPECT_EQ(app.memo_size(), 0u);
}