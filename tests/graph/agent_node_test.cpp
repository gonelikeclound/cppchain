// M8 测试：as_agent_node（Agent → 图节点）+ 串行 orchestrator + handoff 机制（离线确定性）
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cppchain/agent/agent.hpp"
#include "cppchain/graph/agent_node.hpp"
#include "cppchain/graph/graph.hpp"
#include "cppchain/http/fake_transport.hpp"
#include "cppchain/message/serializer.hpp"  // Message ↔ JSON 的 get<Message>()
#include "cppchain/model/openai/openai_chat_model.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using namespace cppchain::graph;
using cppchain::message::Message;
using cppchain::message::Role;
using cppchain::state::StateSchema;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

http::FakeTransport::Script text_script(std::string const& text) {
  http::FakeTransport::Script s;
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"), kDone};
  return s;
}

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

struct Fixture {
  std::shared_ptr<http::FakeTransport> transport = std::make_shared<http::FakeTransport>();
  std::shared_ptr<model::openai::OpenAIChatModel> model;
  tool::ToolRegistry empty;  // 具名空注册表：Agent 只存非拥有指针，防悬垂

  Fixture() {
    model::ModelConfig cfg;
    cfg.api_key = "k";
    cfg.base_url = "https://api.example.test/v1";
    cfg.model = "m";
    model = std::make_shared<model::openai::OpenAIChatModel>(cfg, transport);
  }

  Agent make(AgentOptions opts = {}) { return Agent(*model, empty, std::move(opts)); }
};

std::vector<Message> one_human(std::string text) {
  return std::vector<Message>{message::human_message(std::move(text))};
}

bool transcript_called(Agent const& agent, std::string const& tool_name) {
  for (auto const& m : agent.transcript())
    if (m.type == Role::Ai)
      for (auto const& tc : m.tool_calls)
        if (tc.name == tool_name) return true;
  return false;
}

}  // namespace

TEST(AgentNode, writes_output_key_and_append_to_messages) {
  Fixture f;
  f.transport->push(text_script("答案 42"));

  Agent a = f.make();
  auto node = as_agent_node(a, AgentNodeConfig{
                                   .input = [](Json const& st) { return one_human(st.value("q", "")); },
                                   .output_key = "answer",
                                   .append_to_messages = true,
                               });

  StateGraph g(StateSchema::messages_state());
  g.add_node("a", node).add_edge("a", kEnd).set_entry_point("a");
  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

  auto out = compiled->invoke(Json{{"q", "1+1"}});
  ASSERT_TRUE(out.has_value()) << out.error().message;
  EXPECT_EQ(out->at("answer"), "答案 42");
  ASSERT_EQ(out->at("messages").size(), 1u);          // input 无 messages → 只追加 AI 消息
  EXPECT_EQ(out->at("messages")[0]["type"], "ai");
  EXPECT_EQ(a.iterations(), 1u);
  ASSERT_EQ(a.transcript().size(), 2u);               // [human, ai]
  EXPECT_EQ(a.transcript()[0].text(), "1+1");
}

TEST(AgentNode, orchestration_serial_workers_keep_main_clean) {
  Fixture f;
  // 消费顺序：w0 → w1 → finalize（supervisor 是确定性 lambda，不调模型）
  f.transport->push(text_script("阳光"));
  f.transport->push(text_script("156"));
  f.transport->push(text_script("天气好，且 12*13=156"));

  Agent a = f.make();
  Agent b = f.make();
  Agent c = f.make();

  auto schema = StateSchema::messages_state();
  schema.set_reducer("results", push_reducer());

  StateGraph g(schema);
  g.add_node("supervisor", [](Json const&, NodeContext&) -> Command {
    return Command::with_update(Json{{"tasks", Json::array({
               {{"worker", "w0"}, {"input", "旧金山天气"}},
               {{"worker", "w1"}, {"input", "12*13"}}})}});
  });

  // 串行 orchestrator 的路由：results 已回填任务的个数 = 队列游标（下标对齐，串行不重排）
  auto router = [](Json const& st) -> std::vector<std::string> {
    auto const tasks = st.value("tasks", Json::array());
    std::size_t const i = st.value("results", Json::array()).size();
    if (tasks.is_array() && i < tasks.size()) return {tasks[i].value("worker", "")};
    return {"finalize"};
  };
  g.add_conditional_edges("supervisor", router);

  g.add_node("w0", as_agent_node(a, AgentNodeConfig{
                                         .input = [](Json const& st) { return one_human(st["tasks"][0]["input"]); },
                                         .output_key = "results",
                                         .append_to_messages = false,
                                     }));
  g.add_conditional_edges("w0", router);

  g.add_node("w1", as_agent_node(b, AgentNodeConfig{
                                         .input = [](Json const& st) { return one_human(st["tasks"][1]["input"]); },
                                         .output_key = "results",
                                         .append_to_messages = false,
                                     }));
  g.add_conditional_edges("w1", router);

  g.add_node("finalize", as_agent_node(
                              c, AgentNodeConfig{
                                     .input = [](Json const& st) {
                                       auto const results = st.value("results", Json::array());
                                       std::string joined;
                                       for (auto const& r : results)
                                         joined += (joined.empty() ? "" : "\n") + r.get<std::string>();
                                       return one_human("汇总：\n" + joined);
                                     },
                                     .output_key = "",
                                     .append_to_messages = true,
                                 }));
  g.add_edge("finalize", kEnd).set_entry_point("supervisor");

  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

  Json input = {{"messages", Json::array({Json(message::human_message("两道小题"))})}, {"results", Json::array()}};
  auto out = compiled->invoke(input);
  ASSERT_TRUE(out.has_value()) << out.error().message;

  // worker 结果按任务顺序回填
  ASSERT_EQ(out->at("results"), Json::array({"阳光", "156"}));
  EXPECT_EQ(compiled->last_steps(), 4u);  // supervisor → w0 → w1 → finalize

  // 主上下文干净：messages 里只有输入 + finalize 汇总，worker 原始答案不外泄
  ASSERT_EQ(out->at("messages").size(), 2u);
  EXPECT_EQ(out->at("messages")[1]["type"], "ai");
  EXPECT_EQ(out->at("messages")[1].get<Message>().text(), "天气好，且 12*13=156");
  EXPECT_EQ(a.transcript().size(), 2u);  // 各 worker 各持自己的干净转录
  EXPECT_EQ(b.transcript().size(), 2u);
}

