// wfr-㊿ 测试：task 节点 begin/end —— 文档档案建档、二批还原（cursor 跨批累积、批号+摘要注入）、
// revision-fork（回到历史批快照续跑）、批间复查（recheck_prev 插入 audit_prev + L3 裁判）。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <generator>
#include <iterator>
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

Json lot_ir(long batch_steps) {
  std::string const s = R"json({
    "id": "wf_lot",
    "trigger": {"source": "time", "timing": {"kind": "schedule", "schedule": "0 8 * * *"}},
    "nodes": [
      {"id": "begin", "kind": "task", "task": "@config.task", "phase": "begin", "next": "loop"},
      {"id": "loop", "kind": "loop", "max": __MAX__, "next": "final", "on_max": "final",
       "body": [
         {"id": "step", "kind": "verify", "phase": "announce", "next": "think"},
         {"id": "think", "kind": "model", "model": "m", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"text": "judge", "invalid_tool_calls": "_fail"}, "default": "_fail"},
         {"id": "judge", "kind": "verify", "phase": "judge", "model": "m", "next": "_continue"}
       ]},
      {"id": "final", "kind": "model", "model": "m", "next": "end"},
      {"id": "end", "kind": "task", "task": "@config.task", "phase": "end", "next": "_turn_end"}
    ]
  })json";
  std::string out = s;
  auto const p = out.find("__MAX__");
  if (p != std::string::npos) out.replace(p, 7, std::to_string(batch_steps));
  return Json::parse(out);
}

struct WorkspaceDir {
  std::filesystem::path dir;
  WorkspaceDir()
      : dir(std::filesystem::temp_directory_path() /
            ("wf_lot_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~WorkspaceDir() { std::filesystem::remove_all(dir); }
};

// 每次 fire 用全新 checkpointer + 独立线程：跨批契约 = 档案文件本身（屏蔽跨 fire 回放干扰）
int g_seq = 0;
std::optional<Json> fire_fresh(WorkflowIR const& ir, tool::ToolRegistry& reg,
                               ComponentRegistry& comps, Json cfg) {
  MemoryCheckpointer cp;
  auto wf = compile_workflow(ir, reg, &cp, CompileOptions{.components = &comps});
  if (!wf) { std::cerr << "compile err: " << wf.error().message << "\n"; return std::nullopt; }
  FireOptions fo;
  fo.thread_id = "f" + std::to_string(++g_seq);
  auto r = fire_once(*wf, std::move(cfg), fo);
  if (!r) { std::cerr << "fire err: " << r.error().message << "\n"; return std::nullopt; }
  return *r;
}

}  // namespace

TEST(WfLot, two_batches_restore_cursor_and_inject_batch_no) {
  WorkspaceDir ws;
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  components.models["m"] = &model;
  auto parsed = WorkflowIR::parse(lot_ir(2));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts; vopts.registry = &registry; vopts.components = &components;
  auto const v = validate(*parsed, vopts);
  ASSERT_TRUE(v.ok());
  Json const steps4 = Json::parse(R"json([
    {"id":"s1","goal":"第一批步","deps":[]},
    {"id":"s2","goal":"第一批步","deps":["s1"]},
    {"id":"s3","goal":"第二批步","deps":["s2"]},
    {"id":"s4","goal":"第二批步","deps":["s3"]}])json");
  for (int i = 0; i < 6; ++i)
    model.replies.push_back(cppchain::message::ai_message(i < 3 ? "批1" : "批2"));
  auto r1 = fire_fresh(*parsed, registry, components,
                       Json{{"workspace", ws.dir.string()}, {"task", "t1"}, {"goal", "x"},
                            {"plan_steps", steps4.dump()}});
  ASSERT_TRUE(r1.has_value());
  auto const r2 = fire_fresh(*parsed, registry, components,
                             Json{{"workspace", ws.dir.string()}, {"task", "t1"}});
  ASSERT_TRUE(r2.has_value());
  Json st = *r2;
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 4);
  EXPECT_EQ(st["plan"]["batch_no"].get<long>(), 2);
  std::ifstream in(ws.dir / ".wfr" / "tasks" / "t1.json");
  std::string const txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Json const arch = Json::parse(txt, nullptr, false);
  EXPECT_EQ(arch["cursor"].get<long>(), 4);
  EXPECT_EQ(arch["batch_no"].get<long>(), 2);
}

