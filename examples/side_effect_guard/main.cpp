// demo: side_effect_guard —— 副作用防重放（M7 收口补隐患，纯离线确定性）
//
// 风险：M6 的恢复语义是"重跑该步"，M7 的循环与任何重试都可能让副作用执行两次。
// 演示四件事：
//   ① 首次执行真的发生
//   ② 崩溃重启（日志落盘 → 新进程恢复）后同一调用被**重放**，不重复扣款
//   ③ "只有意图没有结果" → 拒绝重跑（结果未知，交人工确认）
//   ④ 两种幂等键的取舍（tool_call_id vs name+args）
#include <cppchain/agent/journal.hpp>
#include <cppchain/http/fake_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <string>

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;
using cppchain::message::ToolCall;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

http::FakeTransport::Script charge_script(std::string const& call_id) {
  http::FakeTransport::Script s;
  nlohmann::json delta = {
      {"tool_calls", nlohmann::json::array({{{"index", 0},
                                             {"id", call_id},
                                             {"function",
                                              {{"name", "charge"},
                                               {"arguments", R"({"amount":100})"}}}}})}};
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", delta}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"), kDone};
  return s;
}

http::FakeTransport::Script text_script(std::string const& text) {
  http::FakeTransport::Script s;
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"), kDone};
  return s;
}

// 一次"会话"：真实副作用计数由外部传入
std::string run_once(http::FakeTransport& transport, model::ChatModel& model,
                     tool::ToolRegistry& tools, ToolExecutionJournal& journal, int& charge_count,
                     std::string const& call_id, JournalKeyMode mode = JournalKeyMode::ToolCallId) {
  SideEffectGuard::Options opts;
  opts.key_mode = mode;
  auto guard = std::make_shared<SideEffectGuard>(journal, opts);

  AgentOptions aopts;
  aopts.system_prompt = "你是助手，回答简短。";
  Agent agent(model, tools, aopts);
  agent.add_middleware(guard);

  transport.push(charge_script(call_id));
  transport.push(text_script("已处理"));

  auto r = agent.invoke({message::human_message("扣款 100 元")});
  if (!r) return "失败: " + r.error().message;
  (void)charge_count;
  return r->text() + "（重放 " + std::to_string(guard->replays()) + " 次 / 阻塞 " +
         std::to_string(guard->blocked()) + " 次 / 真实执行 " +
         std::to_string(guard->executions()) + " 次）";
}

}  // namespace

int main() {
  // 工具：charge 有副作用；get_balance 只读
  int charge_count = 0;
  tool::ToolRegistry tools;
  tools.add(tool::make_tool("charge", "扣款（有副作用）",
                            tool::SchemaBuilder().prop("amount", "integer", "金额").build(),
                            [&charge_count](nlohmann::json const&,
                                            tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                              ++charge_count;  // 真实副作用：这里代表"钱真的被扣了"
                              return nlohmann::json("charged");
                            },
                            /*read_only=*/false));
  tools.add(tool::make_tool("get_balance", "查余额（只读）", nlohmann::json::object(),
                            [](nlohmann::json const&,
                               tool::ToolContext&) -> std::expected<nlohmann::json, tool::ToolError> {
                              return nlohmann::json("100");
                            }));

  auto transport = std::make_shared<http::FakeTransport>();
  model::ModelConfig cfg;
  cfg.api_key = "demo";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "demo-model";
  model::openai::OpenAIChatModel model(cfg, transport);

  std::filesystem::create_directories("demo-out");
  std::string const path = "demo-out/journal.json";

  // ---- ① 首次执行 ----
  std::println("=== ① 首次扣款 ===");
  {
    MemoryJournal journal;
    std::string const out = run_once(*transport, model, tools, journal, charge_count, "call_1");
    std::println("  {} | 真实扣款次数 = {}", out, charge_count);
    std::ofstream(path) << journal.dump().dump(2);
    std::println("  执行日志已落盘: {}", path);
  }

  // ---- ② 崩溃重启后重放（读回日志）----
  std::println("\n=== ② 崩溃重启后同一请求 ===");
  {
    MemoryJournal journal;
    std::ifstream in(path);
    nlohmann::json persisted;
    in >> persisted;
    journal.load(persisted);
    std::println("  从磁盘恢复 {} 条日志", journal.size());

    std::string const out = run_once(*transport, model, tools, journal, charge_count, "call_1");
    std::println("  {} | 真实扣款次数 = {}  ← 没有重复扣款", out, charge_count);
  }

  // ---- ③ 只有意图没有结果（执行中崩溃）----
  std::println("\n=== ③ 结果未知（执行中崩溃）===");
  {
    MemoryJournal journal;
    JournalEntry e;
    e.key = "call_x";
    e.tool_name = "charge";
    e.state = JournalEntry::State::Intent;  // 只有意图，没等到结果
    journal.record_intent(e);

    std::string const out = run_once(*transport, model, tools, journal, charge_count, "call_x");
    std::println("  {} | 真实扣款次数 = {}  ← 拒绝重跑，交人工确认", out, charge_count);

    auto const entry = journal.find("call_x");
    std::println("  日志状态仍为: {}（未被改写）",
                 entry && entry->state == JournalEntry::State::Intent ? "intent" : "?");
  }

  // ---- ④ 两种幂等键的取舍 ----
  std::println("\n=== ④ 幂等键模式对比（不同 id、相同参数）===");
  {
    std::println("  ToolCallId 模式：id 不同即视为两次独立操作（语义明确，但可能漏掉"
                 "模型重新规划产生的重复）");
    std::println("  ToolNameAndArgs 模式：参数相同即视为重复（能挡住重规划，代价是可能误杀"
                 "合法的重复调用，如\"查两次天气\"）");
    std::println("  → 见 tests/agent/journal_test.cpp 的对照用例（各有断言）");
  }

  std::println("\n结论：副作用防重放的关键是**幂等键 + 三态判定**——"
               "已完成就重放、结果未知就拒跑、没见过才执行。");
  return charge_count == 1 ? 0 : 1;  // 全程真实扣款必须只有 1 次
}