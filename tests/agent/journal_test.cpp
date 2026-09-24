// M7 收口测试：副作用执行日志（防重放 / 未知结果拒跑 / 键模式 / 持久化）
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cppchain/agent/journal.hpp"
#include "cppchain/http/fake_transport.hpp"
#include "cppchain/model/openai/openai_chat_model.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;
using cppchain::message::ToolCall;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

http::FakeTransport::Script tool_script(std::string const& name, std::string const& args,
                                        std::string const& id) {
  http::FakeTransport::Script s;
  nlohmann::json delta = {
      {"tool_calls", nlohmann::json::array({{{"index", 0},
                                             {"id", id},
                                             {"function", {{"name", name}, {"arguments", args}}}}})}};
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", delta}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"), kDone};
  return s;
}

http::FakeTransport::Script text_script(std::string const& text) {
  http::FakeTransport::Script s;
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"), kDone};
  return s;
}

struct Fixture {
  std::shared_ptr<http::FakeTransport> transport = std::make_shared<http::FakeTransport>();
  std::shared_ptr<model::openai::OpenAIChatModel> model;
  tool::ToolRegistry tools;
  int side_effect_calls = 0;   // 真实副作用被执行了几次
  int read_only_calls = 0;

  Fixture() {
    model::ModelConfig cfg;
    cfg.api_key = "k";
    cfg.base_url = "https://api.example.test/v1";
    cfg.model = "m";
    model = std::make_shared<model::openai::OpenAIChatModel>(cfg, transport);

    tools.add(tool::make_tool("charge", "扣款（有副作用）",
                              tool::SchemaBuilder().prop("amount", "integer", "金额").build(),
                              [this](nlohmann::json const&,
                                     tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                                ++side_effect_calls;
                                return nlohmann::json("charged");
                              },
                              /*read_only=*/false));
    tools.add(tool::make_tool("get_balance", "查余额（只读）", nlohmann::json::object(),
                              [this](nlohmann::json const&,
                                     tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                                ++read_only_calls;
                                return nlohmann::json("100");
                              }));
  }

  Agent agent() { return Agent(*model, tools, AgentOptions{}); }
};

}  // namespace

TEST(SideEffectGuard, completed_entry_is_replayed_not_reexecuted) {
  Fixture f;
  MemoryJournal journal;
  auto guard = std::make_shared<SideEffectGuard>(journal);

  // 第一次：模型扣款 → 真的执行
  {
    auto agent = f.agent();
    agent.add_middleware(guard);
    f.transport->push(tool_script("charge", R"({"amount":100})", "call_a"));
    f.transport->push(text_script("已扣款"));
    auto r = agent.invoke({message::human_message("扣 100")});
    ASSERT_TRUE(r.has_value()) << r.error().message;
  }
  EXPECT_EQ(f.side_effect_calls, 1);
  EXPECT_EQ(guard->executions(), 1u);

  // 第二次：同一条 tool_call（同 id）→ 重放，不重复执行
  {
    auto agent = f.agent();
    agent.add_middleware(guard);
    f.transport->push(tool_script("charge", R"({"amount":100})", "call_a"));
    f.transport->push(text_script("已扣款（重放）"));
    auto r = agent.invoke({message::human_message("扣 100")});
    ASSERT_TRUE(r.has_value()) << r.error().message;
  }
  EXPECT_EQ(f.side_effect_calls, 1);  // ← 关键：仍是 1
  EXPECT_EQ(guard->replays(), 1u);

  auto const entry = journal.find("call_a");
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->state, JournalEntry::State::Completed);
  EXPECT_EQ(entry->content, "charged");
}

TEST(SideEffectGuard, intent_without_result_is_blocked) {
  Fixture f;
  MemoryJournal journal;
  // 预置一条"只有意图、没有结果"的记录（模拟执行中崩溃）
  JournalEntry e;
  e.key = "call_x";
  e.tool_name = "charge";
  e.state = JournalEntry::State::Intent;
  journal.record_intent(e);

  auto guard = std::make_shared<SideEffectGuard>(journal);
  auto agent = f.agent();
  agent.add_middleware(guard);
  f.transport->push(tool_script("charge", R"({"amount":50})", "call_x"));
  f.transport->push(text_script("那我先确认一下"));
  auto r = agent.invoke({message::human_message("扣 50")});
  ASSERT_TRUE(r.has_value()) << r.error().message;

  EXPECT_EQ(f.side_effect_calls, 0);  // ← 没有重复执行
  EXPECT_EQ(guard->blocked(), 1u);
  auto const& t = agent.transcript();
  bool found = false;
  for (auto const& m : t)
    if (m.type == Role::Tool && m.status == "error" && m.text().find("拒绝重放") != std::string::npos)
      found = true;
  EXPECT_TRUE(found);
}

