// wfr-㊾ 批① 测试：mch 三角色 workflow —— Planner / Executor / 独立 Reviewer 终审。
// 覆盖三件事：
//   1) 角色身份落审计：judge 落行带 role（executor 自查）、review 落行带 role（reviewer 独立终审）；
//   2) 批级终审的输入隔离：review 只读 计划 + 各步判定（plan_log），不读执行对话/思考；
//   3) fail-closed：review 模型缺失 / 返回不可解析 → 判 fail（绝不当作通过）。
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <memory>
#include <optional>
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
  std::vector<std::string> last_humans;  // 每次 invoke 的最后一条 human 文本（供审查输入窗口审计）
  std::expected<Message, model::ModelError> invoke(std::span<Message const> msgs,
                                                   model::CallOptions const&) override {
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "replies exhausted", {}});
    Message m = replies.front();
    replies.pop_front();
    ++invoked;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
      if (it->type == message::Role::Human) { last_humans.push_back(it->text()); break; }
    }
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

// mch 环：2 步带 model 判据 → loop 后 review 批级终审（nodes 里 __REV__ 由 run 时替换）。
Json mch_ir() {
  return Json::parse(R"json({
    "id": "wf_mch",
    "nodes": [
      {"id": "plan", "kind": "plan", "goal": "@config.goal",
       "system_prompt": "你是【策划者 Planner】",
       "default_plan": {"steps": [
          {"id":"s1","goal":"查天气写盘","deps":[],"verify":{"kind":"model","criteria":"有数字"}},
          {"id":"s2","goal":"给穿衣建议","deps":["s1"],"verify":{"kind":"model","criteria":"基于数据"}}
       ]},
       "next": "loop"},
      {"id": "loop", "kind": "loop", "max": 3, "next": "review", "on_max": "review",
       "body": [
         {"id": "step",  "kind": "verify", "phase": "announce", "next": "think"},
         {"id": "think", "kind": "model", "model": "m",
          "system_prompt": "你是【执行者 Executor】", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"text": "judge", "tool_calls": "act", "invalid_tool_calls": "_fail"},
          "default": "_fail"},
         {"id": "act", "kind": "run_tools", "next": "judge"},
         {"id": "judge", "kind": "verify", "phase": "judge", "model": "m",
          "system_prompt": "你是【步骤自查员】", "role": "executor", "next": "_continue"}
       ]},
      {"id": "review", "kind": "verify", "phase": "review", "model": "__REV__",
       "system_prompt": "你是【独立审查员 Reviewer】", "role": "reviewer",
       "criteria": "@config.mch_criteria", "next": "final"},
      {"id": "final", "kind": "model", "model": "m",
       "system_prompt": "你是【解说者】", "next": "_turn_end"}
    ]
  })json");
}

struct Fixture {
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;
  Fixture() { components.models["m"] = &model; }
};

// 编译 + fire 一次，返回 state；断言前置失败即 nullopt。
// 约定脚本顺序：think(s1) / judge(s1) / think(s2) / judge(s2) / [review] / final。
std::optional<Json> run_mch(Fixture& f, std::string const& rev_model = "m",
                            std::string const& criteria = "整批验收") {
  Json ir = mch_ir();
  std::string raw = ir.dump();
  auto const pos = raw.find("__REV__");
  if (pos != std::string::npos) raw.replace(pos, 7, rev_model);
  auto parsed = WorkflowIR::parse(Json::parse(raw));
  EXPECT_TRUE(parsed);
  if (!parsed) return std::nullopt;
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  vopts.components = &f.components;
  auto const v = validate(*parsed, vopts);
  EXPECT_TRUE(v.ok()) << v.describe();
  if (!v.ok()) return std::nullopt;
  auto wf = compile_workflow(*parsed, f.registry, &f.cp,
                             CompileOptions{.components = &f.components});
  EXPECT_TRUE(wf) << (wf ? "" : wf.error().message);
  if (!wf) return std::nullopt;
  FireOptions fo;
  fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"goal", "x"}, {"mch_criteria", criteria}}, fo);
  EXPECT_TRUE(r) << (r ? "" : r.error().message);
  if (!r) return std::nullopt;
  return std::optional<Json>{*r};
}

