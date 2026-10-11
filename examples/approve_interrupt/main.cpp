// demo: wfr-㊸ @② approve_interrupt —— workflow 级人工审批 HITL（interrupt 悬挂/恢复）
//
// 验证主线：NeedHuman 工具（bash）被 approve_tools 悬挂 → 落快照 + payload 携待批请求 →
// 外部给裁决（这里用确定性答复模拟 wfr 的交互循环）→ resume → 节点重跑、interrupt() 返回
// 裁决 → action=approve 放行（bash 真执行，executor 经 ToolNode 注入）、reject 拒绝不执行。
// 对应 wfr 的真机交互循环见 src/wfr/main.cpp（"批准？[approve | reject <理由> | abort]"）。
#include <cppchain/message/message.hpp>
#include <cppchain/state/checkpointer.hpp>
#include <cppchain/tool/bash_tool.hpp>
#include <cppchain/tool/executor.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/components.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/validator.hpp>

#include <deque>
#include <print>

#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;
using cppchain::graph::InterruptStatus;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using cppchain::state::MemoryCheckpointer;
using Json = nlohmann::json;

namespace {

// 脚本化模型：按顺序吐预置回复（demo 用，避开真端点）
class ScriptedModel final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const&) override {
    Message m = replies.front();
    replies.pop_front();
    return m;
  }
  std::generator<message::MessageChunk> stream(std::span<Message const>,
                                               model::CallOptions const&) override {
    co_return;
  }
  core::CancelToken stream_async(std::span<Message const>, model::CallOptions const&,
                                 model::StreamHandlers) override {
    return {};
  }
  model::Task<std::expected<Message, model::ModelError>> invoke_async(std::span<Message const>,
                                                                      model::CallOptions) override {
    return {};
  }
};

Message ai_bash(char const* id, std::string command) {
  Message m = cppchain::message::ai_message("");
  ToolCall c;
  c.id = id;
  c.name = "bash";
  c.args = Json{{"command", std::move(command)}};
  m.tool_calls.push_back(std::move(c));
  return m;
}

}  // namespace

int main() {
  // 工具：真实 bash（PlainSpawnExecutor 执行后端 + NeedHuman 人审）+ 只读价查询
  FakeToolSinks sinks;
  tool::ToolRegistry registry = make_fake_tools(sinks);
  auto spawn = tool::make_plain_spawn_executor();
  registry.add(tool::make_bash_tool(spawn.get()));

  ScriptedModel model;
  model.replies.push_back(ai_bash("cc-a", "echo hello review"));        // approve 场景
  model.replies.push_back(ai_bash("cc-b", "echo this should not run")); // reject 场景

  ComponentRegistry comps;
  comps.models["m"] = &model;
  comps.toolsets["ts"] = {"bash"};

  // 单轮 workflow：think → route → approve → act(run_tools)
  Json ir = Json{{"id", "approve_interrupt_demo"},
                 {"nodes",
                  Json::array(
                      {Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"tools", "ts"},
                            {"next", "route"}},
                       Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
                            {"cases", Json{{"tool_calls", "approve"}, {"invalid_tool_calls", "_fail"},
                                           {"text", "_turn_end"}}},
                            {"default", "_fail"}},
                       Json{{"id", "approve"}, {"kind", "approve_tools"}, {"next", "act"}},
                       Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "echo"}},
                       Json{{"id", "echo"}, {"kind", "echo"}, {"text", "🔧 $act.result.summary"},
                            {"next", "_turn_end"}}})}};

  auto parsed = WorkflowIR::parse(ir);
  if (!parsed) { std::println("IR 解析失败: {}", parsed.error()); return 1; }
  ValidatorOptions vopts;
  vopts.registry = &registry;
  vopts.components = &comps;
  auto v = validate(*parsed, vopts);
  if (!v.ok()) { std::println("校验失败: {}", v.describe()); return 1; }

  MemoryCheckpointer cp;  // 进程内快照：悬挂/恢复的前提（interrupt 需要 checkpointer + thread）
  auto compiled = compile_workflow(*parsed, registry, &cp, CompileOptions{.components = &comps});
  if (!compiled) { std::println("编译失败: {}", compiled.error().message); return 1; }
  auto wf = std::move(*compiled);

  // fire 一次 + （如悬挂）一个人工裁决再 resume —— 对应 wfr 的交互循环
  auto fire_with_review = [&](std::string thread,
                              std::optional<Json> resume) -> std::expected<Json, WorkflowError> {
    FireOptions fo;
    fo.thread_id = std::move(thread);
    fo.resume = std::move(resume);
    auto r = fire_once(wf, Json::object(), std::move(fo));
    if (!r) return std::unexpected(r.error());
    return *r;
  };

  // ---- 场景 A：人工批准 → bash 真执行 ----
  std::println("== 场景 A：basher 请求 + 人工批准 ==");
  auto ra = fire_with_review("t-a", std::nullopt);
  if (!ra) { std::println("fire 失败: {}", ra.error().message); return 1; }
  InterruptStatus const penda = wf.graph.last_interrupt();
  if (!penda.interrupted) { std::println("（未悬挂 —— 预期 bash 应被 NeedHuman 拦截）"); return 1; }
  std::println("  悬挂: node={}  待批: {}", penda.node,
               penda.payload["calls"][0]["args"]["command"].get<std::string>());
  std::println("  → 裁决: action=approve（真机对应 wfr 交互输入）");
  ra = fire_with_review("t-a", Json{{"action", "approve"}});
  if (!ra) { std::println("resume 失败: {}", ra.error().message); return 1; }
  std::println("  跑完（不再悬挂 = {})", !wf.graph.last_interrupt().interrupted ? "true" : "false");
  std::println("  工具摘要: {}", (*ra)["results"]["act"]["summary"].dump());
  std::println("  approve 记录: approved={} denied={}",
               (*ra)["results"]["approve"]["approved_ids"].dump(),
               (*ra)["results"]["approve"]["denied"].dump());

  // ---- 场景 B：人工拒绝 → bash 不执行（反馈回喂） ----
  std::println();
  std::println("== 场景 B：basher 请求 + 人工拒绝 ==");
  auto rb = fire_with_review("t-b", std::nullopt);
  if (!rb) { std::println("fire 失败: {}", rb.error().message); return 1; }
  InterruptStatus const pendb = wf.graph.last_interrupt();
  if (!pendb.interrupted) { std::println("（未悬挂）"); return 1; }
  std::println("  悬挂: node={}  待批: {}", pendb.node,
               pendb.payload["calls"][0]["args"]["command"].get<std::string>());
  std::println("  → 裁决: action=reject, feedback=样例人工拒绝");
  rb = fire_with_review("t-b", Json{{"action", "reject"}, {"feedback", "样例人工拒绝"}});
  if (!rb) { std::println("resume 失败: {}", rb.error().message); return 1; }
  std::println("  approve 记录: denied={}", (*rb)["results"]["approve"]["denied"].dump());
  std::println("  工具摘要: {}（拒绝的调用被 run_tools 过滤，不执行）",
               (*rb)["results"]["act"]["summary"].dump());

  std::println();
  std::println("要点：NeedHuman 工具的副作用从静态策略升级为「悬挂 → 人审 → 放行/拒绝」；");
  std::println("     拒绝理由以 error ToolMessage 回喂模型（可见可自纠）；真机循环在 wfr CLI。");
  return 0;
}