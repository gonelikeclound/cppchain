// demo: live_tools —— 真网络工具（get_weather / web_search）挂到 M7 Agent 上跑（真机）
//
// 需要 .env 提供：
//   LLM_API_KEY / LLM_BASE_URL / LLM_MODEL   —— Agent 的对话模型（如 DeepSeek）
//   WEB_SEARCH_API_KEY                        —— feedcoop 搜索 API Key（Bearer，参考 simple-agent）
//   （可选）WEB_SEARCH_BASE_URL                —— 默认 open.feedcoopapi.com/search_api/global_search
//
// 若 LLM_API_KEY 缺失，则退化为「工具直连自检」：不经过模型，直接调用 get_weather /
// web_search 的 handler，验证工具本身在真实网络下可用。也可用 LIVE_TOOLS_DIRECT=1 强制直连模式。
//
// 本 demo 同时是「状态时效性」的观测点：get_weather / web_search 都设置了 ttl，
// 说明结果会随时间过期、需重查而非凭历史推断（见 04-problems/06）。
//
// 实测：
//   · get_weather 走 Open-Meteo（免费、无需密钥）：中文/英文地名 → 经纬度 → 实时天气 + 预报
//   · web_search 走 feedcoop global search（Bearer key，返回原始文档列表，见 web_search.hpp）
#include <cppchain/agent/agent.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/tool/weather.hpp>
#include <cppchain/tool/web_search.hpp>

#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::model;
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

// UTF-8 安全的字节截断（避免把中文切断）
std::string clip(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

// 打印一轮轨迹（role + 摘要）
void show_transcript(agent::Agent const& agent) {
  for (auto const& m : agent.transcript()) {
    std::string const role = std::string(message::role_str(m.type));
    if (m.type == Role::Ai && !m.tool_calls.empty()) {
      for (auto const& tc : m.tool_calls)
        std::println("  [{}] 调用工具 {}({})", role, tc.name, tc.args.dump());
      if (!m.text().empty()) std::println("  [{}] {}", role, m.text());
    } else if (m.type == Role::Tool) {
      std::println("  [tool:{}] {}", m.status, clip(m.text(), 260));
    } else {
      std::println("  [{}] {}", role, clip(m.text(), 200));
    }
  }
}

// 不经过模型，直接调用工具 handler（LLM 密钥缺失时的自检路径）
void run_direct_tools(tool::ToolRegistry const& tools) {
  for (auto const& [name, args] :
       std::initializer_list<std::pair<std::string const, nlohmann::json>>{
           {"get_weather", {{"city", "北京"}, {"days", 1}}},
           {"web_search", {{"query", "今天 AI 领域有什么热点新闻？"}}},
       }) {
    auto const* spec = tools.find(name);
    if (!spec) {
      std::println("✗ {} 未注册", name);
      continue;
    }
    tool::ToolContext ctx;
    ctx.tool_call_id = "direct_" + name;
    auto r = spec->handler(args, ctx);
    if (r)
      std::println("✓ {} → {}", name, r->dump(2));
    else
      std::println("✗ {} → {}", name, r.error().message);
  }
}

}  // namespace

int main() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const llm_key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const llm_base = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const llm_model = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  std::string const ca = ca_bundle_from_env();

  auto transport = std::make_shared<http::CurlMultiTransport>();

  // ---- 注册真网络工具 ----
  tool::ToolRegistry tools;

  tool::WeatherToolConfig wc;
  wc.ca_info = ca;
  tools.add(tool::make_weather_tool(transport, std::move(wc)));

  tool::WebSearchConfig sc;
  std::string const ws_url =
      demo::env_or(dotenv, "WEB_SEARCH_BASE_URL",
                   "https://open.feedcoopapi.com/search_api/global_search");
  std::string const ws_key = demo::env_or(dotenv, "WEB_SEARCH_API_KEY");
  sc.base_url = ws_url;
  sc.api_key = ws_key;
  sc.ca_info = ca;
  tools.add(tool::make_web_search_tool(transport, std::move(sc)));
  // 注意：不可在 move 之后再读 sc.*（moved-from 为空串）——上面先拷贝到了本地变量

  std::println("get_weather : Open-Meteo（免费，无需密钥）");
  std::println("web_search  : {}（feedcoop，密钥 {}）", ws_url, ws_key.empty() ? "未配置" : "已加载");
  std::println("CA bundle   : {}\n", ca.empty() ? "(系统默认)" : ca);

  // 直连自检模式：显式 `LIVE_TOOLS_DIRECT=1` 或缺少 LLM 密钥时，不经过 Agent 直接调工具
  bool const direct = demo::env_or(dotenv, "LIVE_TOOLS_DIRECT") == "1" || llm_key.empty();
  if (direct) {
    std::println("{}",
                 llm_key.empty() ? "LLM_API_KEY 缺失 → 直接自检工具，不经过 Agent："
                                 : "LIVE_TOOLS_DIRECT=1 → 直接自检工具：");
    std::println();
    run_direct_tools(tools);
    return 0;
  }

  model::ModelConfig cfg;
  cfg.api_key = llm_key;
  cfg.base_url = llm_base;
  cfg.model = llm_model;
  cfg.ca_info = ca;
  model::openai::OpenAIChatModel model(cfg, transport);
  std::println("对话模型: {} @ {}\n", llm_model, llm_base);

  // ---- ① 实时天气（真实 Open-Meteo 查询）----
  {
    std::println("=== ① 实时天气：北京 ===");
    agent::AgentOptions opts;
    opts.system_prompt = "你是实时助手：需要最新信息时调用工具，回答保持简短有力。";
    agent::Agent agent(model, tools, opts);
    auto r = agent.invoke(
        {message::human_message("北京今天天气怎么样？要带伞或加衣服吗？用 get_weather 查一下。")});
    if (!r) {
      std::println("Agent 失败: {} ({})", r.error().message, model::kind_str(r.error().kind));
      return 1;
    }
    show_transcript(agent);
    std::println("→ 迭代 {} 轮\n{}", agent.iterations(), r->text());
  }

  // ---- ② 联网搜索（feedcoop global search，原始文档列表）----
  {
    std::println("\n=== ② 联网搜索：langchain 最新动态 ===");
    agent::AgentOptions opts;
    opts.system_prompt = "你是实时助手：时效性信息用 web_search 获取，回答时尽量带上来源。";
    agent::Agent agent(model, tools, opts);
    auto r = agent.invoke({message::human_message(
        "用 web_search 查一下 langchain 最近有什么重要更新，然后用一两句话总结，并给出来源链接。")});
    if (!r) {
      std::println("Agent 失败: {} ({})", r.error().message, model::kind_str(r.error().kind));
      return 1;
    }
    show_transcript(agent);
    std::println("→ 迭代 {} 轮\n{}", agent.iterations(), r->text());
  }

  return 0;
}