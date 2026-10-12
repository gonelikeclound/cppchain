// wfr-㊹ 批① 测试：任务级 P-E-V 环的 **L1 档**（plan + execute，无验证；arch 19 §三之一）。
// 覆盖：固定 default_plan 完整跑通（cursor 推进 + plan_log 落痕 + 步骤指令进 messages）、
// 坏 plan 校验拒绝、verify 带 kind（>L1）时 judge 显式 fail 不静默推进、plan 节点模型生成档。
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

// L1 环 JSON（plan：default_plan 两步骤无 verify → loop{announce, model(无工具), route, judge}）
Json l1_ir(Json default_plan) {
  Json skeleton = nlohmann::json::parse(R"json({
    "id": "wf_l1",
    "trigger": {"source": "time", "timing": {"kind": "schedule", "schedule": "0 8 * * *"}},
    "nodes": [
      {"id": "plan", "kind": "plan", "goal": "@config.goal", "default_plan": null, "next": "loop"},
      {"id": "loop", "kind": "loop", "max": 2, "next": "_turn_end", "on_max": "_turn_end",
       "body": [
         {"id": "step", "kind": "verify", "phase": "announce", "next": "think"},
         {"id": "think", "kind": "model", "model": "m", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"text": "judge", "tool_calls": "act", "invalid_tool_calls": "_fail"},
          "default": "_fail"},
         {"id": "act", "kind": "run_tools", "next": "judge"},
         {"id": "judge", "kind": "verify", "phase": "judge", "next": "_continue"}
       ]}
    ]
  })json");
  skeleton["nodes"][0]["default_plan"] = std::move(default_plan);
  return skeleton;
}

Json two_steps() {
  return Json::parse(R"json({"steps": [
      {"id": "s1", "goal": "说第一句话", "deps": []},
      {"id": "s2", "goal": "补第二句话", "deps": ["s1"]}
    ]})json");
}

struct Fixture {
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;

  Fixture() { components.models["m"] = &model; }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    return o;
  }

  // 编译（走 validator，校验 plan 的 default_plan）
  std::expected<CompiledWorkflow, std::string> give(Json default_plan) {
    auto parsed = WorkflowIR::parse(l1_ir(std::move(default_plan)));
    if (!parsed) return std::unexpected(parsed.error());
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto const v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(v.describe());
    auto wf = compile_workflow(*parsed, registry, &cp, opts());
    if (!wf) return std::unexpected(wf.error().message);
    return *wf;
  }
};

Json final_state(Json const& state) { return state; }

}  // namespace

TEST(WfPlanL1, two_steps_execute_without_verify_and_advance_cursor) {
  Fixture f;
  // 每步一次模型调用（无工具 → text → judge）
  f.model.replies.push_back(cppchain::message::ai_message("第一步完成"));
  f.model.replies.push_back(cppchain::message::ai_message("第二步完成"));
  auto wf = f.give(two_steps());
  ASSERT_TRUE(wf) << wf.error();

  FireOptions fo;
  fo.thread_id = "t1";
  Json config{{"goal", "做一个小任务"}};
  auto const r = fire_once(*wf, std::move(config), fo);
  ASSERT_TRUE(r) << r.error().message;

  Json const st = *r;
  Json const plan = st["plan"];
  ASSERT_EQ(plan["cursor"].get<long>(), 2) << "无验证 → 两步都做完并推进 cursor";
  ASSERT_EQ(plan["steps"].size(), 2u);
  EXPECT_EQ(plan["steps"][0]["status"].get<std::string>(), "done");
  EXPECT_EQ(plan["steps"][1]["status"].get<std::string>(), "done");
  // plan_log：两步各一条 L1 判定（kind=none, verdict=done）
  ASSERT_EQ(st["plan_log"].size(), 2u);
  EXPECT_EQ(st["plan_log"][0]["kind"].get<std::string>(), "none");
  EXPECT_EQ(st["plan_log"][0]["verdict"].get<std::string>(), "done");
  EXPECT_EQ(st["plan_log"][1]["step"].get<std::string>(), "s2");
  // 步骤指令进了 messages（announce 把每步目标喂给模型）
  auto const msgs = st["messages"];
  std::string joined;
  for (Json const& m : msgs)
    if (m.value("type", "") == "human")
      for (Json const& b : m["content"]) joined += b.value("text", "");
  EXPECT_NE(joined.find("执行步骤 1/2 [s1]"), std::string::npos) << joined;
  EXPECT_NE(joined.find("执行步骤 2/2 [s2]"), std::string::npos) << joined;
}

