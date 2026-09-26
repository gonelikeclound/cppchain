// M8 测试：subagent-as-tool（ask_expert）—— 隔离 / 失败回喂 / 子代理中间件 / 并发串行化
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cppchain/agent/agent.hpp"
#include "cppchain/agent/subagent.hpp"
#include "cppchain/http/fake_transport.hpp"
#include "cppchain/model/openai/openai_chat_model.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;

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

// 单工具子代理：get_weather（只读，返回 "Sunny in X"）
void fill_child_tools(tool::ToolRegistry& reg) {
  reg.add(tool::make_tool("get_weather", "查天气",
                          tool::SchemaBuilder().prop("city", "string", "城市").build(),
                          [](nlohmann::json const& args,
                             tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                            auto city = tool::arg<std::string>(args, "city");
                            if (!city) return std::unexpected(city.error());
                            return nlohmann::json("Sunny in " + *city);
                          }));
}

struct Fixture {
  std::shared_ptr<http::FakeTransport> transport = std::make_shared<http::FakeTransport>();
  std::shared_ptr<model::openai::OpenAIChatModel> model;
  tool::ToolRegistry child_tools;
  tool::ToolRegistry parent_tools;

  Fixture() {
    model::ModelConfig cfg;
    cfg.api_key = "k";
    cfg.base_url = "https://api.example.test/v1";
    cfg.model = "m";
    model = std::make_shared<model::openai::OpenAIChatModel>(cfg, transport);
    fill_child_tools(child_tools);
  }

  SubAgentSpec weather_spec(AgentOptions opts = {}) {
    SubAgentSpec spec;
    spec.name = "ask_weather";
    spec.description = "天气专家：查询某城市天气，需要时结果来自真实查询";
    spec.model = model.get();
    spec.tools = &child_tools;
    spec.options = std::move(opts);
    return spec;
  }
};

// 解析 ToolMessage（subagent 工具返回的 JSON）里的 stats
nlohmann::json stats_of(Message const& tool_msg) {
  EXPECT_EQ(tool_msg.type, Role::Tool);
  auto parsed = nlohmann::json::parse(tool_msg.text(), nullptr, false);
  if (parsed.is_discarded()) ADD_FAILURE() << "subagent 结果应为一整个 JSON: " << tool_msg.text();
  return parsed.is_object() ? parsed : nlohmann::json::object();
}

}  // namespace

TEST(SubAgent, one_child_round_trip_returns_answer_and_stats) {
  Fixture f;
  AgentOptions child_opts;
  child_opts.system_prompt = "你是天气专家，需要实时天气时调用工具。";
  f.parent_tools.add(make_subagent_tool(f.weather_spec(child_opts)));

  // 脚本顺序 = 消费顺序：父一档（委派）→ 子一档（查工具）→ 子二档（终答）→ 父子二档（终答）
  f.transport->push(tool_script("ask_weather", R"({"query":"旧金山天气"})"));
  f.transport->push(tool_script("get_weather", R"({"city":"SF"})"));
  f.transport->push(text_script("旧金山晴"));
  f.transport->push(text_script("专家答复：旧金山晴"));

  // 父无 system_prompt → 转录形态 = [human, ai(委派), tool(子代理结果), ai(终答)]
  Agent parent(*f.model, f.parent_tools);

  auto r = parent.invoke({message::human_message("旧金山天气？")});
  ASSERT_TRUE(r.has_value()) << r.error().message;

  EXPECT_EQ(r->text(), "专家答复：旧金山晴");
  EXPECT_EQ(parent.iterations(), 2u);

  auto const& t = parent.transcript();
  ASSERT_GE(t.size(), 4u);
  EXPECT_EQ(t[1].type, Role::Ai);
  EXPECT_EQ(t[1].tool_calls.size(), 1u);
  ASSERT_EQ(t[2].type, Role::Tool);
  EXPECT_EQ(t[2].status, "success");

  auto body = stats_of(t[2]);
  EXPECT_EQ(body["answer"], "旧金山晴");
  EXPECT_EQ(body["stats"]["tool_turns"], 1);       // 子代理内部确实发生了工具往返
  EXPECT_EQ(body["stats"]["iterations"], 2);
  EXPECT_FALSE(message::validate_sequence(t).has_value());

  // 隔离：子代理内部工具名绝不进入父转录
  EXPECT_EQ(t[2].text().find("get_weather"), std::string::npos);
  EXPECT_EQ(t[2].text().find("Sunny in"), std::string::npos);  // 子代理的原始工具结果也不露出
}

