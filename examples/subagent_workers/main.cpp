// demo: subagent_workers —— M8 orchestrator-worker（串行版，真机）
//
//  看点：no-Send 的串行 orchestrator。主管（M4 结构化输出把提问拆成任务计划）
//   → 条件 router 按"results 已回填个数"点选下一 worker → finalize 汇总。
//  worker 的结果只进 state.results（push reducer），主线 messages 保持干净。
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（worker 只用离线工具 calculator/get_datetime）
#include <cppchain/agent/agent.hpp>
#include <cppchain/graph/agent_node.hpp>
#include <cppchain/graph/graph.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/message/serializer.hpp>  // raw.get<Message>()
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/model/structured.hpp>
#include <cppchain/tool/calculator.hpp>
#include <cppchain/tool/datetime.hpp>

#include <filesystem>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using namespace cppchain::tool;
using namespace cppchain::graph;
using cppchain::message::Message;
using cppchain::message::Role;
using cppchain::state::StateSchema;
using Json = nlohmann::json;

namespace {

std::string ca_bundle_from_env() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string ca = demo::env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca.empty()) {
    if (char const* home = std::getenv("HOME")) {
      std::string const c = std::string(home) + "/.geo/corp-ca.pem";
      if (std::filesystem::exists(c)) ca = c;
    }
  }
  return ca;
}

std::string clip(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

void show_agent(std::string const& label, Agent const& agent) {
  std::println("—— {}（{} 次迭代）——", label, agent.iterations());
  for (auto const& m : agent.transcript()) {
    std::string const role = std::string(message::role_str(m.type));
    if (m.type == Role::Ai && !m.tool_calls.empty()) {
      for (auto const& tc : m.tool_calls)
        std::println("  [{}] 调用 {}({})", role, tc.name, tc.args.dump());
      if (!m.text().empty()) std::println("  [{}] {}", role, m.text());
    } else if (m.type == Role::Tool) {
      std::println("  [tool:{}] {}", m.status, clip(m.text(), 120));
    } else {
      std::println("  [{}] {}", role, clip(m.text(), 120));
    }
  }
}

std::vector<Message> one_human(std::string text) {
  return std::vector<Message>{message::human_message(std::move(text))};
}

// 主管拆任务的 schema：worker 用 enum 限定为图里真实节点名 → 非法拓扑在结构层挡住
Json plan_schema() {
  return {{"type", "object"},
          {"required", {"tasks"}},
          {"properties",
           {{"tasks",
             {{"type", "array"},
              {"items",
               {{"type", "object"},
                {"required", {"worker", "input"}},
                {"properties",
                 {{"worker", {{"type", "string"}, {"enum", {"worker_math", "worker_news"}}}},
                  {"input", {{"type", "string"}}}}}}}}}}}};
}

// 串行 orchestrator 的路由：results 已回填任务的个数 = 队列游标（下标对齐，串行不重排）。
// 所有"需要决定下一步"的节点共用它。
auto router = [](Json const& st) -> std::vector<std::string> {
  auto const tasks = st.value("tasks", Json::array());
  std::size_t const i = st.value("results", Json::array()).size();
  if (tasks.is_array() && i < tasks.size()) return {tasks[i].value("worker", "")};
  return {"finalize"};
};

// 任意 worker 的输入切片：队列游标处那个任务（router 与 worker 读取同一下标 → 天然一致）
auto worker_input = [](Json const& st) -> std::vector<Message> {
  std::size_t const i = st.value("results", Json::array()).size();
  return one_human(st.at("tasks")[i].at("input"));
};

}  // namespace

