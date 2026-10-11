// wfr-㊸ @② 测试：approve_tools 接 M10 interrupt —— 人工审批悬挂/恢复闭环
//
// 验证：NeedHuman 工具（bash 类）→ approve 节点**悬挂**（落快照 + payload 携待批请求）；
// resume action=approve → run_tools 放行真执行；reject/坏值 → 拒绝 + feedback 回喂 + 跳过；
// read_only 免审 / deny 名单硬拦（先于人审）不悬挂；guard 共存时放行的进、拒绝的不进；
// 协议不变式（AI tool_calls 后必有收口 ToolMessage）全程保持。
//
// 纪律（problems/03 §5）：断言路径真被走到 —— 用副作用计数器 + last_interrupt() + 审计。
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <memory>
#include <string>
#include <vector>

#include "cppchain/graph/graph.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/tool/exec_guard.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;
using namespace demo::fakes;
using cppchain::graph::InterruptStatus;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using cppchain::state::MemoryCheckpointer;
using workflow::WorkflowError;

namespace {

using Json = nlohmann::json;
using Res = std::expected<Json, ToolError>;

// 脚本化模型：按顺序吐预置回复（与 approve_tools_test 同款）
class ScriptedModel final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::size_t invoked = 0;

  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const&) override {
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "scripted replies exhausted", {}});
    Message m = replies.front();
    replies.pop_front();
    ++invoked;
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

Message ai_tool_calls(std::vector<ToolCall> tcs, std::string const& id = "ai1") {
  Message m = cppchain::message::ai_message("");
  for (ToolCall& tc : tcs) m.tool_calls.push_back(std::move(tc));
  if (!id.empty()) m.id = id;
  return m;
}

// NeedHuman 工具：有真实副作用（计数器），审批级别 = NeedHuman（越过静态策略，强制人审）
struct NeedHumanTool {
  int calls = 0;
  ToolSpec spec;
  NeedHumanTool() {
    spec = make_tool("dangerous_op", "危险操作（需人审）",
                     SchemaBuilder().prop("v", "string", "值").build(),
                     [this](Json const& args, ToolContext&) -> Res {
                       ++calls;
                       auto v = arg<std::string>(args, "v");
                       return Json{{"ok", true}, {"calls", calls}, {"v", v ? *v : "?"}};
                     },
                     /*read_only=*/false);
    spec.approval = ToolApprovalLevel::NeedHuman;
  }
};

ToolCall op_call(std::string id) { return ToolCall{id, "dangerous_op", Json{{"v", "x"}}}; }
ToolCall price_call(std::string id) {
  return ToolCall{id, std::string(kToolFetchPrice), Json{{"item_id", "P1"}}};
}

// 单轮 workflow：think(model) → route → approve → act(run_tools)（deny 可配）
Json sidefx_ir(std::vector<std::string> deny = {}) {
  Json d = Json::array();
  for (std::string const& s : deny) d.push_back(s);
  Json nodes = Json::array(
      {Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"tools", "ts"}, {"next", "route"}},
       Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
            {"cases", Json{{"tool_calls", "approve"}, {"invalid_tool_calls", "_fail"},
                           {"text", "_turn_end"}}},
            {"default", "_fail"}},
       Json{{"id", "approve"}, {"kind", "approve_tools"}, {"deny", d}, {"next", "act"}},
       Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "echo"}},
       Json{{"id", "echo"}, {"kind", "echo"}, {"text", "done"}, {"next", "_turn_end"}}});
  return Json{{"id", "wf_suspend"}, {"trigger", Json{{"source", "time"},
                                                     {"timing", Json{{"kind", "schedule"},
                                                                     {"schedule", "0 8 * * *"}}}}},
              {"nodes", nodes}};
}

// 假"会话日志"（LogReader 等价物；须与 guard 同寿命 —— 由 Fixture 持有）
struct FakeLog {
  bool intent = false;
  bool completed = false;
  std::string content;
  std::string status = "success";
  ExecutionGuard::LogState operator()(std::string const&) const {
    ExecutionGuard::LogState st;
    if (completed) { st.completed = true; st.content = content; st.status = status; }
    if (intent) st.intent = true;
    return st;
  }
};

std::vector<Message> messages_from_result(Json const& state) {
  std::vector<Message> out;
  auto const it = state.find("messages");
  if (it == state.end() || !it->is_array()) return out;
  for (Json const& raw : *it)
    try {
      out.push_back(raw.get<Message>());
    } catch (std::exception const&) {
    }
  return out;
}

