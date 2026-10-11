// wfr-㊷ 集成测试：ExecutionGuard 挂进 workflow 的执行点（c_run_tools / c_tool）
//
// 验证的命题：`CompileOptions::tool_policy` 注入的 guard 真的在工具执行点生效
//（防重放 / Intent 兜底），且 guard 为 null 时行为与现状完全一致。
// 用真实副作用工具 send_notification（read_only=false，追加到收件箱）做断言——
// 数收件箱条数（磁盘断言类），不看模型嘴上说什么。
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <memory>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/tool/exec_guard.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;
using namespace demo::fakes;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using workflow::WorkflowError;

namespace {

// 脚本化模型：按顺序吐预置回复（一轮一次 invoke）
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

Message ai_tool_call(std::string name, Json const& args, std::string const& id = "c1") {
  Message m = cppchain::message::ai_message("");
  m.tool_calls.push_back(ToolCall{id, std::move(name), args});
  return m;
}

// 假"会话日志"：宿主 LogReader 的等价物（wfr 里是 .wfr/sessions 重建）
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

// 单点 workflow：think(model) → act(run_tools)，send_notification 为副作用工具
Json side_effect_ir() {
  return Json{
      {"id", "wf_side"},
      {"trigger", Json{{"source", "time"},
                       {"timing", Json{{"kind", "schedule"}, {"schedule", "0 8 * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "think"},
                          {"kind", "model"},
                          {"model", "m"},
                          {"tools", "ts"},
                          {"next", "act"}},
                     Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_turn_end"}}})}};
}

struct Fixture {
  FakeToolSinks sinks;
  tool::ToolRegistry registry = make_fake_tools(sinks);
  ScriptedModel model;
  ComponentRegistry components;
  std::unique_ptr<ExecutionGuard> guard;  // wfr-㊷：副作用防护（null = 不防护）

  Fixture() {
    components.models["m"] = &model;
    components.toolsets["ts"] = {std::string(kToolSendNotification)};
  }

  // 造一个新 guard（ExecutionGuard 含 mutex 不可赋值，故此重建而非 f.guard = …）
  void set_guard(ExecutionGuard::Options opts) {
    guard = std::make_unique<ExecutionGuard>(std::move(opts));
  }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    o.tool_policy = guard.get();  // 注入副作用防护（null = 现状行为）
    return o;
  }

  std::expected<Json, WorkflowError> run() {
    auto ir = WorkflowIR::parse(side_effect_ir());
    if (!ir) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, ir.error()});
    auto wf = compile_workflow(*ir, registry, nullptr, opts());
    if (!wf) return std::unexpected(wf.error());
    return fire_once(*wf, Json::object());
  }
};

}  // namespace

// guard 为 null（默认 Options 无 log）→ 不防护：send 正常执行一次
TEST(WfSideEffectGuard, no_guard_runs_tool_normally) {
  Fixture f;
  f.model.replies.push_back(ai_tool_call(std::string(kToolSendNotification),
                                         Json{{"title", "hello"}, {"body", "world"}}));
  auto r = f.run();
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(f.sinks.notifications.size(), 1u);  // 收件箱 1 条
  EXPECT_EQ(f.model.invoked, 1u);
}

// 未开始在日志 → 正常执行（guard 不拦头次调用）
TEST(WfSideEffectGuard, first_call_runs_when_not_in_log) {
  Fixture f;
  FakeLog log;  // intent=false, completed=false → 未开始
  f.set_guard(ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }});
  f.model.replies.push_back(ai_tool_call(std::string(kToolSendNotification),
                                         Json{{"title", "hello"}, {"body", "world"}}));
  auto r = f.run();
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(f.sinks.notifications.size(), 1u);
  EXPECT_EQ(f.guard->executions(), 1u);   // 真实执行 1 次
  EXPECT_EQ(f.guard->replays(), 0u);
}

// 已完成 → 重放，不二次执行（防重放核心）
TEST(WfSideEffectGuard, completed_replays_and_does_not_re_send) {
  Fixture f;
  FakeLog done;
  done.completed = true;
  done.content = R"({"ok":true})";
  f.set_guard(ExecutionGuard::Options{.log = [&done](std::string const& k) { return done(k); }});
  f.model.replies.push_back(ai_tool_call(std::string(kToolSendNotification),
                                         Json{{"title", "hello"}, {"body", "world"}}));
  auto r = f.run();
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(f.sinks.notifications.size(), 0u);  // 没有真的发（收件箱空 = 没副作用）
  EXPECT_EQ(f.guard->replays(), 1u);             // 走重放路径
  EXPECT_EQ(f.guard->executions(), 0u);          // handler 没被调
  // ToolMessage 是重放内容（模型能看到"已发过"）
}

