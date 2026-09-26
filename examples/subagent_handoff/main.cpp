// demo: subagent_handoff —— M8 handoff 简化（真机）
//
//  看点：worker 判定"超出我的领域"→ 调用保留工具 call_supervisor 上交 → 包裹层
//  检测后返回 Command{goto:["supervisor"]}（M6 原语）→ 主管重新分派；redispatch
//  达到上限则 finalize 兜底，杜绝"模型反复上交"死循环。
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL
#include <cppchain/agent/agent.hpp>
#include <cppchain/graph/agent_node.hpp>
#include <cppchain/graph/graph.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/message/serializer.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/tool/calculator.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <print>
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

std::vector<Message> one_human(std::string text) {
  return std::vector<Message>{message::human_message(std::move(text))};
}

// 最后一次 Human 消息的纯文本（路由/输入依据）
std::string last_human_text(Json const& st) {
  auto const& msgs = st.value("messages", Json::array());
  for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
    try {
      Message const m = it->get<Message>();
      if (m.type == Role::Human) return m.text();
    } catch (std::exception const&) {
      continue;
    }
  }
  return "";
}

// worker 转录里是否出现过某保留工具（handoff 的确定性信号，不解析措辞）
bool transcript_called(Agent const& agent, std::string const& tool_name) {
  for (auto const& m : agent.transcript())
    if (m.type == Role::Ai)
      for (auto const& tc : m.tool_calls)
        if (tc.name == tool_name) return true;
  return false;
}

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

  // ---- 角色 ----
  // 数学专家 worker：工具 = calculator + 保留名 call_supervisor（上交理由）
  tool::ToolRegistry worker_tools;
  worker_tools.add(make_calculator_tool());
  worker_tools.add(tool::make_tool(
      "call_supervisor", "本专家处理不了时，把原因上交主管（非数学问题必须调用它，不要用普通文字回答）",
      tool::SchemaBuilder().prop("reason", "string", "处理不了的原因").build(),
      [](nlohmann::json const& args,
         tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
        auto reason = tool::arg<std::string>(args, "reason");
        if (!reason) return std::unexpected(reason.error());
        return nlohmann::json("已上交主管：" + *reason);
      }));

  AgentOptions wm_opts;
  wm_opts.system_prompt =
      "你是数学专家，只处理数学问题（可以用 calculator）。"
      "遇到任何非数学问题，必须调用 call_supervisor 把原因交给主管，绝对不要用普通文字硬答。";
  Agent worker_math(model, worker_tools, wm_opts);

  // 主管：看到 [handoff] 的上交说明就简短回应，决定重新分派或收尾
  tool::ToolRegistry empty;  // 具名空注册表（防悬垂）
  AgentOptions sv_opts;
  sv_opts.system_prompt =
      "你是主管。若最后一条是人类且以 [handoff] 开头，说明是 worker 上交，简短回应、交给重新分派。"
      "若是用户提问，只说一句可分派的话（如'我让数学专家处理'），不要直接解答。";
  Agent supervisor(model, empty, sv_opts);

  // 收尾
  AgentOptions fn_opts;
  fn_opts.system_prompt = "你是收尾者：综合已有信息，给用户一个最终答复。";
  Agent finalizer(model, empty, fn_opts);

  // ---- worker 包裹层：检测 call_supervisor → 上交（redispatch+1，goto supervisor）----
  auto worker_node = [&](Json const& state, NodeContext& ctx) -> Command {
    AgentNodeConfig wcfg;
    wcfg.input = [](Json const& st) { return one_human(st.value("task", "")); };
    wcfg.append_to_messages = true;
    Command cmd = as_agent_node(worker_math, std::move(wcfg))(state, ctx);

    if (transcript_called(worker_math, "call_supervisor")) {
      cmd.update["messages"] =
          Json::array({Json(message::human_message("[handoff] 非数学问题，请主管决定"))});
      cmd.update["redispatch"] = state.value("redispatch", 0) + 1;
      cmd.goto_nodes = {"supervisor"};  // 控制权交回（M6 Command 原语）
    } else {
      cmd.update["answered"] = true;  // 有解了 → 直接收尾
    }
    return cmd;
  };

  // 收尾节点
  AgentNodeConfig fcfg;
  fcfg.input = [](Json const& st) { return one_human("请给最终答复。原始问题：" + st.value("task", "")); };
  fcfg.append_to_messages = true;

  // ---- 路由：有解 / 上交超限 → 收尾；否则派给 worker（上交后主管"再试一次"也走这里）----
  auto router = [](Json const& st) -> std::vector<std::string> {
    if (st.value("answered", false)) return {"finalize"};
    if (st.value("redispatch", 0) >= 2) return {"finalize"};
    return {"worker_math"};
  };

  // ---- 图装配 ----
  StateGraph g(StateSchema::messages_state());
  g.add_node("supervisor", [&](Json const& st, NodeContext& ctx) -> Command {
    AgentNodeConfig scfg;
    scfg.input = [](Json const& s) -> std::vector<Message> {
      std::string const t = last_human_text(s);
      return one_human(t.empty() ? "开始" : t);
    };
    scfg.append_to_messages = true;
    return as_agent_node(supervisor, scfg)(st, ctx);
  });
  g.add_conditional_edges("supervisor", router);

  g.add_node("worker_math", worker_node).add_conditional_edges("worker_math", router);

  g.add_node("finalize", as_agent_node(finalizer, fcfg)).add_edge("finalize", kEnd);
  g.set_entry_point("supervisor");

  auto compiled = g.compile();
  if (!compiled) {
    std::println("图编译失败: {}", compiled.error().message);
    return 1;
  }

  // ---- 跑两个场景：数学成功；天气 → 数学专家上交 → 主管 re-deploy → 上限兜底 ----
  std::println("模型: {} —— handoff 简化\n", name);
  for (std::string const question : {"1 + 2 等于几？", "今天天气如何？可以带伞吗？"}) {
    std::println("═══ 提问：{} ═══", question);
    Json input = {{"messages", Json::array({Json(message::human_message(question))})},
                  {"task", question},
                  {"answered", false},
                  {"redispatch", 0}};
    auto out = compiled->invoke(input);
    if (!out) {
      std::println("执行失败: {} ({})", out.error().message, graph_error_kind(out.error().kind));
      return 1;
    }

    for (auto const& raw : out->at("messages")) {
      Message const m = raw.get<Message>();
      std::string const role = std::string(message::role_str(m.type));
      if (m.type == Role::Tool)
        std::println("  [tool:{}] {}", m.status, clip(m.text(), 90));
      else
        std::println("  [{}] {}", role, clip(m.text(), 110));
    }
    std::println("  → 步骤 {} | answered={} | redispatch={}\n", compiled->last_steps(),
                 out->value("answered", false), out->value("redispatch", 0));
  }
}