TEST(WfPlanL1, bad_default_plan_is_rejected_by_validate) {
  Fixture f;
  Json bad = Json{{"steps", Json::array({Json{{"id", "s1"}}})}};  // 缺 goal
  auto wf = f.give(std::move(bad));
  EXPECT_FALSE(wf) << "缺 goal 的步骤必须被 VN20 拒";
  EXPECT_NE(wf.error().find("goal"), std::string::npos);
}

TEST(WfPlanL1, verify_kind_beyond_l1_fails_explicitly_and_does_not_advance) {
  // L1 环不该出现带 verify 的步骤；若出现（编辑计划误配），judge 显式 fail 且不推进 cursor
  Fixture f;
  Json steps = two_steps();
  steps["steps"][0]["verify"] = Json{{"kind", "model"}, {"criteria", "看似完成"}};  // L3 档（批③ 未实现）
  f.model.replies.push_back(cppchain::message::ai_message("做了"));
  f.model.replies.push_back(cppchain::message::ai_message("做了"));
  auto wf = f.give(std::move(steps));
  ASSERT_TRUE(wf) << wf.error();

  FireOptions fo;
  fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json const st = *r;
  Json const plan = st["plan"];
  EXPECT_EQ(plan["cursor"].get<long>(), 0) << "verify kind 未实现 → 不推进";
  EXPECT_EQ(plan["steps"][0]["status"].get<std::string>(), "failed");
  EXPECT_EQ(st["plan_log"][0]["verdict"].get<std::string>(), "fail");
  EXPECT_NE(st["plan_log"][0]["why"].get<std::string>().find("未实现"), std::string::npos);
}

TEST(WfPlanL1, plan_node_model_generation_adopts_generated_steps) {
  Fixture f;
  // 第一次模型调用 = plan 节点生成计划（返回 JSON）；随后两步各一次。
  f.model.replies.push_back(cppchain::message::ai_message(
      R"({"steps":[{"id":"s1","goal":"生成的一步","deps":[]}]})"));
  f.model.replies.push_back(cppchain::message::ai_message("完成"));
  auto parsed = WorkflowIR::parse(Json::parse(R"json({
    "id": "wf_gen",
    "trigger": {"source": "time", "timing": {"kind": "schedule", "schedule": "0 8 * * *"}},
    "nodes": [
      {"id": "plan", "kind": "plan", "model": "m", "next": "loop"},
      {"id": "loop", "kind": "loop", "max": 1, "next": "_turn_end",
       "body": [
         {"id": "step", "kind": "verify", "phase": "announce", "next": "think"},
         {"id": "think", "kind": "model", "model": "m", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"text": "judge"}, "default": "_fail"},
         {"id": "judge", "kind": "verify", "phase": "judge", "next": "_continue"}
       ]}
    ]
  })json"));
  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  vopts.components = &f.components;
  auto const v = validate(*parsed, vopts);
  ASSERT_TRUE(v.ok()) << v.describe();
  auto wf = compile_workflow(*parsed, f.registry, &f.cp, f.opts());
  ASSERT_TRUE(wf) << wf.error().message;

  FireOptions fo;
  fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"goal", "生成一个计划"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json const st = *r;
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 1);
  EXPECT_EQ(st["plan"]["steps"][0]["goal"].get<std::string>(), "生成的一步");
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "done");
}