struct Fixture {
  FakeToolSinks sinks;
  NeedHumanTool op;
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;
  std::unique_ptr<ExecutionGuard> guard;
  std::vector<std::string> guard_decisions;  // guard 审计收集
  FakeLog log;  // 会话日志等价物（必须比 guard 活得更久：guard 的 LogReader 按引用捕获）

  Fixture() {
    registry = make_fake_tools(sinks);
    registry.add(op.spec);  // needhuman 工具（handler 捕获 op，registry 持有 spec 副本）——先 add 后再取也没问题
    components.models["m"] = &model;
    components.toolsets["ts"] = {std::string(kToolFetchPrice), std::string("dangerous_op")};

    // log 未开始：guard 若进来会当"第一次执行"
    ExecutionGuard::Options go;
    go.log = [this](std::string const&) { return log(""); };
    go.audit = [this](AuditRecord const& rec) { guard_decisions.push_back(rec.decision); };
    guard = std::make_unique<ExecutionGuard>(std::move(go));
  }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    o.tool_policy = guard.get();
    return o;
  }

  // 编译（校验 + 快照 checkpointer）
  std::expected<CompiledWorkflow, std::string> give(std::vector<std::string> deny = {}) {
    auto parsed = WorkflowIR::parse(sidefx_ir(deny));
    if (!parsed) return std::unexpected(parsed.error());
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(v.describe());
    auto wf = compile_workflow(*parsed, registry, &cp, opts());
    if (!wf) return std::unexpected(wf.error().message);
    return *wf;
  }
};

}  // namespace

// ---- 悬挂 → approve 放行：真实执行 ----

TEST(WfApproveInterrupt, needhuman_suspends_then_approve_runs) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({op_call("c1")}));
  auto wf = f.give();
  ASSERT_TRUE(wf) << wf.error();

  FireOptions fo;
  fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json::object(), fo);
  ASSERT_TRUE(r1) << r1.error().message;
  InterruptStatus const pend = wf->graph.last_interrupt();
  EXPECT_TRUE(pend.interrupted);                 // 挂着
  EXPECT_EQ(pend.node, "approve");
  EXPECT_EQ(pend.payload["kind"], "approve_tools_request");
  EXPECT_EQ(pend.payload["node"], "approve");
  ASSERT_EQ(pend.payload["calls"].size(), 1u);   // 待批请求可供宿主渲染
  EXPECT_EQ(pend.payload["calls"][0]["tool"], "dangerous_op");
  EXPECT_EQ(pend.payload["calls"][0]["id"], "c1");
  EXPECT_EQ(f.op.calls, 0);                       // 悬挂期间不执行

  fo.resume = Json{{"action", "approve"}};
  auto r2 = fire_once(*wf, Json::object(), std::move(fo));
  ASSERT_TRUE(r2) << r2.error().message;
  EXPECT_FALSE(wf->graph.last_interrupt().interrupted);  // 跑完
  EXPECT_EQ(f.op.calls, 1);                        // 放行后真执行
  Json const& res = (*r2)["results"]["approve"];
  EXPECT_EQ(res["approved_ids"].size(), 1u);
  EXPECT_EQ(res["approved_ids"][0], "c1");
  EXPECT_EQ(res["denied"].size(), 0u);
  // 审计：放行的调用进 guard（未开始 → allow）
  ASSERT_EQ(f.guard_decisions.size(), 1u);
  EXPECT_EQ(f.guard_decisions[0], "allow");
  // 协议不变式
  EXPECT_FALSE(cppchain::message::validate_sequence(messages_from_result(*r2)).has_value());
}

// ---- 悬挂 → reject(feedback)：拒绝 + 反馈回喂 + 不执行 ----

TEST(WfApproveInterrupt, reject_with_feedback_skips_and_feeds_back) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({op_call("c1")}));
  auto wf = f.give();
  ASSERT_TRUE(wf);

  FireOptions fo;
  fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json::object(), fo);
  ASSERT_TRUE(r1);
  EXPECT_TRUE(wf->graph.last_interrupt().interrupted);

  fo.resume = Json{{"action", "reject"}, {"feedback", "这个不能跑"}};
  auto r2 = fire_once(*wf, Json::object(), std::move(fo));
  ASSERT_TRUE(r2);
  EXPECT_FALSE(wf->graph.last_interrupt().interrupted);
  EXPECT_EQ(f.op.calls, 0);                         // 拒绝：不执行
  Json const& res = (*r2)["results"]["approve"];
  ASSERT_EQ(res["denied"].size(), 1u);
  EXPECT_EQ(res["denied"][0]["id"], "c1");
  EXPECT_NE(res["denied"][0]["why"].get<std::string>().find("这个不能跑"), std::string::npos);
  // 模型可见的 error ToolMessage（feedback 进消息）—— content 是 TextBlock 数组
  bool saw_error = false;
  for (Json const& m : (*r2)["messages"]) {
    if (m.value("type", "") != "tool" || m.value("tool_call_id", "") != "c1" ||
        m.value("status", "") != "error")
      continue;
    auto const it = m.find("content");
    if (it != m.end() && it->is_array() && !it->empty() && (*it)[0].contains("text") &&
        (*it)[0]["text"].get<std::string>().find("这个不能跑") != std::string::npos)
      saw_error = true;
  }
  EXPECT_TRUE(saw_error) << "拒绝理由要以 error ToolMessage 回喂给模型";
  // 审计：被拒的调用不进 guard
  EXPECT_TRUE(f.guard_decisions.empty());
  // 协议不变式（AI tool_calls → 紧跟 error ToolMessage）
  EXPECT_FALSE(cppchain::message::validate_sequence(messages_from_result(*r2)).has_value());
}

