// demo: tools_approval —— file_write 副作用审批端到端（M7，纯离线确定性）
//
// 用**真实的内置文件工具** file_write（read_only=false）验证两件事：
//   ① 审批（ToolApproval）：执行前拦 —— 名单拒绝 / 回调按参数判定（只允许写 notes/ 子目录）
//   ② 防重放（SideEffectGuard + FileJournal）：同一 tool_call_id 崩溃重启后**重放**，不重复写盘
// 纵贯对比：只读工具（file_read）既不进审批名单也不记账 → 正常执行。
//
// 硬断言全部基于**真实磁盘状态**（文件在不在、内容对不对），不是看模型嘴上说什么。
// langchain 参照：LangGraph 的 interrupt 审批前身 + 官方"副作用必须幂等"的约束。
#include <cppchain/agent/agent.hpp>
#include <cppchain/agent/journal.hpp>
#include <cppchain/http/fake_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/tool/file_tools.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;

namespace fs = std::filesystem;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

// 模型第一轮：发起一个文件工具调用
http::FakeTransport::Script tool_script(std::string const& call_id, std::string const& name,
                                        std::string const& args) {
  http::FakeTransport::Script s;
  nlohmann::json delta = {{"tool_calls", nlohmann::json::array({{{"index", 0},
                                                                 {"id", call_id},
                                                                 {"function",
                                                                  {{"name", name},
                                                                   {"arguments", args}}}}})}};
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", delta}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"), kDone};
  return s;
}

// 模型第二轮：看到（成功/拒绝/重放）结果后给出最终答复
http::FakeTransport::Script text_script(std::string const& text) {
  http::FakeTransport::Script s;
  s.chunks = {sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}}.dump()),
              sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"), kDone};
  return s;
}

std::string clip(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

// 一次会话：模型发工具调用 → （审批/重放/真实执行）→ 模型给最终答复。
// mws 传入一次会话要挂的中间件（各自带计数，事后读）。
Message run_turn(http::FakeTransport& transport, model::ChatModel& model, tool::ToolRegistry& tools,
                 std::vector<std::shared_ptr<Middleware>> mws, std::string const& user,
                 http::FakeTransport::Script call, http::FakeTransport::Script reply) {
  AgentOptions aopts;
  aopts.system_prompt = "你是助手，遵守系统策略，回答简短。";
  Agent agent(model, tools, aopts);
  for (auto& mw : mws) agent.add_middleware(mw);

  transport.push(std::move(call));
  transport.push(std::move(reply));
  auto const r = agent.invoke({message::human_message(user)});
  return r ? *r : message::ai_message("(失败: " + r.error().message + ")");
}

// 回调审批：只允许路径以 notes/ 开头（演示"按参数判定"，比名单更细）
ToolApproval::Decider notes_only_decider() {
  return [](message::ToolCall const& call, std::string& why) {
    std::string const path = call.args.value("path", std::string{});
    if (path.starts_with("notes/")) return true;
    why = "只允许写入沙箱内 notes/ 目录（收到路径 '" + path + "'）";
    return false;
  };
}

int g_failures = 0;

void check(bool cond, std::string const& label) {
  std::println("  {} {}", cond ? "✓" : "✗", label);
  if (!cond) ++g_failures;
}

}  // namespace