int main() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const base = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const name = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  if (key.empty()) {
    std::println("缺少 LLM_API_KEY（见 .env.example）");
    return 1;
  }

  model::ModelConfig cfg;
  cfg.api_key = key;
  cfg.base_url = base;
  cfg.model = name;
  cfg.ca_info = ca_bundle_from_env();
  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::openai::OpenAIChatModel model(cfg, transport);

  // ---- 三个角色 Agent：计算 worker / 日期 worker / 汇总者 ----
  tool::ToolRegistry math_tools;
  math_tools.add(make_calculator_tool());
  tool::ToolRegistry time_tools;
  time_tools.add(make_datetime_tool());
  tool::ToolRegistry empty;  // 具名空注册表（Agent 只存非拥有指针，防悬垂）

  AgentOptions wm_opts;
  wm_opts.system_prompt = "你是数学专家：计算必须调用 calculator 工具，给出准确结果。";
  Agent worker_math(model, math_tools, wm_opts);

  AgentOptions wn_opts;
  wn_opts.system_prompt = "你是时间日期专家：现在几号/星期几必须调用 get_datetime 工具。";
  Agent worker_news(model, time_tools, wn_opts);

  AgentOptions fn_opts;
  fn_opts.system_prompt = "你是汇总者：把子任务结果合并成一句对用户的话，不复述内部过程。";
  Agent finalizer(model, empty, fn_opts);

  // ---- 图装配 ----
  auto schema = StateSchema::messages_state();
  schema.set_reducer("results", push_reducer());

  // 主管节点：M4 结构化输出 → 一次性产出任务计划（不经过 Agent，不做工具）
  auto supervisor = [&model](Json const& st, NodeContext&) -> Command {
    std::vector<Message> msgs;
    msgs.push_back(message::system_message(
        "你是任务分派者：把用户的请求拆成 1~2 个子任务，每个子任务交给一个 worker。"
        "worker_math 负责数学计算；worker_news 负责时间日期。只拆确实需要外部工具的子任务。"));
    if (st.contains("messages"))
      for (auto const& raw : st.at("messages")) msgs.push_back(raw.get<Message>());

    model::StructuredOutput so(model, plan_schema());
    auto plan = so.invoke(msgs, model::CallOptions{});
    if (!plan) throw std::runtime_error("分派计划失败: " + plan.error().message);
    return Command::with_update(Json{{"tasks", plan->at("tasks")}});
  };

  StateGraph g(schema);
  g.add_node("supervisor", supervisor).add_conditional_edges("supervisor", router);

  g.add_node("worker_math", as_agent_node(worker_math, AgentNodeConfig{
                                                           .input = worker_input,
                                                           .output_key = "results",
                                                           .append_to_messages = false,
                                                       }))
      .add_conditional_edges("worker_math", router);

  g.add_node("worker_news", as_agent_node(worker_news, AgentNodeConfig{
                                                           .input = worker_input,
                                                           .output_key = "results",
                                                           .append_to_messages = false,
                                                       }))
      .add_conditional_edges("worker_news", router);

  g.add_node("finalize", as_agent_node(finalizer, AgentNodeConfig{
                                                      .input = [](Json const& st) {
                                                        auto const results = st.value("results", Json::array());
                                                        std::string joined;
                                                        for (auto const& r : results)
                                                          joined += (joined.empty() ? "" : "\n") + r.get<std::string>();
                                                        return one_human("子任务结果：\n" + joined + "\n请给用户一个最终回答");
                                                      },
                                                      .output_key = "",
                                                      .append_to_messages = true,
                                                  }))
      .add_edge("finalize", kEnd);

  g.set_entry_point("supervisor");
  auto compiled = g.compile();
  if (!compiled) {
    std::println("图编译失败: {}", compiled.error().message);
    return 1;
  }

  Json input = {{"messages", Json::array({Json(message::human_message(
                     "今天星期几？另外帮我计算 12 * 13 等于多少，给一个完整的答复。"))})},
                {"results", Json::array()}};

  std::println("模型: {} —— 跑 orchestrator 串行版\n", name);
  auto out = compiled->invoke(input);
  if (!out) {
    std::println("执行失败: {} ({})", out.error().message,
                 graph::graph_error_kind(out.error().kind));
    return 1;
  }

  // 主线 messages：只有用户输入 + 汇总者的最终答复（worker 内部不出现）
  std::println("=== 主线 messages（supervisor 提问 + 最终汇总）===");
  for (auto const& raw : out->at("messages")) {
    Message const m = raw.get<Message>();
    std::println("  [{}] {}", std::string(message::role_str(m.type)), clip(m.text(), 140));
  }
  std::println("\n总步骤: {} | tasks: {} | results: {}", compiled->last_steps(),
               out->at("tasks").size(), out->at("results").dump());

  // 各 worker 独立的完整转录（它们内部发生了工具往返，但没进主线）
  std::println("\n=== 各子代理自己的上下文（独立、不共享给主线）===");
  show_agent("worker_math", worker_math);
  show_agent("worker_news", worker_news);
  show_agent("finalizer", finalizer);
}