// ---- 坏/缺 action 的人裁决 → fail-closed 拒绝（不执行）----

TEST(WfApproveInterrupt, ambiguous_resume_is_fail_closed_reject) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({op_call("c1")}));
  auto wf = f.give();
  ASSERT_TRUE(wf);

  FireOptions fo;
  fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json::object(), fo);
  ASSERT_TRUE(r1);
  EXPECT_TRUE(wf->graph.last_interrupt().interrupted);

  fo.resume = Json{{"hm", true}};   // 不是 {"action":"approve"}
  auto r2 = fire_once(*wf, Json::object(), std::move(fo));
  ASSERT_TRUE(r2);
  EXPECT_EQ(f.op.calls, 0);
  Json const& res = (*r2)["results"]["approve"];
  ASSERT_EQ(res["denied"].size(), 1u);
  EXPECT_NE(res["denied"][0]["why"].get<std::string>().find("人工审批拒绝"), std::string::npos);
}

// ---- read_only 免审：不悬挂直接过 ----

TEST(WfApproveInterrupt, read_only_never_suspends) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({price_call("c1")}));
  auto wf = f.give();
  ASSERT_TRUE(wf);

  FireOptions fo;
  fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json::object(), fo);
  ASSERT_TRUE(r1) << r1.error().message;
  EXPECT_FALSE(wf->graph.last_interrupt().interrupted);  // 只读工具不悬挂
  Json const& res = (*r1)["results"]["approve"];
  ASSERT_EQ(res["approved_ids"].size(), 1u);
  EXPECT_EQ(res["approved_ids"][0], "c1");
}

// ---- deny 名单硬拦：先于人审，不悬挂 ----

TEST(WfApproveInterrupt, deny_list_beats_needhuman) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({op_call("c1")}));
  auto wf = f.give({std::string("dangerous_op")});
  ASSERT_TRUE(wf);

  FireOptions fo;
  fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json::object(), fo);
  ASSERT_TRUE(r1) << r1.error().message;
  EXPECT_FALSE(wf->graph.last_interrupt().interrupted);  // deny 直接拒，无需人审
  EXPECT_EQ(f.op.calls, 0);
  Json const& res = (*r1)["results"]["approve"];
  ASSERT_EQ(res["denied"].size(), 1u);
  EXPECT_EQ(res["denied"][0]["id"], "c1");
}

// ---- 混合：静态放行(只读) + 悬挂(NeedHuman)同批 —— 整节点悬挂、恢复后都放行 ----

TEST(WfApproveInterrupt, mixed_static_approved_and_pending_approve_both) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({price_call("c1"), op_call("c2")}));
  auto wf = f.give();
  ASSERT_TRUE(wf);

  FireOptions fo;
  fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json::object(), fo);
  ASSERT_TRUE(r1);
  EXPECT_TRUE(wf->graph.last_interrupt().interrupted);
  // 待批请求只含 NeedHuman 的那个（c1 静态放行不悬挂）
  ASSERT_EQ(wf->graph.last_interrupt().payload["calls"].size(), 1u);
  EXPECT_EQ(wf->graph.last_interrupt().payload["calls"][0]["id"], "c2");

  fo.resume = Json{{"action", "approve"}};
  auto r2 = fire_once(*wf, Json::object(), std::move(fo));
  ASSERT_TRUE(r2);
  EXPECT_EQ(f.op.calls, 1);   // c2 真执行
  Json const& res = (*r2)["results"]["approve"];
  EXPECT_EQ(res["approved_ids"].size(), 2u);  // c1(只读) + c2(人批) 都放行
  EXPECT_EQ(res["approved_ids"][0], "c1");
  EXPECT_EQ(res["approved_ids"][1], "c2");
  EXPECT_EQ(res["denied"].size(), 0u);
}