// M7 测试：Agent 循环与三个中间件（离线 FakeTransport，确定性）
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cppchain/agent/agent.hpp"
#include "cppchain/http/fake_transport.hpp"
#include "cppchain/model/openai/openai_chat_model.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;
using cppchain::state::MemoryCheckpointer;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

// 模型回复：文本
http::FakeTransport::Script text_script(std::string const& text) {
  http::FakeTransport::Script s;
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"), kDone};
  return s;
}

// 模型回复：调用工具
http::FakeTransport::Script tool_script(std::string const& name, std::string const& args,
                                        std::string const& id = "call_1") {
  http::FakeTransport::Script s;
  nlohmann::json delta = {
      {"tool_calls", nlohmann::json::array({{{"index", 0},
                                             {"id", id},
                                             {"function", {{"name", name}, {"arguments", args}}}}})}};
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", delta}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"), kDone};
  return s;
}

void fill_tools(tool::ToolRegistry& reg) {
  reg.add(tool::make_tool("get_weather", "查天气",
                          tool::SchemaBuilder().prop("city", "string", "城市").build(),
                          [](nlohmann::json const& args,
                             tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                            auto city = tool::arg<std::string>(args, "city");
                            if (!city) return std::unexpected(city.error());
                            return nlohmann::json("Sunny in " + *city);
                          }));
  reg.add(tool::make_tool("dump_all", "返回一大段数据", nlohmann::json::object(),
                          [](nlohmann::json const&, tool::ToolContext&) {
                            return std::expected<nlohmann::json, tool::ToolError>{
                                nlohmann::json(std::string(500, 'x'))};
                          }));
  reg.add(tool::make_tool("delete_file", "删除文件（有副作用）",
                          tool::SchemaBuilder().prop("path", "string", "路径").build(),
                          [](nlohmann::json const&, tool::ToolContext&) {
                            return std::expected<nlohmann::json, tool::ToolError>{
                                nlohmann::json("deleted")};
                          },
                          /*read_only=*/false));
}

struct Fixture {
  std::shared_ptr<http::FakeTransport> transport = std::make_shared<http::FakeTransport>();
  std::shared_ptr<model::openai::OpenAIChatModel> model;
  tool::ToolRegistry tools;

  Fixture() {
    model::ModelConfig cfg;
    cfg.api_key = "k";
    cfg.base_url = "https://api.example.test/v1";
    cfg.model = "m";
    model = std::make_shared<model::openai::OpenAIChatModel>(cfg, transport);
    fill_tools(tools);
  }

  Agent make(AgentOptions opts = {}) { return Agent(*model, tools, std::move(opts)); }
};

}  // namespace

TEST(Agent, single_tool_round_trip_then_final) {
  Fixture f;
  f.transport->push(tool_script("get_weather", R"({"city":"SF"})"));
  f.transport->push(text_script("旧金山晴"));

  Agent agent = f.make();
  auto r = agent.invoke({message::human_message("天气？")});

  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->text(), "旧金山晴");
  EXPECT_EQ(agent.iterations(), 2u);

  auto const& t = agent.transcript();
  // system? 无 → [human, ai(tool_calls), tool, ai(final)]
  ASSERT_GE(t.size(), 4u);
  EXPECT_EQ(t[1].type, Role::Ai);
  ASSERT_EQ(t[1].tool_calls.size(), 1u);
  EXPECT_EQ(t[2].type, Role::Tool);
  EXPECT_EQ(t[2].status, "success");
  EXPECT_EQ(t[2].text(), "Sunny in SF");
  EXPECT_FALSE(message::validate_sequence(t).has_value());  // 协议闭环
}

TEST(Agent, multiple_tool_rounds) {
  Fixture f;
  f.transport->push(tool_script("get_weather", R"({"city":"SF"})", "c1"));
  f.transport->push(tool_script("dump_all", "{}", "c2"));
  f.transport->push(text_script("都查完了"));

  Agent agent = f.make();
  auto r = agent.invoke({message::human_message("都查一下")});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(agent.iterations(), 3u);
  EXPECT_EQ(r->text(), "都查完了");
}

TEST(Agent, max_iterations_is_enforced) {
  Fixture f;
  for (int i = 0; i < 5; ++i) f.transport->push(tool_script("get_weather", R"({"city":"SF"})"));

  AgentOptions opts;
  opts.max_iterations = 2;
  Agent agent = f.make(opts);
  auto r = agent.invoke({message::human_message("循环吧")});

  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("max_iterations"), std::string::npos);
  EXPECT_EQ(agent.iterations(), 2u);
}

TEST(Agent, system_prompt_prepended_and_sent) {
  Fixture f;
  f.transport->push(text_script("好"));

  AgentOptions opts;
  opts.system_prompt = "你是严谨的助手";
  Agent agent = f.make(opts);
  ASSERT_TRUE(agent.invoke({message::human_message("你好")}).has_value());

  auto body = nlohmann::json::parse(f.transport->requests()[0].body);
  ASSERT_EQ(body["messages"].size(), 2u);
  EXPECT_EQ(body["messages"][0]["role"].get<std::string>(), "system");
  EXPECT_NE(body["messages"][0]["content"].get<std::string>().find("严谨"), std::string::npos);
}

// ---------- 卡位①：超长工具返回 ----------

