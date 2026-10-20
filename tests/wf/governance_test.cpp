// wfr-㊹ 治理（problems/33）· governed 全链单 workflow 测试。
// 覆盖终态四档 + UNVERIFIED 双档：unverified 推进 / judge fail→_fail / on_max→_timeout /
// plan_review abort→_cancelled / happy path 全链 / review fail→_fail。
// 内联 governed IR（不依赖 share 文件，测试自洽）；ScriptedModel 驱动；
// s1 的 probe 判据用「预建文件」实现 done（脚本不回工具调用，text 直过）。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <generator>
#include <memory>
#include <optional>
#include <random>
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

// 内联 governed IR：s1 带 probe（预建文件 → done）、s2 无判据（unverified）。
// plan_review confirm 可注入（false = 关确认门，不经 HITL）；approve 走 `deny:[]` + 脚本 text 直过。
Json governed_ir(std::string confirm = "false", long max = 6) {
  std::string s = R"json({
    "id": "wf_gov",
    "nodes": [
      {"id": "recall", "kind": "load_history",
       "path": "@config.workspace", "session": "@config.session", "next": "begin"},
      {"id": "begin", "kind": "task", "task": "@config.task", "phase": "begin", "next": "plan"},
      {"id": "plan", "kind": "plan", "goal": "@config.goal",
       "default_plan": {"steps": [
          {"id": "s1", "goal": "写 .out/gov_artifact.md 含 gov", "deps": [],
           "verify": {"kind": "probe", "path": ".out/gov_artifact.md", "contains": "gov"}},
          {"id": "s2", "goal": "一句话结论", "deps": ["s1"]}
       ]},
       "next": "loop"},
      {"id": "loop", "kind": "loop", "max": __MAX__, "next": "review", "on_max": "_timeout",
       "body": [
         {"id": "hitl", "kind": "plan_review", "confirm": "__CONFIRM__", "next": "step"},
         {"id": "step", "kind": "verify", "phase": "announce", "next": "think"},
         {"id": "think", "kind": "model", "model": "m", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"text": "judge", "tool_calls": "approve", "invalid_tool_calls": "_fail"},
          "default": "_fail"},
         {"id": "approve", "kind": "approve_tools", "deny": [], "next": "act"},
         {"id": "act", "kind": "run_tools", "next": "judge"},
         {"id": "judge", "kind": "verify", "phase": "judge", "model": "m",
          "system_prompt": "自查", "role": "executor", "next": "jgate"},
         {"id": "jgate", "kind": "expr",
          "expr": "$judge.result.outcome == \"fail\"", "then": "_fail", "else": "_continue"}
       ]},
      {"id": "review", "kind": "verify", "phase": "review", "model": "m",
       "system_prompt": "独立审查员", "role": "reviewer",
       "criteria": "@config.mch_criteria", "next": "gate"},
      {"id": "gate", "kind": "expr",
       "expr": "$review.result.verdict == \"fail\"", "then": "_fail", "else": "final"},
      {"id": "final", "kind": "model", "model": "m", "system_prompt": "解说", "next": "end"},
      {"id": "end", "kind": "task", "task": "@config.task", "phase": "end", "next": "_turn_end"}
    ]
  })json";
  auto const p = s.find("__MAX__");
  if (p != std::string::npos) s.replace(p, 7, std::to_string(max));
  auto const c = s.find("__CONFIRM__");
  if (c != std::string::npos) s.replace(c, 11, confirm);
  return Json::parse(s);
}

struct WorkspaceDir {
  std::filesystem::path dir;
  WorkspaceDir()
      : dir(std::filesystem::temp_directory_path() /
           ("wf_gov_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::error_code ec;
    std::filesystem::create_directories(dir / ".out", ec);
  }
  ~WorkspaceDir() { std::filesystem::remove_all(dir); }
};

struct Fixture {
  WorkspaceDir ws;
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;
  Fixture() { components.models["m"] = &model; }

