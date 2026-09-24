// demo: agent_loop —— Agent 循环与三个卡位中间件（M7，真机）
//
// langchain 参照：create_agent(model, tools, middleware=[...])
// 四段：
//   ① 多步工具调用（模型 → 工具 → 模型）
//   ② 超长工具返回 → TruncateToolResults 截断
//   ③ 工具副作用 → ToolApproval 拒绝并回喂
//   ④ 上下文压缩 → TrimHistory（LLM 摘要版）
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL
#include <cppchain/agent/agent.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <memory>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;

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

void fill_tools(tool::ToolRegistry& reg) {
  reg.add(tool::make_tool("get_weather", "查询指定城市的当前天气",
                          tool::SchemaBuilder().prop("city", "string", "城市名").build(),
                          [](nlohmann::json const& args,
                             tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                            auto city = tool::arg<std::string>(args, "city");
                            if (!city) return std::unexpected(city.error());
                            return nlohmann::json("晴，24°C（" + *city + "）");
                          }));

  // 故意返回一大段内容，用来触发"超长工具返回"
  reg.add(tool::make_tool("search_docs", "搜索内部文档，返回匹配片段",
                          tool::SchemaBuilder().prop("query", "string", "检索词").build(),
                          [](nlohmann::json const& args,
                             tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                            auto q = tool::arg<std::string>(args, "query");
                            if (!q) return std::unexpected(q.error());
                            std::string doc = "【" + *q + " 的检索结果】\n";
                            for (int i = 1; i <= 40; ++i)
                              doc += "片段 " + std::to_string(i) +
                                     "：这是一段较长的正文内容，用于演示超长工具返回的处理策略。\n";
                            return nlohmann::json(doc);
                          }));

  // 有副作用、且模型**愿意调用**的工具（用于演示审批中间件；
  // delete_file 这类高危工具模型会自行拒绝，走不到中间件）
  reg.add(tool::make_tool("send_email", "给指定收件人发送邮件",
                          tool::SchemaBuilder()
                              .prop("to", "string", "收件人")
                              .prop("body", "string", "正文")
                              .build(),
                          [](nlohmann::json const& args,
                             tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                            auto to = tool::arg<std::string>(args, "to");
                            if (!to) return std::unexpected(to.error());
                            return nlohmann::json("已发送给 " + *to);
                          },
                          /*read_only=*/false));

  // 有副作用的工具
  reg.add(tool::make_tool("delete_file", "删除服务器上的文件（危险操作）",
                          tool::SchemaBuilder().prop("path", "string", "文件路径").build(),
                          [](nlohmann::json const& args,
                             tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                            auto p = tool::arg<std::string>(args, "path");
                            if (!p) return std::unexpected(p.error());
                            return nlohmann::json("已删除 " + *p);
                          },
                          /*read_only=*/false));
}

