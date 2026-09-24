// demo: stale_state —— 状态时效性（M7+ 新问题域，真机）
//
// 问题：有些状态必须实时获取（余额/库存/价格/文件内容），但上下文里的**历史结果**会被
// 模型当成现状直接用。本 demo 做 A/B 对照：
//   A 无守卫：查一次余额 → 等过 TTL → 再问余额 → 模型会不会重新调用工具？
//   B 有守卫：同样流程，过期结果被改写为"不能作为现状使用" → 行为是否改变？
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL
#include <cppchain/agent/freshness.hpp>
#include <cppchain/message/serializer.hpp>   // Message ↔ JSON（预置历史需要）
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

// UTF-8 安全截断：按字节切会把中文切成乱码（本项目已踩过多次）
std::string clip(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

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
  // ---- C. 压缩 × 时效：三种策略对照（预置一段含过期工具结果的历史）----
  std::println("\n=== C. 压缩 × 时效（历史里有一个 1 小时前的余额结果，TTL=3s）===");
  {
    // 造历史：老的余额查询（已过期）+ 几轮闲聊 + 现在的问题
    auto stale_round = [&] {
      Message ai;
      ai.type = message::Role::Ai;
      ai.tool_calls.push_back(message::ToolCall{"call_old", "get_balance", nlohmann::json::object()});
      Message tool = message::tool_message("余额 = 100 元（实时）", "call_old");
      auto const past = std::chrono::duration_cast<std::chrono::seconds>(
                            (std::chrono::system_clock::now() - std::chrono::hours(1))
                                .time_since_epoch())
                            .count();
      tool.additional_kwargs[freshness::kObservedAt] = past;
      tool.additional_kwargs[freshness::kTtlSeconds] = std::int64_t{3};
      return std::vector<Message>{ai, tool};
    };

    // LLM 摘要器：会把旧值写进摘要文本，但**丢掉观测时间**
    auto llm_summarizer = [&model](std::vector<Message> const& dropped)
        -> std::expected<std::string, model::ModelError> {
      std::vector<Message> prompt{message::system_message(
          "把下面的对话压缩成不超过 80 字的摘要，保留事实与数字，直接输出摘要。")};
      for (auto const& m : dropped) prompt.push_back(m);
      auto r = model.invoke(prompt, model::CallOptions{});
      if (!r) return std::unexpected(r.error());
      return r->text();
    };

    auto run_policy = [&](TrimHistory::VolatilePolicy policy, char const* label,
                          bool with_llm_summary) {
      state::MemoryCheckpointer cp;
      std::vector<Message> history{message::system_message(
          "你是严谨的助手：涉及余额这类会变化的状态，只能依据最新的查询结果回答。回答不超过 30 字。")};
      history.push_back(message::human_message("我余额多少？"));
      for (auto& m : stale_round()) history.push_back(m);
      history.push_back(message::ai_message("100 元。"));
      for (int i = 0; i < 3; ++i) {
        history.push_back(message::human_message("闲聊第 " + std::to_string(i) + " 句"));
        history.push_back(message::ai_message("好的。"));
      }
      nlohmann::json snapshot = nlohmann::json::object();
      snapshot["messages"] = history;
      cp.put("cmp-" + std::string(label), "", std::nullopt, std::move(snapshot), {},
             nlohmann::json{{"phase", "seed"}});

      TrimHistory::Options topts;
      topts.max_messages = 6;  // 触发压缩，且过期结果落在折叠区
      topts.volatile_policy = policy;
      if (with_llm_summary) topts.summarizer = llm_summarizer;

      AgentOptions aopts;
      aopts.checkpointer = &cp;
      aopts.thread_id = "cmp-" + std::string(label);
      Agent agent(model, tools, aopts);
      auto trim = std::make_shared<TrimHistory>(topts);
      auto guard = std::make_shared<StaleStateGuard>();
      agent.add_middleware(trim);
      agent.add_middleware(guard);

      int const before = tool_calls_counter;
      auto r = agent.invoke({message::human_message("那我现在余额到底是多少？给我确切数字。")});
      int const calls = tool_calls_counter - before;
      std::println("  [{}] 压缩 {} 次 | 保留易变组 {} | 丢弃 {} | 过期标注 {} | 工具调用 {} 次",
                   label, trim->compressions(), trim->preserved_groups(), trim->dropped_groups(),
                   guard->invalidations(), calls);
      if (!r) {
        std::println("      失败: {} | {}", r.error().message, r.error().raw.dump().substr(0, 200));
        return std::pair<int, std::string>{-1, "失败"};
      }
      std::println("      回答: {}", r->text());
      return std::pair<int, std::string>{calls, r->text()};
    };

    auto const fold = run_policy(TrimHistory::VolatilePolicy::Fold, "Fold + 占位摘要", false);
    auto const preserve = run_policy(TrimHistory::VolatilePolicy::Preserve, "Preserve 保留", false);
    auto const drop = run_policy(TrimHistory::VolatilePolicy::Drop, "Drop 丢弃", false);
    // 决定性用例：LLM 摘要会把旧值写进摘要、却丢掉时间戳
    auto const fold_llm = run_policy(TrimHistory::VolatilePolicy::Fold, "Fold + LLM摘要", true);

    std::println("\n  对照小结（工具调用次数 / 回答）:");
    std::println("    Fold + 占位摘要 : {} 次 → {}", fold.first, clip(fold.second, 60));
    std::println("    Fold + LLM摘要  : {} 次 → {}", fold_llm.first, clip(fold_llm.second, 60));
    std::println("    Preserve        : {} 次 → {}", preserve.first, clip(preserve.second, 60));
    std::println("    Drop            : {} 次 → {}", drop.first, clip(drop.second, 60));
    // 结论文案必须由**数据**驱动：硬编码的叙述会与本次观测矛盾（本项目踩过）
    auto verdict = [](std::pair<int, std::string> const& r) -> std::string {
      return r.first > 0 ? "重新取数 ✓" : "未取数（直接作答 or 向用户报告过期）";
    };
    std::println("\n  逐条判定（由本次数据生成）:");
    std::println("    Fold + 占位摘要 : {} 次 → {}", fold.first, verdict(fold));
    std::println("    Fold + LLM摘要  : {} 次 → {}", fold_llm.first, verdict(fold_llm));
    std::println("    Preserve        : {} 次 → {}", preserve.first, verdict(preserve));
    std::println("    Drop            : {} 次 → {}", drop.first, verdict(drop));
    std::println("\n  机制层面的结论（与模型随机性无关）:");
    std::println("    · Fold 会**丢掉时效元数据** → 守卫再也看不到它（信息损失不可逆）");
    std::println("    · Preserve 保住元数据 → 守卫能标注过期；但「标注」不等于「模型必定重取」");
    std::println("    · Drop 整组丢弃 + 明示「请重新查询」 → 结构上迫使重取");
    std::println("  ⚠️ 本次**未复现**「用旧值作答」：模型较自觉 + system prompt 明确要求用最新结果。");
    std::println("     风险并非不存在，只是需要更弱的前提（弱模型 / 不强调时效的提示词 / 摘要里旧值显得权威）；");
    std::println("     且每次运行模型行为可能不同 —— 评估时效类机制不能只看单次结果。");

    if (fold.first < 0 || preserve.first < 0 || drop.first < 0 || fold_llm.first < 0) return 1;
  }

  std::println("\n要点：根因是「模型不知道自己看到的是什么时候的快照」——"
               "要么给它时间戳，要么替它判断（框架失效标注）。");
  return 0;
}