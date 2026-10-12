// wfr-㊹ 批③ 测试：L3 LLM 档 —— verify kind=model 接裁判模型（Pass/Fail + feedback，fail-closed）。
// 覆盖：裁判 pass → done 推进 + feedback 落 plan_log；裁判 fail → 不推进 + 理由回喂；
// judge 缺模型 / 返回不可解析 → fail-closed（不当作 pass）。
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

// L3 环：1 步带 verify kind=model；judge 节点复用同一模型 m（脚本按序吐 step 回复 + judge 回复）
Json l3_ir(std::string const& judge_model) {
  std::string const raw = R"json({
    "id": "wf_l3",
    "trigger": {"source": "time", "timing": {"kind": "schedule", "schedule": "0 8 * * *"}},
    "nodes": [
      {"id": "plan", "kind": "plan", "goal": "@config.goal", "default_plan": null, "next": "loop"},
      {"id": "loop", "kind": "loop", "max": 1, "next": "_turn_end", "on_max": "_turn_end",
       "body": [
         {"id": "step", "kind": "verify", "phase": "announce", "next": "think"},
         {"id": "think", "kind": "model", "model": "m", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"text": "judge", "tool_calls": "act", "invalid_tool_calls": "_fail"},
          "default": "_fail"},
         {"id": "act", "kind": "run_tools", "next": "judge"},
         {"id": "judge", "kind": "verify", "phase": "judge", "model": "__JUDGE__", "next": "_continue"}
       ]}
    ]
  })json";
  std::string out = raw;
  auto const pos = out.find("__JUDGE__");
  if (pos != std::string::npos) out.replace(pos, 9, judge_model);
  return Json::parse(out);
}

}  // namespace

struct Fixture {
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;
  Fixture() { components.models["m"] = &model; }
};

TEST(WfPlanL3, judge_pass_advances_and_lists_feedback) {
  Fixture f;
  Json ir = l3_ir("m");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"给一句北京天气结论","deps":[],
       "verify":{"kind":"model","criteria":"结论要有数字"}}]})json");
  f.model.replies.push_back(cppchain::message::ai_message("北京今天最高 19 度。"));
  f.model.replies.push_back(cppchain::message::ai_message(R"({"pass":true,"feedback":"有数字、简洁"})"));
  auto parsed = WorkflowIR::parse(std::move(ir));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  vopts.components = &f.components;
  auto const v = validate(*parsed, vopts);
  ASSERT_TRUE(v.ok()) << v.describe();
  auto wf = compile_workflow(*parsed, f.registry, &f.cp,
                             CompileOptions{.components = &f.components});
  ASSERT_TRUE(wf) << wf.error().message;

  FireOptions fo;
  fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 1);
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "done");
  EXPECT_EQ(st["plan_log"][0]["kind"].get<std::string>(), "model");
  EXPECT_EQ(st["plan_log"][0]["verdict"].get<std::string>(), "done");
  EXPECT_NE(st["plan_log"][0]["why"].get<std::string>().find("有数字、简洁"), std::string::npos) << st["plan_log"][0]["why"];
}

TEST(WfPlanL3, judge_fail_does_not_advance_and_feedback_returns) {
  Fixture f;
  Json ir = l3_ir("m");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"给一句北京天气结论","deps":[],
       "verify":{"kind":"model","criteria":"要引用来源"}}]})json");
  f.model.replies.push_back(cppchain::message::ai_message("北京今天 19 度。"));
  f.model.replies.push_back(
      cppchain::message::ai_message(R"({"pass":false,"feedback":"没给来源，补上"})"));
  auto parsed = WorkflowIR::parse(std::move(ir));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  vopts.components = &f.components;
  auto const v = validate(*parsed, vopts);
  ASSERT_TRUE(v.ok()) << v.describe();
  auto wf = compile_workflow(*parsed, f.registry, &f.cp,
                             CompileOptions{.components = &f.components});
  ASSERT_TRUE(wf) << wf.error().message;
  FireOptions fo;
  fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 0);   // 裁判不通过 → 不推进
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "failed");
  EXPECT_EQ(st["plan_log"][0]["verdict"].get<std::string>(), "fail");
  EXPECT_NE(st["plan_log"][0]["why"].get<std::string>().find("没给来源"), std::string::npos) << st["plan_log"][0]["why"];
}

TEST(WfPlanL3, judge_unparsed_reply_is_fail_closed) {
  // judge 回复不是 JSON / 缺 pass → 按失败处理（绝不当作通过）
  Fixture f;
  Json ir = l3_ir("m");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"给一句结论","deps":[],
       "verify":{"kind":"model","criteria":"x"}}]})json");
  f.model.replies.push_back(cppchain::message::ai_message("结论。"));
  f.model.replies.push_back(cppchain::message::ai_message("我觉得写得还行"));
  auto parsed = WorkflowIR::parse(std::move(ir));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  vopts.components = &f.components;
  auto const v = validate(*parsed, vopts);
  ASSERT_TRUE(v.ok()) << v.describe();
  auto wf = compile_workflow(*parsed, f.registry, &f.cp,
                             CompileOptions{.components = &f.components});
  ASSERT_TRUE(wf) << wf.error().message;
  FireOptions fo;
  fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "failed");
  EXPECT_NE(st["plan_log"][0]["why"].get<std::string>().find("未返回可解析"), std::string::npos) << st["plan_log"][0]["why"];
}

TEST(WfPlanL3, judge_missing_model_is_fail_closed) {
  // verify 节点没配 model（judge 模型缺失）→ 步 fail（不静默通过）
  Fixture f;
  Json ir = l3_ir("");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"给一句结论","deps":[],
       "verify":{"kind":"model","criteria":"x"}}]})json");
  f.model.replies.push_back(cppchain::message::ai_message("结论。"));
  auto parsed = WorkflowIR::parse(std::move(ir));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  vopts.components = &f.components;
  auto const v = validate(*parsed, vopts);
  ASSERT_TRUE(v.ok()) << v.describe();
  auto wf = compile_workflow(*parsed, f.registry, &f.cp,
                             CompileOptions{.components = &f.components});
  ASSERT_TRUE(wf) << wf.error().message;
  FireOptions fo;
  fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "failed");
  EXPECT_NE(st["plan_log"][0]["why"].get<std::string>().find("judge 模型未配置"), std::string::npos);
}