TEST(SideEffectGuard, failed_entry_blocked_by_default_retryable_when_configured) {
  Fixture f;
  MemoryJournal journal;
  JournalEntry e;
  e.key = "call_f";
  e.tool_name = "charge";
  e.state = JournalEntry::State::Failed;
  journal.record_intent(e);

  // 默认：拒绝重跑
  {
    auto guard = std::make_shared<SideEffectGuard>(journal);
    auto agent = f.agent();
    agent.add_middleware(guard);
    f.transport->push(tool_script("charge", R"({"amount":10})", "call_f"));
    f.transport->push(text_script("好的"));
    ASSERT_TRUE(agent.invoke({message::human_message("扣 10")}).has_value());
    EXPECT_EQ(f.side_effect_calls, 0);
    EXPECT_EQ(guard->blocked(), 1u);
  }

  // 显式允许重试 → 真的执行
  {
    SideEffectGuard::Options opts;
    opts.retry_on_failure = true;
    auto guard = std::make_shared<SideEffectGuard>(journal, opts);
    auto agent = f.agent();
    agent.add_middleware(guard);
    f.transport->push(tool_script("charge", R"({"amount":10})", "call_f"));
    f.transport->push(text_script("好的"));
    ASSERT_TRUE(agent.invoke({message::human_message("扣 10")}).has_value());
    EXPECT_EQ(f.side_effect_calls, 1);
  }
}

TEST(SideEffectGuard, read_only_tools_are_not_journaled) {
  Fixture f;
  MemoryJournal journal;
  auto guard = std::make_shared<SideEffectGuard>(journal);
  auto agent = f.agent();
  agent.add_middleware(guard);

  f.transport->push(tool_script("get_balance", "{}", "call_r"));
  f.transport->push(text_script("余额 100"));
  ASSERT_TRUE(agent.invoke({message::human_message("余额")}).has_value());

  EXPECT_EQ(journal.size(), 0u);  // 只读工具不占日志
  EXPECT_EQ(f.read_only_calls, 1);
}

TEST(SideEffectGuard, name_and_args_mode_catches_replanned_duplicates) {
  Fixture f;
  MemoryJournal journal;
  SideEffectGuard::Options opts;
  opts.key_mode = JournalKeyMode::ToolNameAndArgs;
  auto guard = std::make_shared<SideEffectGuard>(journal, opts);

  auto run = [&](std::string const& call_id) {
    auto agent = f.agent();
    agent.add_middleware(guard);
    f.transport->push(tool_script("charge", R"({"amount":100})", call_id));
    f.transport->push(text_script("好的"));
    return agent.invoke({message::human_message("扣 100")});
  };

  ASSERT_TRUE(run("call_1").has_value());
  ASSERT_TRUE(run("call_2").has_value());  // 不同 id、相同参数 → 视为重复

  EXPECT_EQ(f.side_effect_calls, 1);  // 只执行一次
  EXPECT_EQ(guard->replays(), 1u);
}

TEST(SideEffectGuard, tool_call_id_mode_allows_same_args_different_calls) {
  Fixture f;
  MemoryJournal journal;
  auto guard = std::make_shared<SideEffectGuard>(journal);  // 默认 ToolCallId

  auto run = [&](std::string const& call_id) {
    auto agent = f.agent();
    agent.add_middleware(guard);
    f.transport->push(tool_script("charge", R"({"amount":100})", call_id));
    f.transport->push(text_script("好的"));
    return agent.invoke({message::human_message("扣 100")});
  };

  ASSERT_TRUE(run("call_1").has_value());
  ASSERT_TRUE(run("call_2").has_value());

  // id 不同 → 视为两次独立操作（语义最明确；代价是漏掉"重新规划导致的重复"）
  EXPECT_EQ(f.side_effect_calls, 2);
}

TEST(MemoryJournal, json_round_trip_survives_restart) {
  MemoryJournal journal;
  JournalEntry e;
  e.key = "k1";
  e.tool_name = "charge";
  e.args = nlohmann::json{{"amount", 100}};
  journal.record_intent(e);
  journal.record_result("k1", "charged", "success");

  auto const dumped = journal.dump();

  // 模拟进程重启：新实例从磁盘内容恢复
  MemoryJournal restored;
  restored.load(dumped);

  auto const found = restored.find("k1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->state, JournalEntry::State::Completed);
  EXPECT_EQ(found->content, "charged");
  EXPECT_EQ(found->args.at("amount").get<int>(), 100);
}

TEST(SideEffectGuard, replay_works_across_process_restart) {
  Fixture f;
  nlohmann::json persisted;

  {
    MemoryJournal journal;
    auto guard = std::make_shared<SideEffectGuard>(journal);
    auto agent = f.agent();
    agent.add_middleware(guard);
    f.transport->push(tool_script("charge", R"({"amount":100})", "call_r"));
    f.transport->push(text_script("已扣款"));
    ASSERT_TRUE(agent.invoke({message::human_message("扣 100")}).has_value());
    persisted = journal.dump();  // 落盘
  }
  EXPECT_EQ(f.side_effect_calls, 1);

  // "重启"：新日志实例 + 从落盘内容恢复
  MemoryJournal journal2;
  journal2.load(persisted);
  auto guard2 = std::make_shared<SideEffectGuard>(journal2);
  auto agent2 = f.agent();
  agent2.add_middleware(guard2);
  f.transport->push(tool_script("charge", R"({"amount":100})", "call_r"));
  f.transport->push(text_script("已扣款"));
  ASSERT_TRUE(agent2.invoke({message::human_message("扣 100")}).has_value());

  EXPECT_EQ(f.side_effect_calls, 1);  // ← 重启后依然没有重复扣款
  EXPECT_EQ(guard2->replays(), 1u);
}