  std::optional<Json> run(Json ir, std::string confirm = "false") {
    auto parsed = WorkflowIR::parse(ir);
    if (!parsed) { std::cerr << "[gov-parse] " << parsed.error() << "\n"; return std::nullopt; }
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto const v = validate(*parsed, vopts);
    if (!v.ok()) { std::cerr << "[gov-validate] " << v.describe() << "\n"; return std::nullopt; }
    auto wf = compile_workflow(*parsed, registry, &cp, CompileOptions{.components = &components});
    if (!wf) { std::cerr << "[gov-compile] " << wf.error().message << "\n"; return std::nullopt; }
    FireOptions fo;
    fo.thread_id = "t1";
    auto r = fire_once(*wf, Json{{"goal", "x"},
                                 {"task", "gov_test"},
                                 {"workspace", ws.dir.string()},
                                 {"session", "g"},
                                 {"mch_criteria", "整批验收"}}, fo);
    if (!r) {
      std::cerr << "[gov-fire-err] " << r.error().message << "\n";
      return std::nullopt;
    }
    return *r;
  }
};

// 预建 .out/gov_artifact.md（s1 的 probe 判据满足）
void seed_artifact(Fixture& f) {
  std::filesystem::path p = f.ws.dir / ".out" / "gov_artifact.md";
  std::ofstream(p) << "这是一个 gov artifact\n";
}

}  // namespace

// 1) s2 无判据 → unverified（完成但未验证），cursor 推进；s1 probe → done
TEST(WfGoverned, unverified_step_marks_plan_and_advances) {
  Fixture f;
  seed_artifact(f);
  // 脚本：think(s1)→judge?  judge 是 verify(probe) 不调模型（确定性）；think(s2)→无 verify→judge 也不调模型
  // judge 无判据步不调 model（L1）；但 think(model) 每步一次。脚本需覆盖 think 的两轮 + final。
  f.model.replies.push_back(cppchain::message::ai_message("s1 完成"));
  f.model.replies.push_back(cppchain::message::ai_message("s2 完成"));
  f.model.replies.push_back(cppchain::message::ai_message(R"({"pass":true,"feedback":"整批通过"})"));
  f.model.replies.push_back(cppchain::message::ai_message("解说完毕"));
  auto st = f.run(governed_ir());
  ASSERT_TRUE(st) << "governed 应跑通";
  // s1 probe 命中 → done；s2 无判据 → unverified；cursor 到 2
  EXPECT_EQ((*st)["plan"]["steps"][0]["status"].get<std::string>(), "done");
  EXPECT_EQ((*st)["plan"]["steps"][1]["status"].get<std::string>(), "unverified");
  EXPECT_EQ((*st)["plan"]["cursor"].get<long>(), 2);
  // plan_log：s1 done + s2 unverified
  bool saw_unverified = false;
  for (auto const& e : (*st)["plan_log"])
    if (e.is_object() && e.value("step", "") == "s2" && e.value("verdict", "") == "unverified")
      saw_unverified = true;
  EXPECT_TRUE(saw_unverified) << (*st)["plan_log"].dump();
}

// 2) judge fail → jgate → _fail（probe 文件缺失 → fail 不推进 → expr 门分流）
TEST(WfGoverned, judge_fail_routes_to_fail_terminal) {
  Fixture f;  // 不 seed_artifact → s1 probe 失败
  f.model.replies.push_back(cppchain::message::ai_message("s1 我说完成了"));
  auto st = f.run(governed_ir());
  // 失败应在 fire 层报错（_fail 落地节点 throw）——但 expr 门 then _fail 会走 __fail__ 抛异常
  // 实际：fire_once 返回 error（_fail 落地抛 ToolException）
  EXPECT_FALSE(st) << "probe 失败 + jgate → _fail（fire 报错）";
}

// 3) on_max=1 不够跑 2 步 → _timeout 档（loop 空转 max 次未收口）
TEST(WfGoverned, on_max_timeout_records_terminal) {
  Fixture f;
  seed_artifact(f);
  f.model.replies.push_back(cppchain::message::ai_message("s1 完成"));
  f.model.replies.push_back(cppchain::message::ai_message("s2 完成"));
  f.model.replies.push_back(cppchain::message::ai_message(R"({"pass":true,"feedback":"通过"})"));
  f.model.replies.push_back(cppchain::message::ai_message("解说"));
  // max=1：loop 只够 1 轮，s2 未跑 → on_max → _timeout
  auto st = f.run(governed_ir(/*confirm=*/"false", /*max=*/1));
  ASSERT_TRUE(st) << "timeout 档是正常收尾（非异常）";
  EXPECT_EQ((*st)["plan"].value("terminal", ""), "timeout");
  bool saw_terminal = false;
  for (auto const& e : (*st)["plan_log"])
    if (e.is_object() && e.value("kind", "") == "terminal" &&
        e.value("verdict", "") == "timeout")
      saw_terminal = true;
  EXPECT_TRUE(saw_terminal) << (*st)["plan_log"].dump();
}