TEST(AgentMiddleware, truncates_long_tool_results) {
  Fixture f;
  f.transport->push(tool_script("dump_all", "{}"));
  f.transport->push(text_script("好"));

  Agent agent = f.make();
  agent.add_middleware(std::make_shared<TruncateToolResults>(100));
  ASSERT_TRUE(agent.invoke({message::human_message("dump")}).has_value());

  auto const& t = agent.transcript();
  // 找到 ToolMessage
  auto const* tool_msg = [&]() -> Message const* {
    for (auto const& m : t)
      if (m.type == Role::Tool) return &m;
    return nullptr;
  }();
  ASSERT_NE(tool_msg, nullptr);
  EXPECT_LT(tool_msg->text().size(), 500u);
  EXPECT_NE(tool_msg->text().find("已截断"), std::string::npos);  // 标注被截断
}

// ---------- 卡位②：工具副作用（审批） ----------

TEST(AgentMiddleware, approval_rejection_feeds_error_back_to_model) {
  Fixture f;
  f.transport->push(tool_script("delete_file", R"({"path":"/etc/passwd"})"));
  f.transport->push(text_script("那我就不删了"));

  Agent agent = f.make();
  auto approval = std::make_shared<ToolApproval>(std::vector<std::string>{"delete_file"});
  agent.add_middleware(approval);

  auto r = agent.invoke({message::human_message("删掉 /etc/passwd")});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->text(), "那我就不删了");
  EXPECT_EQ(approval->denials(), 1u);

  // 回喂给模型的必须是一条 status=error 的 ToolMessage（模型据此换策略）
  auto const& t = agent.transcript();
  bool found = false;
  for (auto const& m : t)
    if (m.type == Role::Tool && m.status == "error" && m.text().find("rejected") != std::string::npos)
      found = true;
  EXPECT_TRUE(found);
}

TEST(AgentMiddleware, approval_callback_can_allow_selectively) {
  Fixture f;
  f.transport->push(tool_script("delete_file", R"({"path":"/tmp/ok"})"));
  f.transport->push(text_script("删好了"));

  Agent agent = f.make();
  agent.add_middleware(std::make_shared<ToolApproval>(
      ToolApproval::Decider{[](message::ToolCall const& c, std::string& why) {
        std::string const path = c.args.value("path", "");
        if (path.rfind("/tmp/", 0) == 0) return true;
        why = "只允许删除 /tmp 下的文件";
        return false;
      }}));

  auto r = agent.invoke({message::human_message("删 /tmp/ok")});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->text(), "删好了");
}

// ---------- 卡位③：上下文压缩 ----------

TEST(AgentMiddleware, trim_history_compresses_and_model_sees_summary) {
  Fixture f;
  f.transport->push(text_script("知道了"));

  Agent agent = f.make();
  auto trim = std::make_shared<TrimHistory>(4);  // 超过 4 条就压
  agent.add_middleware(trim);

  std::vector<Message> input;
  for (int i = 0; i < 8; ++i) input.push_back(message::human_message("第" + std::to_string(i) + "句"));
  auto r = agent.invoke(input);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(trim->compressions(), 1u);

  // 发给模型的消息数被压回阈值内，且含历史摘要占位
  auto body = nlohmann::json::parse(f.transport->requests()[0].body);
  EXPECT_LE(body["messages"].size(), 5u);
  EXPECT_NE(body["messages"].dump().find("历史摘要"), std::string::npos);
}

TEST(AgentMiddleware, trim_history_uses_summarizer_when_provided) {
  Fixture f;
  f.transport->push(text_script("知道了"));

  Agent agent = f.make();
  auto trim = std::make_shared<TrimHistory>(
      4, TrimHistory::Summarizer{[](std::vector<Message> const& dropped) {
        return std::expected<std::string, model::ModelError>{
            "用户此前问了 " + std::to_string(dropped.size()) + " 个问题"};
      }});
  agent.add_middleware(trim);

  std::vector<Message> input;
  for (int i = 0; i < 8; ++i) input.push_back(message::human_message("q" + std::to_string(i)));
  ASSERT_TRUE(agent.invoke(input).has_value());

  auto body = nlohmann::json::parse(f.transport->requests()[0].body);
  EXPECT_NE(body["messages"].dump().find("用户此前问了"), std::string::npos);
}

// ---------- 快照 ----------

TEST(Agent, writes_checkpoint_and_resumes_from_thread) {
  Fixture f;
  MemoryCheckpointer cp;
  f.transport->push(tool_script("get_weather", R"({"city":"SF"})"));
  f.transport->push(text_script("第一次回答"));

  AgentOptions opts;
  opts.checkpointer = &cp;
  opts.thread_id = "t-1";
  Agent agent = f.make(opts);

  auto r1 = agent.invoke({message::human_message("天气？")});
  ASSERT_TRUE(r1.has_value()) << r1.error().message;
  ASSERT_FALSE(agent.last_checkpoint_id().empty());
  // 一轮带工具的对话会写 2 个快照：工具执行后 + 终态（终态必须有，
  // 否则最后一条 AI 回复不进历史，续跑时会丢上下文）
  auto const snaps = cp.list("t-1", "", 0);
  ASSERT_EQ(snaps.size(), 2u);
  EXPECT_EQ(snaps[0].metadata.at("phase").get<std::string>(), "final");

  // 第二次调用同一 thread → 从快照恢复（历史消息被带入）
  f.transport->push(text_script("第二次回答"));
  auto r2 = agent.invoke({message::human_message("再问一句")});
  ASSERT_TRUE(r2.has_value()) << r2.error().message;

  auto body = nlohmann::json::parse(f.transport->requests()[2].body);
  // 恢复的历史（4 条）+ 新问题 1 条
  EXPECT_GE(body["messages"].size(), 5u);
  EXPECT_NE(body["messages"].dump().find("第一次回答"), std::string::npos);
}