// Intent 残留 + 无 probe → fail-closed：不执行、不二次发送
TEST(WfSideEffectGuard, intent_without_probe_blocks_in_workflow) {
  Fixture f;
  FakeLog log; log.intent = true;  // 崩溃缺环：有发起、无结果
  f.set_guard(ExecutionGuard::Options{.log = [&log](std::string const& k) { return log(k); }});
  f.model.replies.push_back(ai_tool_call(std::string(kToolSendNotification),
                                         Json{{"title", "hello"}, {"body", "world"}}));
  auto r = f.run();
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(f.sinks.notifications.size(), 0u);  // 不重发
  EXPECT_EQ(f.guard->blocked(), 1u);             // fail-closed
  EXPECT_EQ(f.guard->executions(), 0u);
}

// ---- replay_exec_log_reader：会话日志 → 执行状态三态（wfr 装配的事实源） ----

TEST(WfSideEffectGuard, log_reader_three_states) {
  // 未开始：日志里没有任何记录 → 读取器返回"空"（执行）
  std::vector<Message> empty;
  auto reader = replay_exec_log_reader(empty);
  ToolCall probe_call{"k", std::string(kToolSendNotification),
                      Json{{"title", "hello"}, {"body", "world"}}};
  std::string const key =
      ExecutionGuard::key_for(probe_call, ExecKeyMode::NameAndArgs);

  ExecutionGuard::LogState st0 = reader(key);
  EXPECT_FALSE(st0.intent);
  EXPECT_FALSE(st0.completed);

  // 已完成：AI(tool_call) → Tool(结果)
  Message ai = cppchain::message::ai_message("");
  ai.tool_calls.push_back(ToolCall{"c1", std::string(kToolSendNotification),
                                   Json{{"title", "hello"}, {"body", "world"}}});
  Message tool = cppchain::message::tool_message(R"({"ok":true})", "c1");
  auto reader1 = replay_exec_log_reader(std::vector<Message>{ai, tool});
  ExecutionGuard::LogState st1 = reader1(key);
  EXPECT_TRUE(st1.completed);               // 有发有回 → 已完成
  EXPECT_FALSE(st1.intent);
  EXPECT_EQ(st1.content, R"({"ok":true})"); // 重放内容
}

TEST(WfSideEffectGuard, log_reader_intent_residue) {
  // Intent 残留：AI 发了调用、没有对应 ToolMessage（崩溃缺环）
  Message ai = cppchain::message::ai_message("");
  ai.tool_calls.push_back(ToolCall{"c1", std::string(kToolSendNotification),
                                   Json{{"title", "hello"}, {"body", "world"}}});
  auto reader = replay_exec_log_reader(std::vector<Message>{ai});
  std::string const key = ExecutionGuard::key_for(
      ToolCall{"k", std::string(kToolSendNotification), Json{{"title", "hello"}, {"body", "world"}}},
      ExecKeyMode::NameAndArgs);
  ExecutionGuard::LogState st = reader(key);
  EXPECT_TRUE(st.intent);                   // 有发起、无结果 → 崩溃缺环
  EXPECT_FALSE(st.completed);
}

TEST(WfSideEffectGuard, log_reader_param_fingerprint_is_key) {
  // 键是参数指纹：参数键序不同 → 同一键（completed 命中）
  Message ai = cppchain::message::ai_message("");
  ai.tool_calls.push_back(ToolCall{"c1", std::string(kToolSendNotification),
                                   Json{{"body", "world"}, {"title", "hello"}}});  // 键序反
  Message tool = cppchain::message::tool_message(R"({"ok":true})", "c1");
  auto reader = replay_exec_log_reader(std::vector<Message>{ai, tool});
  // 用"正序"的键查 → 应命中（canonical 后同键）
  std::string const key = ExecutionGuard::key_for(
      ToolCall{"k", std::string(kToolSendNotification), Json{{"title", "hello"}, {"body", "world"}}},
      ExecKeyMode::NameAndArgs);
  ExecutionGuard::LogState st = reader(key);
  EXPECT_TRUE(st.completed);
}