// 4) plan_review confirm=true → interrupt → resume abort → _cancelled 档
TEST(WfGoverned, plan_review_abort_lands_cancelled_terminal) {
  Fixture f;
  auto wf = [&]() -> std::optional<CompiledWorkflow> {
    auto parsed = WorkflowIR::parse(governed_ir(/*confirm=*/"true"));
    if (!parsed) return std::nullopt;
    ValidatorOptions vopts;
    vopts.registry = &f.registry;
    vopts.components = &f.components;
    auto const v = validate(*parsed, vopts);
    if (!v.ok()) return std::nullopt;
    auto cwf = compile_workflow(*parsed, f.registry, &f.cp,
                                CompileOptions{.components = &f.components});
    if (!cwf) return std::nullopt;
    return *cwf;
  }();
  ASSERT_TRUE(wf);
  FireOptions fo; fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json{{"goal", "x"}, {"task", "gov_test"},
                                {"workspace", f.ws.dir.string()}, {"session", "g"},
                                {"mch_criteria", "x"}}, fo);
  ASSERT_TRUE(r1);
  EXPECT_EQ(wf->graph.last_interrupt().payload["kind"], "plan_review_request");
  fo.resume = Json{{"action", "abort"}}; fo.input_messages.clear();
  auto r2 = fire_once(*wf, Json{{"goal", "x"}, {"task", "gov_test"},
                                {"workspace", f.ws.dir.string()}, {"session", "g"},
                                {"mch_criteria", "x"}}, fo);
  ASSERT_TRUE(r2) << r2.error().message;
  EXPECT_TRUE((*r2)["plan"].value("cancelled", false)) << (*r2)["plan"].dump();
  EXPECT_EQ((*r2)["plan"].value("terminal", ""), "cancelled") << (*r2)["plan"].dump();
  bool saw_cancel = false;
  for (auto const& e : (*r2)["plan_log"])
    if (e.is_object() && e.value("kind", "") == "cancel") saw_cancel = true;
  EXPECT_TRUE(saw_cancel) << (*r2)["plan_log"].dump();
}

// 5) happy path：s1 probe done + s2 unverified + review pass → final → _turn_end（无 terminal 档）
TEST(WfGoverned, happy_path_full_chain) {
  Fixture f;
  seed_artifact(f);
  f.model.replies.push_back(cppchain::message::ai_message("s1 完成"));
  f.model.replies.push_back(cppchain::message::ai_message("s2 完成"));
  f.model.replies.push_back(cppchain::message::ai_message(R"({"pass":true,"feedback":"整批通过"})"));
  f.model.replies.push_back(cppchain::message::ai_message("解说完毕"));
  auto st = f.run(governed_ir());
  ASSERT_TRUE(st) << "governed 全链应跑通";
  EXPECT_FALSE((*st)["plan"].contains("terminal")) << "完成档不写 terminal";
  EXPECT_EQ((*st)["plan"]["cursor"].get<long>(), 2);
  // 任务档案落盘
  std::filesystem::path const arch = f.ws.dir / ".wfr" / "tasks" / "gov_test.json";
  EXPECT_TRUE(std::filesystem::exists(arch)) << "task end 应写回档案";
}

// 6) review fail（全步 unverified + reviewer 判 fail）→ gate → _fail
TEST(WfGoverned, review_fail_routes_to_fail) {
  Fixture f;
  seed_artifact(f);
  f.model.replies.push_back(cppchain::message::ai_message("s1 完成"));
  f.model.replies.push_back(cppchain::message::ai_message("s2 完成"));
  f.model.replies.push_back(cppchain::message::ai_message(R"({"pass":false,"feedback":"s2 未验证，不予放行"})"));
  auto st = f.run(governed_ir());
  // review fail → gate → _fail → 落地节点抛异常 → fire 报错
  EXPECT_FALSE(st) << "review fail + gate → _fail（fire 报错）";
}
