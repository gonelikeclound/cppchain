// demo: subagent_ask_expert —— M8 ask_expert（subagent-as-tool，真机）
//
//  看点：子代理整体委派 + 主上下文干净 + 副作用透传不豁免。两个场景：
//   ① 委派计算：父把计算任务整体交给子代理（子代理内部用 calculator 往返），
//      父转录只出现一条子代理结果（answer + stats），子转录不进父 prompt。
//   ② 副作用透传：子代理里想发邮件 → 仍被子代理自己的审批中间件拒绝；
//      父只审批"委派"动作本身，不豁免子代理内部的审批（纵深防御）。
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL
#include <cppchain/agent/agent.hpp>
#include <cppchain/agent/subagent.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/tool/calculator.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using namespace cppchain::tool;
using cppchain::message::Message;
using cppchain::message::Role;
using nlohmann::json;

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

// UTF-8 安全的字节截断（避免把中文切断）
std::string clip(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

void show_transcript(Agent const& agent) {
  for (auto const& m : agent.transcript()) {
    std::string const role = std::string(message::role_str(m.type));
    if (m.type == Role::Ai && !m.tool_calls.empty()) {
      for (auto const& tc : m.tool_calls)
        std::println("  [{}] 调用工具 {}({})", role, tc.name, tc.args.dump());
      if (!m.text().empty()) std::println("  [{}] {}", role, m.text());
    } else if (m.type == Role::Tool) {
      std::println("  [tool:{}] {}", m.status, clip(m.text(), 160));
    } else {
      std::println("  [{}] {}", role, clip(m.text(), 160));
    }
  }
}

// 子代理工具返回的 JSON 体（{"answer","stats"}）
std::optional<json> tool_body(Message const& m) {
  if (m.type != Role::Tool) return std::nullopt;
  auto j = json::parse(m.text(), nullptr, false);
  if (j.is_discarded() || !j.is_object()) return std::nullopt;
  return j;
}

// 最后一次 AI 的 usage（父那一轮的用量）
json last_usage(Agent const& agent) {
  for (auto it = agent.transcript().rbegin(); it != agent.transcript().rend(); ++it)
    if (it->type == Role::Ai && it->usage)
      return {{"input", it->usage->input_tokens},
              {"output", it->usage->output_tokens},
              {"total", it->usage->total_tokens}};
  return json::object();
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

  // ---- 子代理：助理专家，内部有计算工具 + 副作用工具（发邮件，假执行）----
  tool::ToolRegistry child_tools;
  child_tools.add(make_calculator_tool());
  child_tools.add(tool::make_tool(
      "send_email", "发送邮件（有副作用）",
      tool::SchemaBuilder().prop("to", "string", "收件人").prop("body", "string", "正文").build(),
      [](nlohmann::json const& args,
         tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
        auto to = tool::arg<std::string>(args, "to");
        if (!to) return std::unexpected(to.error());
        return nlohmann::json("（已发送给 " + *to + "）");
      },
      /*read_only=*/false));

  // 子代理自己的审批：禁发邮件（父只审批委派，不豁免子代理内部）
  auto approval = std::make_shared<ToolApproval>(std::vector<std::string>{"send_email"});

  SubAgentSpec spec;
  spec.name = "ask_staff";
  spec.description =
      "助理专家子代理：负责计算（calculator）与办公文书（send_email）。把整个子任务交给它，"
      "它会返回最终结论与简要统计，适合没必要在主管上下文里展开的任务。";
  spec.model = &model;
  spec.tools = &child_tools;
  AgentOptions child_opts;
  child_opts.system_prompt =
      "你是助理专家。遇到计算必须调用 calculator；需要给用户发邮件就调用 send_email。"
      "只给出结论，不要解释内部步骤。";
  child_opts.max_iterations = 4;
  spec.options = child_opts;
  spec.middlewares = {approval};

  tool::ToolRegistry parent_tools;
  parent_tools.add(make_subagent_tool(spec));

  std::println("模型: {} | 子代理: {} | 父可见工具: {} 个\n", name, spec.name, parent_tools.size());

  // ---- 场景 ① 委派计算：子代理内部往返，父上下文不膨胀 ----
  {
    std::println("=== ① ask_expert：把计算整体交给子代理 ===");
    AgentOptions opts;
    opts.system_prompt = "你是主管。遇到任何需要执行的任务，调用 ask_staff 整体委派，不要自己动手算。";
    Agent parent(model, parent_tools, opts);

    auto r = parent.invoke(
        {message::human_message("计算 123 * 4567 等于多少？把答案告诉我。")});
    if (!r) {
      std::println("失败: {} ({})", r.error().message, model::kind_str(r.error().kind));
      return 1;
    }

    show_transcript(parent);
    std::println("\n→ 父最终答复: {}", r->text());
    std::println("→ 父最后一轮 usage: {}", last_usage(parent).dump());

    for (auto const& m : parent.transcript()) {
      if (auto body = tool_body(m)) {
        std::println("→ 子代理返回: answer={} | stats={}", (*body)["answer"].dump(),
                     (*body)["stats"].dump());
        std::println("  （子代理内部工具往返发生在它自己的上下文里，父只看得到这条 compact 结果）");
      }
    }
  }

  // ---- 场景 ② 副作用透传：让子代理发邮件 → 被子代理自己的审批拒绝 ----
  {
    std::println("\n=== ② 副作用透传：子代理内部审批（send_email 被禁）===");
    AgentOptions opts;
    opts.system_prompt = "你是主管。需要发邮件时，调用 ask_staff 委派。";
    Agent parent(model, parent_tools, opts);

    auto r = parent.invoke({message::human_message(
        "请让 ask_staff 给 boss@example.com 发送一封正文为'本月报告完成'的邮件。")});
    if (!r) {
      std::println("失败: {} ({})", r.error().message, model::kind_str(r.error().kind));
      return 1;
    }

    show_transcript(parent);
    std::println("\n→ 子代理内部审批拒绝次数: {}", approval->denials());
    std::println("  （父转录里看不到'被拒'回喂——审批发生在子代理内部，父只审批了委派动作本身）");
  }
}