// UTF-8 安全的字节截断（避免把中文切断）
std::string clip(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

// 打印一轮轨迹（role + 摘要）
void show_transcript(Agent const& agent) {
  for (auto const& m : agent.transcript()) {
    std::string const role = std::string(message::role_str(m.type));
    if (m.type == Role::Ai && !m.tool_calls.empty()) {
      for (auto const& tc : m.tool_calls)
        std::println("  [{}] 调用工具 {}({})", role, tc.name, tc.args.dump());
      if (!m.text().empty()) std::println("  [{}] {}", role, m.text());
    } else if (m.type == Role::Tool) {
      std::println("  [tool:{}] {}", m.status, clip(m.text(), 140));
    } else {
      std::println("  [{}] {}", role, clip(m.text(), 160));
    }
  }
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
  std::string const ca = ca_bundle_from_env();

  model::ModelConfig cfg;
  cfg.api_key = key;
  cfg.base_url = base;
  cfg.model = name;
  cfg.ca_info = ca;
  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::openai::OpenAIChatModel model(cfg, transport);

  tool::ToolRegistry tools;
  fill_tools(tools);
  std::println("模型: {} | 工具: {} 个\n", name, tools.size());

  // ---- ① 多步工具调用 ----
  {
    std::println("=== ① 多步工具调用 ===");
    AgentOptions opts;
    opts.system_prompt = "你是助手。需要信息时调用工具，回答保持简短。";
    Agent agent(model, tools, opts);
    auto r = agent.invoke({message::human_message("旧金山现在天气怎么样？")});
    if (!r) {
      std::println("失败: {} ({})", r.error().message, model::kind_str(r.error().kind));
      return 1;
    }
    show_transcript(agent);
    std::println("→ 迭代 {} 轮，最终: {}", agent.iterations(), r->text());
  }

  // ---- ② 超长工具返回：截断 ----
  {
    std::println("\n=== ② 超长工具返回（TruncateToolResults 500 字符）===");
    AgentOptions opts;
    opts.system_prompt = "你是助手，回答保持简短。";
    Agent agent(model, tools, opts);
    agent.add_middleware(std::make_shared<TruncateToolResults>(500));
    auto r = agent.invoke({message::human_message("搜索一下 docs 里关于超长的内容，然后一句话总结")});
    if (!r) {
      std::println("失败: {}", r.error().message);
      return 1;
    }
    show_transcript(agent);
    std::println("→ 最终: {}", r->text());
  }

  // ---- ③ 工具副作用：审批拒绝 ----
  {
    std::println("\n=== ③ 工具副作用（策略拒绝 send_email 并回喂）===");
    AgentOptions opts;
    opts.system_prompt = "你是助手。工具被拒绝时向用户说明原因，不要重试同样的调用。";
    Agent agent(model, tools, opts);
    // 策略：禁止对外发信（"有副作用且不可撤回"的典型）
    auto approval = std::make_shared<ToolApproval>(std::vector<std::string>{"send_email"});
    agent.add_middleware(approval);
    auto r = agent.invoke({message::human_message(
        "用 send_email 工具给 boss@example.com 发一封邮件，正文写「我辞职了」。")});
    if (!r) {
      std::println("失败: {}", r.error().message);
      return 1;
    }
    show_transcript(agent);
    std::println("→ 拒绝次数 {}，最终: {}", approval->denials(), r->text());
  }

  // ---- ④ 上下文压缩：LLM 摘要版 ----
  {
    std::println("\n=== ④ 上下文压缩（TrimHistory + LLM 摘要）===");
    AgentOptions opts;
    opts.system_prompt = "你是助手，回答简短。";
    Agent agent(model, tools, opts);

    // 摘要器：用同一个模型把被丢弃的历史压成一句话
    auto summarizer = [&model](std::vector<Message> const& dropped)
        -> std::expected<std::string, model::ModelError> {
      std::vector<Message> prompt{message::system_message(
          "把下面的对话压缩成不超过 60 字的摘要，只保留用户关心的事实与偏好，直接输出摘要。")};
      for (auto const& m : dropped) {
        if (m.type == Role::Ai && !m.tool_calls.empty()) continue;  // 工具细节可丢
        prompt.push_back(m);
      }
      auto r = model.invoke(prompt, model::CallOptions{});
      if (!r) return std::unexpected(r.error());
      return r->text();
    };
    auto trim = std::make_shared<TrimHistory>(6, TrimHistory::Summarizer{summarizer});
    agent.add_middleware(trim);

    // 多轮对话把历史顶上去
    std::vector<Message> input;
    for (int i = 1; i <= 8; ++i)  // 8 条 + system 1 条 = 9 > 阈值 6 → 触发压缩
      input.push_back(message::human_message("第 " + std::to_string(i) +
                                           " 条：请记住我喜欢简洁的回答。"));
    auto r = agent.invoke(input);
    if (!r) {
      std::println("失败: {}", r.error().message);
      return 1;
    }
    std::println("  压缩触发 {} 次；最终轨迹 {} 条消息", trim->compressions(),
                 agent.transcript().size());
    for (auto const& m : agent.transcript())
      if (m.type == Role::System && m.text().find("[历史摘要]") != std::string::npos)
        std::println("  摘要内容: {}", m.text().substr(0, 120));
    std::println("→ 最终: {}", r->text());
  }

  std::println("\n要点：三个卡位问题都在中间件层落地——压缩管输入、截断管工具结果、审批管副作用。");
  return 0;
}