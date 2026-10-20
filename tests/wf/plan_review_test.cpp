// wfr-㊹ 批④ 测试：plan_review 步骤边界任务级 HITL —— continue / rollback_to / abort / 关确认门。
// 同时覆盖引擎的 goto 容器名解析（abort → goto _break 跳出 loop）。
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
#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;
using cppchain::message::Message;
using cppchain::graph::InterruptStatus;
using cppchain::state::MemoryCheckpointer;

namespace {

using Json = nlohmann::json;

class ScriptedModel final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::size_t invoked = 0;
  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const&) override {
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "replies exhausted", {}});
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

Json review_ir(long max, std::string confirm = "true") {
  return Json::parse(R"json({
    "id": "wf_rev",
    "trigger": {"source": "time", "timing": {"kind": "schedule", "schedule": "0 8 * * *"}},
    "nodes": [
      {"id": "plan", "kind": "plan", "goal": "@config.goal", "default_plan": null, "next": "loop"},
      {"id": "loop", "kind": "loop", "max": 6, "next": "_turn_end", "on_max": "_turn_end",
       "body": [
         {"id": "review", "kind": "plan_review", "confirm": "__CONFIRM__", "next": "step"},
         {"id": "step", "kind": "verify", "phase": "announce", "next": "think"},
         {"id": "think", "kind": "model", "model": "m", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"text": "judge", "invalid_tool_calls": "_fail"}, "default": "_fail"},
         {"id": "judge", "kind": "verify", "phase": "judge", "next": "_continue"}
       ]}
    ]
  })json");
}

struct Fixture {
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;
  Fixture() { components.models["m"] = &model; }

  std::expected<CompiledWorkflow, std::string> give(Json steps, long max = 6,
                                                    std::string confirm = "true") {
    Json ir = review_ir(max, confirm);
    ir["nodes"][0]["default_plan"] = steps;
    std::string const s = ir.dump();
    // 注入 confirm 字面量
    std::string replaced = s;
    auto const pos = replaced.find("__CONFIRM__");
    if (pos != std::string::npos) replaced.replace(pos, 11, confirm);
    auto parsed = WorkflowIR::parse(Json::parse(replaced));
    if (!parsed) return std::unexpected(parsed.error());
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto const v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(v.describe());
    auto wf = compile_workflow(*parsed, registry, &cp, CompileOptions{.components = &components});
    if (!wf) return std::unexpected(wf.error().message);
    return *wf;
  }
};

Json one_step() {
  return Json::parse(R"json({"steps": [{"id":"s1","goal":"说一句话","deps":[]}]})json");
}
Json two_steps() {
  return Json::parse(R"json({"steps": [
      {"id":"s1","goal":"第一步","deps":[]},
      {"id":"s2","goal":"第二步","deps":["s1"]}]})json");
}

// 到下一次 interrupt（或结果）继续 fire；返回最终状态或当前 interrupt
struct Step {
  bool end = false;
  Json state = Json::object();
  InterruptStatus pending;
};

}  // namespace

TEST(WfPlanReview, continue_passes_boundary_and_completes) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("完成一步"));
  auto wf = f.give(one_step());
  ASSERT_TRUE(wf) << wf.error();

  FireOptions fo;
  fo.thread_id = "t1";
  auto r = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  InterruptStatus const pend = wf->graph.last_interrupt();
  EXPECT_TRUE(pend.interrupted);
  EXPECT_EQ(pend.payload["kind"], "plan_review_request");
  EXPECT_EQ(pend.payload["node"], "review");

  fo.resume = Json{{"action", "continue"}};
  fo.input_messages.clear();
  auto r2 = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r2) << r2.error().message;
  EXPECT_FALSE(wf->graph.last_interrupt().interrupted);  // 跑完
  Json st = *r2;
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 1);
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "unverified");  // 无判据 → 完成但未验证
}

TEST(WfPlanReview, rollback_to_resets_step_and_redo) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("第一步完成"));
  f.model.replies.push_back(cppchain::message::ai_message("重做第一步完成"));
  auto wf = f.give(two_steps());
  ASSERT_TRUE(wf) << wf.error();

  // fire#1 → 打断于 s1 边界 → continue → s1 完成 → 打断于 s2 边界
  FireOptions fo; fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r1);
  EXPECT_EQ(wf->graph.last_interrupt().payload["kind"], "plan_review_request");
  EXPECT_EQ(wf->graph.last_interrupt().payload["plan"]["next"].get<std::string>(), "第一步");
  fo.resume = Json{{"action", "continue"}}; fo.input_messages.clear();
  auto r2 = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r2);
  Json const st2 = *r2;
  EXPECT_EQ(st2["plan"]["cursor"].get<long>(), 1) << st2["plan"].dump();

  // fire#3 → 打断于 s2 边界 → rollback_to 0 → 从第 0 步重做
  FireOptions fo2; fo2.thread_id = "t1";
  auto r3 = fire_once(*wf, Json{{"goal", "x"}}, fo2);
  if (r3) {
    // 若 r3 因上面的 continue 已经跑完（无悬挂）则直接看状态
  }
  if (wf->graph.last_interrupt().interrupted) {
    fo2.resume = Json{{"action", "rollback_to"}, {"to", 0}};
    fo2.input_messages.clear();
    r3 = fire_once(*wf, Json{{"goal", "x"}}, fo2);
    ASSERT_TRUE(r3) << r3.error().message;
  }
  Json st = *r3;
  // 回退到 0 → s1 重新做 → 最终 cursor=1、s1 unverified（无判据）、s2 pending，plan_log 含 rollback 事件
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 1) << st["plan"].dump();
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "unverified");
  EXPECT_EQ(st["plan"]["steps"][1]["status"].get<std::string>(), "pending");
  auto const lg = st["plan_log"];
  bool found = false;
  for (auto const& e : lg) if (e.value("action", "") == "rollback_to") found = true;
  EXPECT_TRUE(found) << "-- plan_log: " << lg.dump();
}

TEST(WfPlanReview, abort_stops_and_marks_cancelled) {
  Fixture f;
  auto wf = f.give(one_step());
  ASSERT_TRUE(wf) << wf.error();
  FireOptions fo; fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r1);
  EXPECT_EQ(wf->graph.last_interrupt().payload["kind"], "plan_review_request");
  fo.resume = Json{{"action", "abort"}}; fo.input_messages.clear();
  auto r2 = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r2) << r2.error().message;
  Json st = *r2;
  // 治理（problems/33）：abort 统一为取消档（cancelled），plan.terminal 由 _cancelled 落地节点写
  EXPECT_TRUE(st["plan"].value("cancelled", false)) << st["plan"].dump();
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "cancelled");
  EXPECT_EQ(st["plan"].value("terminal", ""), "cancelled") << st["plan"].dump();
  EXPECT_EQ(f.model.invoked, 0u) << "abort 后不得再调模型";
  bool found_cancel = false;
  for (auto const& e : st["plan_log"])
    if (e.is_object() && e.value("kind", "") == "cancel") found_cancel = true;
  EXPECT_TRUE(found_cancel) << st["plan_log"].dump();
}

TEST(WfPlanReview, confirm_false_passes_through_without_interrupt) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("完成"));
  auto wf = f.give(one_step(), 6, "false");
  ASSERT_TRUE(wf) << wf.error();
  FireOptions fo; fo.thread_id = "t1";
  auto r = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_FALSE(wf->graph.last_interrupt().interrupted);  // 关确认门 → 不悬挂
  EXPECT_EQ((*r)["plan"]["cursor"].get<long>(), 1);
  EXPECT_EQ(f.model.invoked, 1u);
}