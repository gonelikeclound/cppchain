// demo: history_agent —— 会话历史：thread_id + Checkpointer 的多轮记忆（M7，真机）
//
// langchain 参照：create_agent(..., thread_id, checkpointer)。同 langgraph 的
// "同 thread 续跑 = 同一段会话"，不同 thread 天然隔离。
//
// 三幕：
//   ① 同一 thread 多轮续跑 —— 发往模型的 messages 逐轮增长（历史累积），模型记得第 1 轮内容
//   ② 对照（默认无状态）—— 不带 checkpointer 的 Agent，每次请求都只有 system+human 两条
//   ③ 隔离 —— 另一个 thread 不共享 ① 的记忆
//
// 核心证明不依赖模型发挥：一个探针中间件记录"每轮真正发给模型的 messages 条数"，
// 由它直接暴露历史是否在涨；模型回答只是用户可见的佐证。
//
// 需要 .env：LLM_API_KEY / LLM_BASE_URL / LLM_MODEL（企业代理见 LLM_CA_BUNDLE）
#include <cppchain/agent/agent.hpp>
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/state/checkpointer.hpp>

#include <filesystem>
#include <initializer_list>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "support/dotenv.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::Role;

namespace {

// 探针：每次模型调用前记录消息条数。
// Agent 每轮只有一次模型调用（无工具时），故数组长度 = 已进行轮数，元素递增 = 历史在涨。
struct HistoryProbe final : agent::Middleware {
  std::vector<std::size_t> sent_sizes;
  void before_model(std::vector<Message>& msgs) override { sent_sizes.push_back(msgs.size()); }
};

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

void show_history(agent::Agent const& a) {
  for (auto const& m : a.transcript()) {
    std::string const role = std::string(message::role_str(m.type));
    std::println("      [{}] {}", role, clip(m.text(), 160));
  }
}

bool answer_mentions(std::string_view answer, std::initializer_list<std::string_view> keys) {
  for (auto const& k : keys)
    if (answer.find(k) == std::string_view::npos) return false;
  return true;
}

}  // namespace

int main() {
  auto dotenv = demo::load_dotenv({".env", "../.env", "../../.env"});
  std::string const key = demo::env_or(dotenv, "LLM_API_KEY");
  std::string const base = demo::env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
  std::string const name = demo::env_or(dotenv, "LLM_MODEL", "deepseek-chat");
  if (key.empty()) {
    std::println("缺少 LLM_API_KEY：请在 .env 或环境变量中提供（参考 .env.example）");
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

  tool::ToolRegistry const no_tools;  // 本 demo 不需要工具；具名仅为满足 Agent 生命周期要求
  std::println("模型: {} @ {}\n", name, base);

  std::string const system =
      "你是记忆测试助手。用户会告诉你个人信息，请记住；后续会考你，回答要直接、简短。";

  int failures = 0;

  // ---------------- ① 记忆：同一 thread 多轮续跑 ----------------
  {
    std::println("=== ① 同 thread（u-1）多轮续跑：历史累积 ===");
    state::MemoryCheckpointer cp;  // 必须比 Agent 活得久
    AgentOptions opts;
    opts.system_prompt = system;
    opts.checkpointer = &cp;
    opts.thread_id = "u-1";
    Agent agent(model, no_tools, opts);
    auto probe = std::make_shared<HistoryProbe>();
    agent.add_middleware(probe);

    auto t1 = agent.invoke({message::human_message("我叫小明，喜欢喝冰美式。请牢记。")});
    std::println("  T1 发往模型的 messages: {} 条", probe->sent_sizes.back());
    std::println("  当前历史："); show_history(agent);

    auto t2 = agent.invoke({message::human_message("我叫什么名字？平时爱喝什么饮品？直接回答。")});
    std::println("  T2 发往模型的 messages: {} 条（↑ 带入前面全部历史）", probe->sent_sizes.back());
    std::println("  T2 回答: {}", t2 ? clip(t2->text(), 200) : "(失败)");
    std::println("  当前历史："); show_history(agent);

    auto t3 = agent.invoke({message::human_message("我从来没跟你说过我爱喝什么，对吧？")});
    std::println("  T3 发往模型的 messages: {} 条（仍含第 1 轮）", probe->sent_sizes.back());
    std::println("  T3 回答: {}", t3 ? clip(t3->text(), 200) : "(失败)");

    bool t2_ok = t2 && answer_mentions(t2->text(), {"小明", "冰美式"});
    bool t3_ok =
        t3 && (t3->text().find("说过") != std::string::npos ||
               t3->text().find("告诉过") != std::string::npos ||
               t3->text().find("记得") != std::string::npos ||
               t3->text().find("\xe6\x98\x8e\xe6\x98\x8e") != std::string::npos);  // 「明明」
    bool growth = probe->sent_sizes.size() >= 3 && probe->sent_sizes[0] < probe->sent_sizes[1] &&
                  probe->sent_sizes[1] < probe->sent_sizes[2];
    std::println("  判定：历史逐轮增长(2→4→6) {} | T2 记得名字+饮品 {} | T3 记得我说过 {}",
                 growth ? "✓" : "✗", t2_ok ? "✓" : "✗", t3_ok ? "✓" : "✗");
    if (!growth || !t2_ok || !t3_ok) ++failures;
    // 断言历史条数=2,4,6：机械判定优先
    bool exact = probe->sent_sizes == std::vector<std::size_t>{2, 4, 6};
    std::println("  结构断言：发往模型条数恰为 [2,4,6] {}", exact ? "✓" : "✗");
    if (!exact) ++failures;
  }

  // ---------------- ② 对照：不带 checkpointer → 无状态 ----------------
  {
    std::println("\n=== ② 对照：不带 checkpointer 的 Agent（默认无状态）===");
    AgentOptions opts;
    opts.system_prompt = system;
    Agent agent(model, no_tools, opts);  // 无 checkpointer / thread
    auto probe = std::make_shared<HistoryProbe>();
    agent.add_middleware(probe);

    auto r1 = agent.invoke({message::human_message("我叫什么名字？")});
    std::println("  第 1 次请求 messages: {} 条", probe->sent_sizes.back());
    std::println("  回答: {}", r1 ? clip(r1->text(), 200) : "(失败)");

    auto r2 = agent.invoke({message::human_message("再问一遍：我叫什么名字？")});
    std::println("  第 2 次请求 messages: {} 条（仍是 2 条 —— 上一轮没被记住）", probe->sent_sizes.back());
    std::println("  回答: {}", r2 ? clip(r2->text(), 200) : "(失败)");
  }

  // ---------------- ③ 隔离：不同 thread 互不相认 ----------------
  {
    std::println("\n=== ③ 隔离：thread u-2 不共享 u-1 的记忆 ===");
    state::MemoryCheckpointer cp2;
    AgentOptions opts;
    opts.system_prompt = system;
    opts.checkpointer = &cp2;
    opts.thread_id = "u-2";
    Agent agent(model, no_tools, opts);
    auto probe = std::make_shared<HistoryProbe>();
    agent.add_middleware(probe);

    auto r = agent.invoke({message::human_message("我叫什么名字？之前有人让你记住过什么吗？")});
    std::println("  请求 messages: {} 条（u-2 是全新会话，看不到 u-1 的内容）", probe->sent_sizes.back());
    std::println("  回答: {}", r ? clip(r->text(), 200) : "(失败)");
  }

  std::println("\n=== 结果: {} ===", failures == 0 ? "全部通过 ✓" : "有失败项 ✗");
  return failures == 0 ? 0 : 1;
}