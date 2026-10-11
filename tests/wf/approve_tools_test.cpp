// wfr-㊷ 测试：approve_tools 节点（副作用审批决策层）
//
// 验证：deny 名单拒绝 → error ToolMessage 回喂 + run_tools 跳过被拒调用（不执行）；
// read_only 免审；全 deny → run_tools 过滤后空、不崩；guard 共存时被拒不二次执行；
// results 发布（$approve.result.approved_ids / denied 可读）；校验（未知/只读工具名报错）。
//
// 纪律（problems/03 §5）：断言路径真被走到 —— 用收件箱/磁盘 + 计数，不看模型嘴上。
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <memory>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/tool/exec_guard.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;
using namespace demo::fakes;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using workflow::WorkflowError;

namespace {

// 脚本化模型：按顺序吐预置回复
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

// 单轮 workflow：think(model) → route → approve → act(run_tools)
Json sidefx_ir(Json deny, bool with_echo = false) {
  Json nodes = Json::array(
      {Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"tools", "ts"}, {"next", "route"}},
       Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
            {"cases", Json{{"tool_calls", "approve"}, {"invalid_tool_calls", "_fail"},
                           {"text", "_turn_end"}}},
            {"default", "_fail"}},
       Json{{"id", "approve"}, {"kind", "approve_tools"}, {"deny", deny}, {"next", "act"}},
       Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "done"}},
       Json{{"id", "done"}, {"kind", "echo"}, {"text", "⭕ $approve.result.approved_ids.length 个放行"}, {"next", "_turn_end"}}});
  if (!with_echo) nodes[4] = Json{{"id", "done"}, {"kind", "echo"}, {"text", "done"}, {"next", "_turn_end"}};
  return Json{{"id", "wf_sidefx"}, {"trigger", Json{{"source", "time"},
                                                    {"timing", Json{{"kind", "schedule"},
                                                                    {"schedule", "0 8 * * *"}}}}},
              {"nodes", nodes}};
}

// 把 result JSON 的 messages 反序列化（validate_sequence 需要 Message 向量）
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
  ToolRegistry registry = make_fake_tools(sinks);
  ScriptedModel model;
  ComponentRegistry components;
  std::unique_ptr<ExecutionGuard> guard;

  Fixture() {
    components.models["m"] = &model;
    components.toolsets["ts"] = std::vector<std::string>{std::string(kToolSendNotification), std::string(kToolFetchPrice)};
  }

  void set_guard(ExecutionGuard::Options opts) {
    guard = std::make_unique<ExecutionGuard>(std::move(opts));
  }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    o.tool_policy = guard.get();
    return o;
  }

  // 编译失败时返回左值（WorkflowError），成功则 fire
  std::expected<Json, WorkflowError> build_and_fire(Json ir) {
    auto parsed = WorkflowIR::parse(ir);
    if (!parsed) {
      return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    }
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto v = validate(*parsed, vopts);
    if (!v.ok())
      return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, v.describe()});
    auto wf = compile_workflow(*parsed, registry, nullptr, opts());
    if (!wf) return std::unexpected(wf.error());
    return fire_once(*wf, Json::object());
  }
};

ToolCall send_call(std::string id) {
  return ToolCall{id, std::string(kToolSendNotification), Json{{"title", "hello"}, {"body", "x"}}};
}
ToolCall price_call(std::string id) {
  return ToolCall{id, std::string(kToolFetchPrice), Json{{"item_id", "P1"}}};
}

// 假"会话日志"：LogReader 等价物（wfr 里是 .wfr/sessions 重建）
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

}  // namespace

// deny send_notification：approve 产 error ToolMessage、act 跳过它（收件箱空 = 没真发）
TEST(WfApproveTools, deny_blocks_and_act_skips) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({send_call("c1")}));
  f.model.replies.push_back(cppchain::message::ai_message("被拒了，不能发"));

  auto r = f.build_and_fire(sidefx_ir(Json::array({std::string(kToolSendNotification)})));
  ASSERT_TRUE(r) << r.error().message;

  // denied 有 error ToolMessage，收件箱空（没真发），协议完整
  EXPECT_EQ(f.sinks.notifications.size(), 0u);
  Json const& results = (*r)["results"]["approve"];
  ASSERT_EQ(results["denied"].size(), 1u);
  EXPECT_EQ(results["denied"][0]["tool"], kToolSendNotification);
  EXPECT_EQ(results["approved_ids"].size(), 0u);
  // messages 里应有 error ToolMessage（tool_call_id=c1, status=error）
  bool saw_error = false;
  for (Json const& m : (*r)["messages"]) {
    auto const t = m.value("type", "");
    if (t == "tool" && m.value("tool_call_id", "") == "c1" && m.value("status", "") == "error")
      saw_error = true;
  }
  EXPECT_TRUE(saw_error) << "approve 应产 error ToolMessage 让模型看到被拒";
  // 协议不变式
  auto const msgs_v = messages_from_result(*r);
  EXPECT_FALSE(cppchain::message::validate_sequence(msgs_v).has_value());
}