TEST(SubAgent, child_approval_runs_inside_child_not_parent) {
  Fixture f;
  auto approval = std::make_shared<ToolApproval>(
      std::vector<std::string>{"send_email"});  // 子代理审批名单：禁发邮件

  tool::ToolRegistry child;
  child.add(tool::make_tool("send_email", "发邮件",
                            tool::SchemaBuilder().prop("to", "string", "收件人").build(),
                            [](nlohmann::json const&, tool::ToolContext&) {
                              return std::expected<nlohmann::json, tool::ToolError>{
                                  nlohmann::json("sent")};
                            },
                            /*read_only=*/false));

  SubAgentSpec spec;
  spec.name = "ask_doc";
  spec.description = "文书专家";
  spec.model = f.model.get();
  spec.tools = &child;
  AgentOptions child_opts;
  child_opts.system_prompt = "你是文书专家。";
  spec.options = child_opts;
  spec.middlewares = {approval};

  f.parent_tools.add(make_subagent_tool(spec));

  f.transport->push(tool_script("ask_doc", R"({"query":"给老板发邮件"})"));
  f.transport->push(tool_script("send_email", R"({"to":"boss","body":"hi"})"));
  f.transport->push(text_script("我无法发送邮件"));
  f.transport->push(text_script("知道了"));

  // 父无 system_prompt → [human, ai(委派), tool(子代理结果), ai(终答)]
  Agent parent(*f.model, f.parent_tools);
  auto r = parent.invoke({message::human_message("发个邮件")});
  ASSERT_TRUE(r.has_value()) << r.error().message;

  // 子代理内部审批确实被走到（父并未参与审批）
  EXPECT_EQ(approval->denials(), 1u);
  auto const& t = parent.transcript();
  ASSERT_GE(t.size(), 4u);
  EXPECT_EQ(t[2].type, Role::Tool);
  EXPECT_EQ(t[2].status, "success");
  // 被拒的"error 回喂消息"是子代理内部的，不进父转录
  EXPECT_EQ(t[2].text().find("rejected:"), std::string::npos);
}

TEST(SubAgent, child_model_error_feeds_back_as_tool_error) {
  Fixture f;
  AgentOptions child_opts;
  child_opts.system_prompt = "天气专家";
  f.parent_tools.add(make_subagent_tool(f.weather_spec(child_opts)));

  f.transport->push(tool_script("ask_weather", R"({"query":"X"})"));
  http::FakeTransport::Script err;
  err.error = "upstream boom";
  f.transport->push(err);                       // 子代理模型调用失败
  f.transport->push(text_script("收到错误，换个方式"));  // 父模型自愈

  Agent parent(*f.model, f.parent_tools);
  auto r = parent.invoke({message::human_message("查天气")});
  ASSERT_TRUE(r.has_value()) << r.error().message;

  auto const& t = parent.transcript();
  ASSERT_GE(t.size(), 4u);
  EXPECT_EQ(t[2].type, Role::Tool);
  EXPECT_EQ(t[2].status, "error");              // 子代理错误回喂为 error ToolMessage
  EXPECT_NE(t[2].text().find("upstream boom"), std::string::npos);
  EXPECT_EQ(parent.iterations(), 2u);           // 父模型收到错误后继续
}

TEST(SubAgent, concurrent_calls_same_model_are_serialized) {
  Fixture f;
  AgentOptions child_opts;
  child_opts.system_prompt = "天气专家";
  auto spec = f.weather_spec(child_opts);
  f.parent_tools.add(make_subagent_tool(spec));

  // 两个子代理任务各自消费一对脚本；串行锁保证整体顺序确定（A,B + A,B 或 B,A + B,A 皆可）
  for (int i = 0; i < 2; ++i) {
    f.transport->push(tool_script("get_weather", R"({"city":"SF"})"));
    f.transport->push(text_script("旧金山晴"));
  }

  tool::ToolSpec const& sub_tool = *f.parent_tools.find("ask_weather");
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  nlohmann::json out_a, out_b;

  std::thread ta([&] {
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    tool::ToolContext ctx;
    auto r = sub_tool.handler(nlohmann::json{{"query", "旧金山天气"}}, ctx);
    if (r) out_a = *r;
  });
  std::thread tb([&] {
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    tool::ToolContext ctx;
    auto r = sub_tool.handler(nlohmann::json{{"query", "旧金山天气"}}, ctx);
    if (r) out_b = *r;
  });

  while (ready.load() < 2) std::this_thread::yield();
  go.store(true);
  ta.join();
  tb.join();

  ASSERT_TRUE(out_a.is_object());
  ASSERT_TRUE(out_b.is_object());
  EXPECT_EQ(out_a["answer"], "旧金山晴");
  EXPECT_EQ(out_b["answer"], "旧金山晴");
  EXPECT_EQ(out_a["stats"]["tool_turns"], 1);   // 各自独立完成一轮工具往返
  EXPECT_EQ(out_b["stats"]["tool_turns"], 1);
  EXPECT_EQ(f.transport->requests().size(), 4u);  // 4 次模型调用全部串行消费
}