TEST(WfLot, revision_forks_back_to_an_earlier_batch_snapshot) {
  WorkspaceDir ws;
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  components.models["m"] = &model;
  auto parsed = WorkflowIR::parse(lot_ir(2));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts; vopts.registry = &registry; vopts.components = &components;
  auto const v = validate(*parsed, vopts);
  ASSERT_TRUE(v.ok());
  Json const steps4 = Json::parse(R"json([
    {"id":"s1","goal":"步1","deps":[]},
    {"id":"s2","goal":"步2","deps":["s1"]},
    {"id":"s3","goal":"步3","deps":["s2"]},
    {"id":"s4","goal":"步4","deps":["s3"]}])json");
  for (int i = 0; i < 12; ++i)
    model.replies.push_back(cppchain::message::ai_message("做完了"));
  fire_fresh(*parsed, registry, components,
             Json{{"workspace", ws.dir.string()}, {"task", "fx"}, {"goal", "x"},
                  {"plan_steps", steps4.dump()}});
  fire_fresh(*parsed, registry, components, Json{{"workspace", ws.dir.string()}, {"task", "fx"}});
  // 历史快照 batch1（cursor=2）存在；revision=1 从它续 → 末态 cursor=4
  std::ifstream in(ws.dir / ".wfr" / "tasks" / "fx.history" / "batch1.json");
  std::string const txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_FALSE(txt.empty()) << "history/batch1.json 应存在";
  Json const snap = Json::parse(txt, nullptr, false);
  EXPECT_EQ(snap["cursor"].get<long>(), 2);
  EXPECT_EQ(snap["batch_no"].get<long>(), 1);
  auto const r3 = fire_fresh(*parsed, registry, components,
                             Json{{"workspace", ws.dir.string()}, {"task", "fx"},
                                  {"task_revision", "1"}});
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3->value("plan", Json()).value("cursor", 0L), 4);
}

TEST(WfLot, recheck_prev_inserts_audit_step_with_model_judge) {
  WorkspaceDir ws;
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  components.models["m"] = &model;
  auto parsed = WorkflowIR::parse(lot_ir(2));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts; vopts.registry = &registry; vopts.components = &components;
  auto const v = validate(*parsed, vopts);
  ASSERT_TRUE(v.ok());
  Json const steps2 = Json::parse(R"json([
    {"id":"s1","goal":"第一步","deps":[]},
    {"id":"s2","goal":"第二步","deps":["s1"]}])json");
  auto ai = [](std::string t) { return cppchain::message::ai_message(std::move(t)); };
  model.replies.push_back(ai("步1")); model.replies.push_back(ai("步2")); model.replies.push_back(ai("批1汇总"));
  fire_fresh(*parsed, registry, components,
             Json{{"workspace", ws.dir.string()}, {"task", "rp"}, {"goal", "x"},
                  {"plan_steps", steps2.dump()}});
  model.replies.push_back(ai("正在复查上批摘要与本批目标"));
  model.replies.push_back(ai(R"({"pass":true,"feedback":"上批结论与目标一致"})"));
  model.replies.push_back(ai("批2汇总"));
  auto const r2 = fire_fresh(*parsed, registry, components,
                             Json{{"workspace", ws.dir.string()}, {"task", "rp"},
                                  {"recheck_prev", "on"}});
  ASSERT_TRUE(r2.has_value());
  Json st = *r2;
  bool has_audit = false, audited = false;
  for (auto const& s : st["plan"]["steps"]) if (s.value("id", "") == "audit_prev") has_audit = true;
  for (auto const& e : st["plan_log"])
    if (e.value("step", "") == "audit_prev" && e.value("kind", "") == "model") audited = true;
  EXPECT_TRUE(has_audit) << st["plan"].dump();
  EXPECT_TRUE(audited) << st["plan_log"].dump();
  EXPECT_EQ(st["plan"]["batch_no"].get<long>(), 2);
}