TEST(AgentNode, worker_handoff_back_to_supervisor_via_goto) {
  Fixture f;
  // worker（数学专家）工具：call_supervisor（保留名，处理不了时上交理由）
  tool::ToolRegistry worker_tools;
  worker_tools.add(tool::make_tool(
      "call_supervisor", "本专家处理不了时上交理由给主管",
      tool::SchemaBuilder().prop("reason", "string", "原因").build(),
      [](nlohmann::json const& args,
         tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
        auto reason = tool::arg<std::string>(args, "reason");
        if (!reason) return std::unexpected(reason.error());
        return nlohmann::json("收到上交：" + *reason);
      }));

  AgentOptions wopts;
  wopts.system_prompt = "你是数学专家，只处理数学问题；处理不了就调用 call_supervisor 上交。";
  Agent wm(*f.model, worker_tools, wopts);

  AgentOptions sopts;
  sopts.system_prompt = "你是主管，收到上交说明后给出最终处理。";
  Agent sup(*f.model, f.empty, sopts);

  // 脚本：wm 调 call_supervisor → wm 终答 → supervisor 收到 [handoff] 后收尾
  f.transport->push(tool_script("call_supervisor", R"({"reason":"天气不归我管"})"));
  f.transport->push(text_script("我这边处理不了"));
  f.transport->push(text_script("收到上交，我来处理"));

  // worker 节点 = as_agent_node + 包裹层：检测到 call_supervisor → [handoff] + redispatch + goto sup
  AgentNodeConfig wcfg;
  wcfg.input = [](Json const& st) { return one_human(st.value("q", "")); };
  auto inner = as_agent_node(wm, wcfg);
  auto worker_node = [&inner, &wm](Json const& state, NodeContext& ctx) -> Command {
    Command cmd = inner(state, ctx);
    if (transcript_called(wm, "call_supervisor")) {
      cmd.update["messages"] =
          Json::array({Json(message::human_message("[handoff] 天气不归我管"))});
      cmd.update["redispatch"] = 1;
      cmd.goto_nodes = {"supervisor"};
    }
    return cmd;
  };

  AgentNodeConfig scfg;
  scfg.input = [](Json const& st) {
    auto const& msgs = st.at("messages");
    return one_human(msgs.empty() ? "" : msgs.back().get<Message>().text());
  };
  auto sup_node = as_agent_node(sup, scfg);

  StateGraph g(StateSchema::messages_state());
  g.add_node("worker", worker_node);
  g.add_node("supervisor", sup_node);
  g.add_edge("supervisor", kEnd);
  g.set_entry_point("worker");

  auto compiled = g.compile();
  ASSERT_TRUE(compiled.has_value());
  auto out = compiled->invoke(Json{{"q", "天气如何"}, {"messages", Json::array()}});
  ASSERT_TRUE(out.has_value()) << out.error().message;

  // handoff 生效：worker → supervisor（控制权上交，而非 worker 自答结束）
  EXPECT_EQ(compiled->last_steps(), 2u);
  auto const& msgs = out->at("messages");
  ASSERT_GE(msgs.size(), 2u);

  bool saw_handoff = false;
  for (auto const& m : msgs)
    if (m.get<Message>().text().starts_with("[handoff]")) saw_handoff = true;
  EXPECT_TRUE(saw_handoff);
  EXPECT_EQ(msgs.back().get<Message>().text(), "收到上交，我来处理");
  EXPECT_EQ(out->value("redispatch", 0), 1);
  EXPECT_TRUE(transcript_called(wm, "call_supervisor"));
}