int main() {
  // ---- 沙箱与日志：每次从干净状态开始（防上一次运行污染对照）----
  fs::path const sandbox = "demo-out/tools_approval_sandbox";
  std::string const journal_path = "demo-out/tools_approval_journal.json";
  std::error_code ec;
  fs::remove_all(sandbox, ec);
  fs::remove(journal_path, ec);
  fs::create_directories(sandbox);

  tool::FileToolsConfig fcfg;
  fcfg.root_dir = sandbox;
  tool::ToolRegistry tools;
  tools.add(tool::make_file_write_tool(fcfg));  // read_only=false：审批 + 防重放
  tools.add(tool::make_file_read_tool(fcfg));   // read_only=true：都不该记账

  auto transport = std::make_shared<http::FakeTransport>();
  model::ModelConfig cfg;
  cfg.api_key = "demo";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "demo-model";
  model::openai::OpenAIChatModel model(cfg, transport);

  std::println("tools_approval：file_write 副作用审批端到端（沙箱 {}）", sandbox.generic_string());
  std::println("  工具: file_write<read_only=false> + file_read<read_only=true>\n");

  // ---- ① 名单拒绝：策略禁止 file_write，模型无论如何调都会被拦 ----
  std::println("=== ① 策略拒绝（名单）===");
  {
    MemoryJournal journal;
    auto guard = std::make_shared<SideEffectGuard>(journal);
    auto approval = std::make_shared<ToolApproval>(std::vector<std::string>{"file_write"});
    auto out = run_turn(
        *transport, model, tools, {approval, guard},
        "把这条计划写进 denied.txt",
        tool_script("call_deny", "file_write", R"({"path":"denied.txt","content":"x"})"),
        text_script("明白，我放弃写文件，遵守系统策略。"));
    std::println("  模型答复: {}", clip(out.text(), 90));
    check(approval->denials() == 1, "ToolApproval 拒绝 1 次");
    check(guard->executions() == 0, "SideEffectGuard 未记录任何执行（拒绝不代表执行）");
    check(!fs::exists(sandbox / "denied.txt"), "denied.txt 未创建（磁盘无副作用）");
  }

  // ---- ② 回调按参数判定：只许写 notes/；恶意路径被拦、合法路径真的写入 ----
  std::println("\n=== ② 回调审批（只允许 notes/ 子目录）===");
  {
    FileJournal journal(journal_path);  // 持久化：供下一段"重启"演示
    auto guard = std::make_shared<SideEffectGuard>(journal);
    auto approval = std::make_shared<ToolApproval>(notes_only_decider());

    auto ok_turn = run_turn(
        *transport, model, tools, {approval, guard},
        "把计划写到 notes/plan.md",
        tool_script("call_ok", "file_write", R"({"path":"notes/plan.md","content":"v1"})"),
        text_script("已写入 notes/plan.md。"));
    std::println("  模型答复: {}", clip(ok_turn.text(), 80));

    auto deny_turn = run_turn(
        *transport, model, tools, {approval, guard},
        "把信息写到根目录 root.txt",
        tool_script("call_deny2", "file_write", R"({"path":"root.txt","content":"x"})"),
        text_script("抱歉，root.txt 不在允许目录，我换个地方写。"));
    std::println("  模型答复: {}", clip(deny_turn.text(), 80));

    check(approval->denials() == 1, "越界路径 root.txt 被回调拒绝 1 次");
    check(guard->executions() == 1, "合法写入真实执行 1 次");
    check(fs::exists(sandbox / "notes" / "plan.md"), "notes/plan.md 确实写盘");
    check(!fs::exists(sandbox / "root.txt"), "root.txt 未创建（拒绝后无副作用）");
    std::ifstream in(sandbox / "notes" / "plan.md");
    std::string content((std::istreambuf_iterator<char>(in)), {});
    check(content == "v1", "计划内容为 v1");
  }

  // ---- ③ 崩溃重启后防重放：同一 tool_call_id → 重放，绝不重复写盘 ----
  std::println("\n=== ③ 重启后同一调用（重放，不重复执行）===");
  {
    FileJournal journal(journal_path);  // 构造即从磁盘恢复 ② 的记录
    std::println("  启动时自动恢复 {} 条日志", journal.size());
    auto guard = std::make_shared<SideEffectGuard>(journal);
    auto approval = std::make_shared<ToolApproval>(notes_only_decider());

    auto out = run_turn(
        *transport, model, tools, {approval, guard},
        "再写一次计划（同一次调用重放）",
        // 同一 id "call_ok"，但参数不同（v2）——若重跑就会写成 v2，重放则保持 v1
        tool_script("call_ok", "file_write", R"({"path":"notes/plan.md","content":"v2"})"),
        text_script("这次调用与上次相同，不再重复写入。"));
    std::println("  模型答复: {}", clip(out.text(), 80));

    check(guard->replays() == 1 && guard->executions() == 0,
          "重放 1 次、真实执行 0 次（幂等键 = tool_call_id）");
    std::ifstream in(sandbox / "notes" / "plan.md");
    std::string content((std::istreambuf_iterator<char>(in)), {});
    check(content == "v1", "文件内容仍是 v1（v2 没有被写进去，证明副作用未重放）");
  }

  // ---- ④ 只读工具对照：file_read 不记账、也不进审批名单 ----
  std::println("\n=== ④ 只读工具对照（file_read 不记账 / 不进名单）===");
  {
    MemoryJournal journal;
    auto guard = std::make_shared<SideEffectGuard>(journal);
    auto approval = std::make_shared<ToolApproval>(std::vector<std::string>{"file_write"});
    auto out = run_turn(
        *transport, model, tools, {approval, guard},
        "读一下 notes/plan.md",
        tool_script("call_read", "file_read", R"({"path":"notes/plan.md"})"),
        text_script("计划内容我看了，是 v1。"));
    std::println("  模型答复: {}", clip(out.text(), 60));
    check(approval->denials() == 0, "file_read 不在拒绝名单 → 正常放行");
    check(guard->executions() == 0 && guard->replays() == 0, "只读工具不记账（未污染日志）");
  }

  // ---- 磁盘终态一览 ----
  std::println("\n=== 磁盘终态 ===");
  {
    tool::ToolContext ctx;
    ctx.tool_call_id = "final";
    auto list = tool::make_list_dir_tool(fcfg).handler(nlohmann::json{{"path", "."}}, ctx);
    std::println("  沙箱内容: {}", list ? list->dump() : "(列目录失败)");
  }

  std::println("\n=== {} ===",
               g_failures == 0 ? "全部通过 ✓（审批 + 防重放 + 只读对照，真实磁盘断言）"
                               : ("有 " + std::to_string(g_failures) + " 项失败 ✗"));
  return g_failures == 0 ? 0 : 1;
}