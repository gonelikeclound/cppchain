// M7+ 测试：状态时效性守卫（StaleStateGuard）
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cppchain/agent/stale.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/http/fake_transport.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/openai/openai_chat_model.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

// 可控时钟：测试自己推进。
// 注意：传进 std::function 时会被**拷贝**，所以状态必须共享（否则测试推进的是原对象，
// 守卫看到的是副本 —— 这个坑第一次写测试时就踩了）。
struct FakeClock {
  std::shared_ptr<std::chrono::system_clock::time_point> now =
      std::make_shared<std::chrono::system_clock::time_point>();
  std::chrono::system_clock::time_point operator()() const { return *now; }
  void advance(std::chrono::seconds s) { *now += s; }
};

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

Message tool_message_with_stamp(std::int64_t observed_at, std::int64_t ttl,
                                std::string const& content = "balance=100") {
  Message m = message::tool_message(content, "call_1");
  m.additional_kwargs[StaleStateGuard::kObservedAt] = observed_at;
  m.additional_kwargs[StaleStateGuard::kTtlSeconds] = ttl;
  return m;
}

}  // namespace

TEST(StaleStateGuard, fresh_result_is_untouched) {
  FakeClock clk;
  StaleStateGuard guard(clk);
  std::vector<Message> msgs{tool_message_with_stamp(0, 60)};

  clk.advance(std::chrono::seconds(10));  // 10 ≤ 60 → 仍新鲜
  guard.before_model(msgs);

  EXPECT_EQ(msgs[0].text(), "balance=100");
  EXPECT_EQ(guard.invalidations(), 0u);
  EXPECT_FALSE(msgs[0].additional_kwargs.contains(StaleStateGuard::kStale));
}

TEST(StaleStateGuard, expired_result_is_rewritten_with_warning) {
  FakeClock clk;
  StaleStateGuard guard(clk);
  std::vector<Message> msgs{tool_message_with_stamp(0, 60)};

  clk.advance(std::chrono::seconds(120));  // 120 > 60 → 过期
  guard.before_model(msgs);

  auto const text = msgs[0].text();
  EXPECT_NE(text.find("不能作为现状使用"), std::string::npos);
  EXPECT_NE(text.find("已超过时效"), std::string::npos);
  EXPECT_NE(text.find("请重新调用工具"), std::string::npos);
  EXPECT_NE(text.find("balance=100"), std::string::npos);  // 旧值保留作参考
  EXPECT_EQ(guard.invalidations(), 1u);
  EXPECT_TRUE(msgs[0].additional_kwargs.value(StaleStateGuard::kStale, false));
}

TEST(StaleStateGuard, rewrite_is_idempotent) {
  FakeClock clk;
  StaleStateGuard guard(clk);
  std::vector<Message> msgs{tool_message_with_stamp(0, 60)};
  clk.advance(std::chrono::seconds(120));

  guard.before_model(msgs);
  guard.before_model(msgs);  // 再来一次不该重复包裹
  guard.before_model(msgs);

  EXPECT_EQ(guard.invalidations(), 1u);
  auto const text = msgs[0].text();
  EXPECT_EQ(text.find("原值仅供参考"), text.rfind("原值仅供参考"));  // 只出现一次
}

TEST(StaleStateGuard, preserves_tool_call_id_so_protocol_stays_closed) {
  FakeClock clk;
  StaleStateGuard guard(clk);
  Message ai;
  ai.type = Role::Ai;
  ai.tool_calls.push_back(message::ToolCall{"call_1", "get_balance", nlohmann::json::object()});
  std::vector<Message> msgs{ai, tool_message_with_stamp(1000, 60)};

  clk.advance(std::chrono::seconds(120));
  guard.before_model(msgs);

  EXPECT_EQ(msgs[1].tool_call_id.value_or(""), "call_1");
  EXPECT_FALSE(message::validate_sequence(msgs).has_value());  // 协议闭环未被破坏
}

TEST(StaleStateGuard, tools_without_ttl_are_ignored) {
  FakeClock clk;
  StaleStateGuard guard(clk);
  std::vector<Message> msgs{message::tool_message("静态知识：C++ 由 Bjarne 设计", "call_1")};

  clk.advance(std::chrono::seconds(100000));
  guard.before_model(msgs);

  EXPECT_EQ(msgs[0].text(), "静态知识：C++ 由 Bjarne 设计");
  EXPECT_EQ(guard.invalidations(), 0u);
}

TEST(StaleStateGuard, timestamp_survives_serialization_roundtrip) {
  FakeClock clk;
  StaleStateGuard guard(clk);
  Message const stamped = tool_message_with_stamp(0, 60);

  // 序列化 → 反序列化（模拟 checkpoint 落盘/恢复）
  auto const j = nlohmann::json(stamped);
  Message const restored = j.get<Message>();
  ASSERT_TRUE(restored.additional_kwargs.contains(StaleStateGuard::kObservedAt));

  std::vector<Message> msgs{restored};
  clk.advance(std::chrono::seconds(120));
  guard.before_model(msgs);

  EXPECT_EQ(guard.invalidations(), 1u);  // 恢复后依然能识别过期
}

// ---------- 端到端：过期标注真的进入了发给模型的 prompt ----------

TEST(StaleStateGuard, stale_marker_reaches_the_model_prompt) {
  auto transport = std::make_shared<http::FakeTransport>();
  model::ModelConfig cfg;
  cfg.api_key = "k";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "m";
  model::openai::OpenAIChatModel model(cfg, transport);

  int calls = 0;
  tool::ToolRegistry tools;
  auto balance = tool::make_tool("get_balance", "查余额（易变状态）", nlohmann::json::object(),
                                 [&calls](nlohmann::json const&,
                                          tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                                   ++calls;
                                   return nlohmann::json("balance=100");
                                 });
  balance.ttl = std::chrono::seconds(0);  // 立刻过期，便于测试
  tools.add(std::move(balance));

  // 时钟每次调用前进 1 秒 → 工具结果在下一轮 before_model 时必然过期
  auto tick = std::make_shared<std::chrono::system_clock::time_point>(
      std::chrono::system_clock::time_point{});
  auto clock = [tick] {
    *tick += std::chrono::seconds(1);
    return *tick;
  };
  auto guard = std::make_shared<StaleStateGuard>(clock);

  // 注意：Agent::invoke 每次重置消息，历史要靠 checkpointer + thread 维系
  state::MemoryCheckpointer cp;
  AgentOptions opts;
  opts.checkpointer = &cp;
  opts.thread_id = "stale-e2e";
  Agent agent(model, tools, opts);
  agent.add_middleware(guard);

  transport->push(tool_script("get_balance", "{}", "c1"));
  transport->push(text_script("余额 100"));
  auto r = agent.invoke({message::human_message("我余额多少")});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(calls, 1);

  // 第二次调用：历史里有已过期的工具结果 → 应被标注后再发给模型
  transport->push(text_script("（重新确认过）"));
  auto r2 = agent.invoke({message::human_message("再问一次余额")});
  ASSERT_TRUE(r2.has_value()) << r2.error().message;

  auto const body = nlohmann::json::parse(transport->requests().back().body);
  auto const dumped = body["messages"].dump();
  EXPECT_NE(dumped.find("不能作为现状使用"), std::string::npos);
  EXPECT_GE(guard->invalidations(), 1u);
}