// read_only 免审：deny 名单里那只读工具 → 正常执行（不 deny）
TEST(WfApproveTools, read_only_tool_not_denied) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({price_call("c1")}));  // fetch_price 只读
  f.model.replies.push_back(cppchain::message::ai_message("查到了"));

  // deny 名单**包含** fetch_price —— 但它是只读 → 校验应报"deny 只读工具是死配置"
  auto r = f.build_and_fire(sidefx_ir(Json::array({std::string(kToolFetchPrice)})));
  ASSERT_FALSE(r) << "deny 名单含只读工具应被 VN17 拒绝";
}

// 放行路径：未 deny 的副作用工具正常执行
TEST(WfApproveTools, non_denied_runs_normally) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({send_call("c1")}));
  f.model.replies.push_back(cppchain::message::ai_message("发了"));

  auto r = f.build_and_fire(sidefx_ir(Json::array()));
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(f.sinks.notifications.size(), 1u);  // 真发了
  Json const& results = (*r)["results"]["approve"];
  EXPECT_EQ(results["approved_ids"].size(), 1u);
  EXPECT_EQ(results["approved_ids"][0], "c1");
  EXPECT_EQ(results["denied"].size(), 0u);
}

// 混合：c1 放行、c2 deny → c1 真执行、c2 被拒；协议完整
TEST(WfApproveTools, mixed_approve_and_deny) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({price_call("c1"), send_call("c2")}));
  f.model.replies.push_back(cppchain::message::ai_message("c1 查了，c2 被拒"));

  auto r = f.build_and_fire(sidefx_ir(Json::array({std::string(kToolSendNotification)})));
  ASSERT_TRUE(r) << r.error().message;
  Json const& results = (*r)["results"]["approve"];
  EXPECT_EQ(results["approved_ids"].size(), 1u);
  EXPECT_EQ(results["approved_ids"][0], "c1");
  EXPECT_EQ(results["denied"].size(), 1u);
  EXPECT_EQ(results["denied"][0]["id"], "c2");
  auto const msgs_v = messages_from_result(*r);
  EXPECT_FALSE(cppchain::message::validate_sequence(msgs_v).has_value());
}

// 全 deny → run_tools 过滤后空，不崩，正常走 next（echo 打印 0 个放行）
TEST(WfApproveTools, all_denied_act_does_not_crash) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({send_call("c1"), send_call("c2")}));
  f.model.replies.push_back(cppchain::message::ai_message("都没发"));

  auto r = f.build_and_fire(sidefx_ir(Json::array({std::string(kToolSendNotification)})));
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(f.sinks.notifications.size(), 0u);
  Json const& results = (*r)["results"]["approve"];
  EXPECT_EQ(results["approved_ids"].size(), 0u);
  EXPECT_EQ(results["denied"].size(), 2u);
}

// guard 共存：被拒的调用不进 guard（executions/审计不增加、不产第二条 ToolMessage）
TEST(WfApproveTools, guard_does_not_execute_denied_call) {
  Fixture f;
  FakeLog log;  // 模拟会话历史"未见过"该调用 —— guard 若进来会当第一次执行
  f.set_guard(ExecutionGuard::Options{.log = [&log](std::string const&) { return log(""); }});
  f.model.replies.push_back(ai_tool_calls({send_call("c1")}));
  f.model.replies.push_back(cppchain::message::ai_message("被拒"));

  auto r = f.build_and_fire(sidefx_ir(Json::array({std::string(kToolSendNotification)})));
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(f.sinks.notifications.size(), 0u);
  EXPECT_EQ(f.guard->executions(), 0u);  // 关键：denied 根本没进 guard
  EXPECT_EQ(f.guard->replays(), 0u);
  // c1（也就是 send）只有 approve 那一条 error ToolMessage，没有第二条执行结果
  std::size_t tool_msgs = 0;
  for (Json const& m : (*r)["messages"])
    if (m.value("type", "") == "tool") ++tool_msgs;
  EXPECT_EQ(tool_msgs, 1u);
}

// 校验：deny 未知工具名 → VN17 报错
TEST(WfApproveTools, validator_rejects_unknown_deny) {
  Fixture f;
  auto r = f.build_and_fire(sidefx_ir(Json::array({"nosuch_tool"})));
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("VN17"), std::string::npos) << r.error().message;
}

// 校验：approve 缺 next → VN17 报错
TEST(WfApproveTools, validator_rejects_missing_next) {
  Fixture f;
  Json ir = sidefx_ir(Json::array());
  ir["nodes"][2].erase("next");
  auto parsed = WorkflowIR::parse(ir);
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  vopts.components = &f.components;
  auto v = validate(*parsed, vopts);
  ASSERT_FALSE(v.ok()) << v.describe();
  EXPECT_NE(v.describe().find("VN17"), std::string::npos) << v.describe();
}