void push_happy_script(Fixture& f, std::string const& review_reply,
                       bool include_review = true) {
  f.model.replies.push_back(cppchain::message::ai_message("s1 执行完毕。"));
  f.model.replies.push_back(cppchain::message::ai_message(R"({"pass":true,"feedback":"有数字"})"));
  f.model.replies.push_back(cppchain::message::ai_message("s2 执行完毕。"));
  f.model.replies.push_back(cppchain::message::ai_message(R"({"pass":true,"feedback":"基于数据"})"));
  if (include_review) f.model.replies.push_back(cppchain::message::ai_message(review_reply));
  f.model.replies.push_back(cppchain::message::ai_message("解说：两步都完成。"));
}

}  // namespace

TEST(WfMch, roles_land_in_plan_log_and_review_sees_plan_only) {
  Fixture f;
  push_happy_script(f, R"({"pass":true,"feedback":"整批验收通过"})");
  auto st = run_mch(f);
  ASSERT_TRUE(st);
  // —— 阶段一：角色身份串落审计 ——
  int executor_rows = 0, reviewer_rows = 0;
  for (auto const& e : (*st)["plan_log"]) {
    if (!e.contains("kind")) continue;
    if (e["kind"] == "model") {
      EXPECT_EQ(e.value("role", ""), "executor") << "judge 行应记 role=executor\n" << e.dump();
      ++executor_rows;
    }
    if (e["kind"] == "review") {
      EXPECT_EQ(e.value("role", ""), "reviewer") << "review 行应记 role=reviewer\n" << e.dump();
      EXPECT_EQ(e.value("verdict", ""), "done");
      ++reviewer_rows;
    }
  }
  EXPECT_EQ(executor_rows, 2);
  EXPECT_EQ(reviewer_rows, 1);
  EXPECT_EQ((*st)["plan"]["cursor"].get<long>(), 2);  // 两步都 done

  // —— 阶段二：批级终审输入隔离 ——
  // review 的 human 输入 = 验收标准 + 任务目标 + 【步骤与判定】；**不含**执行者思考正文
  //（不出现 announce 步骤文本 "📋 执行步骤"）。judge/think 的输入才是执行对话。
  std::string review_in;
  for (auto it = f.model.last_humans.rbegin(); it != f.model.last_humans.rend(); ++it) {
    if (it->find("【步骤与判定】") != std::string::npos) { review_in = *it; break; }
  }
  ASSERT_FALSE(review_in.empty()) << "review 输入应含【步骤与判定】";
  EXPECT_NE(review_in.find("整批验收"), std::string::npos) << review_in;   // 判据来自 config
  EXPECT_NE(review_in.find("查天气写盘"), std::string::npos) << review_in;  // 步骤目标可见
  EXPECT_EQ(review_in.find("📋 执行步骤"), std::string::npos)
      << "review 不应看到执行对话/思考窗口\n" << review_in;
}

TEST(WfMch, review_fail_closed_when_model_missing) {
  // review 节点不配 model → 批级终审判 fail（不静默通过）。
  Fixture f;
  push_happy_script(f, R"({"pass":true,"feedback":"x"})", /*include_review=*/true);
  auto st = run_mch(f, /*rev_model=*/"");
  ASSERT_TRUE(st);
  bool found_review_fail = false;
  for (auto const& e : (*st)["plan_log"]) {
    if (e.contains("kind") && e["kind"] == "review") {
      EXPECT_EQ(e.value("verdict", ""), "fail");
      EXPECT_NE(e.value("why", "").find("review 模型未配置"), std::string::npos);
      found_review_fail = true;
    }
  }
  EXPECT_TRUE(found_review_fail) << (*st)["plan_log"].dump();
}

TEST(WfMch, review_fail_closed_on_unparsable_reply) {
  // review 返回非 JSON → 判 fail（fail-closed，不当作通过）。
  Fixture f;
  push_happy_script(f, "我觉得整批还行");
  auto st = run_mch(f);
  ASSERT_TRUE(st);
  bool found_review_fail = false;
  for (auto const& e : (*st)["plan_log"]) {
    if (e.contains("kind") && e["kind"] == "review") {
      EXPECT_EQ(e.value("verdict", ""), "fail");
      EXPECT_NE(e.value("why", "").find("未返回可解析"), std::string::npos);
      found_review_fail = true;
    }
  }
  EXPECT_TRUE(found_review_fail) << (*st)["plan_log"].dump();
}