// demo: stale_state —— 状态时效性（M7+ 新问题域，真机）
//
// 问题：有些状态必须实时获取（余额/库存/价格/文件内容），但上下文里的**历史结果**会被
// 模型当成现状直接用。本 demo 做 A/B 对照：
//   A 无守卫：查一次余额 → 等过 TTL → 再问余额 → 模型会不会重新调用工具？
//   B 有守卫：同样流程，过期结果被改写为"不能作为现状使用" → 行为是否改变？
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL
#include <cppchain/agent/stale.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/state/checkpointer.hpp>

#include <chrono>
#include <memory>
#include <print>
#include <string>
#include <thread>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::state::MemoryCheckpointer;

namespace {

int tool_calls_counter = 0;  // 真实工具调用次数（A/B 对照的关键指标）

std::string ca_bundle() {
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

struct RunResult {
  int tool_calls = 0;
  std::string answer;
  bool saw_stale_marker = false;
  bool failed = false;   // 区分"模型没调工具"与"请求本身失败"（否则结论会误导）
};

// 一轮 A/B：同一个 thread，先查余额 → 等过 TTL → 再问余额
RunResult run_case(model::ChatModel& model, bool use_guard, tool::ToolRegistry& tools,
                   std::chrono::seconds ttl, int ttl_seconds_for_report) {
  MemoryCheckpointer cp;
  AgentOptions opts;
  opts.system_prompt = "你是严谨的助手：涉及余额、数量、价格这类会变化的状态，"
                       "只能依据**最新**的查询结果回答。回答不超过 30 字。";
  opts.checkpointer = &cp;
  opts.thread_id = use_guard ? "with-guard" : "no-guard";
  Agent agent(model, tools, opts);

  std::shared_ptr<StaleStateGuard> guard;
  if (use_guard) {
    guard = std::make_shared<StaleStateGuard>();
    agent.add_middleware(guard);
  }

  RunResult out;
  int const before = tool_calls_counter;
  auto r1 = agent.invoke({message::human_message("我账户余额是多少？")});
  if (!r1) {
    out.failed = true;
    out.answer = "失败: " + r1.error().message + " | " + r1.error().raw.dump().substr(0, 300);
    return out;
  }
  std::println("  [第1轮] 工具调用 {} 次 | 回答: {}", tool_calls_counter - before, r1->text());

  std::println("  …等待 {} 秒让结果过期…", ttl_seconds_for_report);
  std::this_thread::sleep_for(ttl);

  int const mid = tool_calls_counter;
  auto r2 = agent.invoke({message::human_message("那我现在余额到底是多少？给我确切数字。")});
  if (!r2) {
    out.failed = true;
    out.answer = "失败: " + r2.error().message + " | " + r2.error().raw.dump().substr(0, 300);
    return out;
  }
  out.tool_calls = tool_calls_counter - mid;
  out.answer = r2->text();
  if (guard) out.saw_stale_marker = guard->invalidations() > 0;
  return out;
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
  cfg.ca_info = ca_bundle();
  auto transport = std::make_shared<http::CurlMultiTransport>();
  model::openai::OpenAIChatModel model(cfg, transport);

  // 余额工具：声明 TTL = 3 秒（易变状态）
  auto balance = tool::make_tool("get_balance", "查询账户当前余额（会随时间变化）",
                                 nlohmann::json::object(),
                                 [](nlohmann::json const&,
                                    tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                                   ++tool_calls_counter;
                                   // 每次查询返回不同的值：便于判断"到底有没有重新查"
                                   return nlohmann::json("余额 = " +
                                                         std::to_string(100 + tool_calls_counter) +
                                                         " 元（实时）");
                                 });
  balance.ttl = std::chrono::seconds(3);
  tool::ToolRegistry tools;
  tools.add(std::move(balance));

  std::println("模型: {} | 工具 get_balance 声明 TTL=3 秒\n", name);

  std::println("=== A. 无时效守卫 ===");
  auto const a = run_case(model, /*use_guard=*/false, tools, std::chrono::seconds(4), 4);
  std::println("  → 第2轮工具调用 {} 次 | 回答: {}", a.tool_calls, a.answer);

  std::println("\n=== B. 有 StaleStateGuard ===");
  auto const b = run_case(model, /*use_guard=*/true, tools, std::chrono::seconds(4), 4);
  std::println("  → 第2轮工具调用 {} 次 | 过期标注生效: {} | 回答: {}", b.tool_calls,
               b.saw_stale_marker ? "是" : "否", b.answer);

  if (a.failed || b.failed) {
    std::println("\n⚠️ 有 case 请求失败，本次不构成有效对照（结论留待成功运行后再记）");
    return 1;
  }

  std::println("\n观察结论（如实记录，模型行为可能每次不同）：");
  std::println("  · A 无守卫：第2轮工具调用 {} 次 —— {}",
               a.tool_calls,
               a.tool_calls == 0 ? "模型直接用历史结果作答（正是本问题域要治的病症）"
                                 : "模型自己重新查了（模型有自觉性，但不可依赖）");
  std::println("  · B 有守卫：第2轮工具调用 {} 次 —— {}", b.tool_calls,
               b.tool_calls > 0 ? "过期标注促使模型重新取数"
                                : "模型仍用旧值（守卫不够，需要更强的手段，如强制刷新）");
  std::println("\n要点：根因是「模型不知道自己看到的是什么时候的快照」——"
               "要么给它时间戳，要么替它判断（框架失效标注）。");
  return 0;
}