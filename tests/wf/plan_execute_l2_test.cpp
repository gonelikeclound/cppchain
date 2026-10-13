// wfr-㊹ 批② 测试：L2 规则档 —— verify 用确定性判据（probe / rule，不调 LLM）在步骤间验收。
// 覆盖：probe 命中 → done 推进、探针不中/文件缺失 → fail 不推进、rule{file_nonempty}、rule{json_key}；
// A1 收尾：strategy.on_fail=rollback_deps 失败自动回退到依赖步重做。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <generator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "cppchain/graph/graph.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/tool/file_tools.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;
using cppchain::message::Message;
using cppchain::message::ToolCall;
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

Message ai_msg(std::string t) { return cppchain::message::ai_message(std::move(t)); }
Message ai_call(std::vector<message::ToolCall> calls) {
  Message m = cppchain::message::ai_message("");
  for (auto& c : calls) m.tool_calls.push_back(std::move(c));
  return m;
}

struct WorkspaceDir {
  std::filesystem::path dir;
  WorkspaceDir()
      : dir(std::filesystem::temp_directory_path() /
            ("wf_pev_l2_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~WorkspaceDir() { std::filesystem::remove_all(dir); }
};

Json l2_ir() {
  return Json::parse(R"json({
    "id": "wf_l2",
    "trigger": {"source": "time", "timing": {"kind": "schedule", "schedule": "0 8 * * *"}},
    "nodes": [
      {"id": "plan", "kind": "plan", "goal": "@config.goal",
       "default_plan": null, "next": "loop"},
      {"id": "loop", "kind": "loop", "max": 4, "next": "_turn_end", "on_max": "_turn_end",
       "body": [
         {"id": "step", "kind": "verify", "phase": "announce", "next": "think"},
         {"id": "think", "kind": "model", "model": "m", "tools": "", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"tool_calls": "act", "text": "judge", "invalid_tool_calls": "_fail"},
          "default": "_fail"},
         {"id": "act", "kind": "run_tools", "next": "judge"},
         {"id": "judge", "kind": "verify", "phase": "judge", "next": "_continue"}
       ]}
    ]
  })json");
}

FileToolsConfig ft_config(std::filesystem::path ws) {
  FileToolsConfig c;
  c.root_dir = std::move(ws);
  return c;
}

struct Fixture {
  WorkspaceDir ws;
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;

  Fixture() { components.models["m"] = &model; }

  void add_file_tools() {
    registry.add(make_file_write_tool(ft_config(ws.dir)));
    registry.add(make_file_read_tool(ft_config(ws.dir)));
  }

  std::expected<CompiledWorkflow, std::string> give(Json default_plan, bool with_file_tools,
                                                    long max_override = 0) {
    Json ir = l2_ir();
    ir["nodes"][0]["default_plan"] = default_plan;
    if (default_plan.contains("steps") && default_plan["steps"].is_array())
      ir["nodes"][1]["max"] = max_override > 0 ? max_override : default_plan["steps"].size();
    if (with_file_tools) ir["nodes"][1]["body"][1]["tools"] = "ts";
    auto parsed = WorkflowIR::parse(std::move(ir));
    if (!parsed) return std::unexpected(parsed.error());
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto const v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(v.describe());
    CompileOptions c;
    c.components = &components;
    auto wf = compile_workflow(*parsed, registry, &cp, c);
    if (!wf) return std::unexpected(wf.error().message);
    return *wf;
  }
};

}  // namespace

TEST(WfPlanL2, probe_pass_advances_and_file_exists) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  Json dp = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"写 .out/note.txt 内容为【任务进行中】","deps":[],
       "verify":{"kind":"probe","path":".out/note.txt","contains":"任务进行中"}}]})json");
  f.model.replies.push_back(ai_call({ToolCall{"c1", "file_write",
                                               Json{{"path", ".out/note.txt"},
                                                    {"content", "任务进行中"}}}}));
  auto wf = f.give(std::move(dp), true);
  ASSERT_TRUE(wf) << wf.error();
  FireOptions fo; fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  EXPECT_TRUE(std::filesystem::exists(f.ws.dir / ".out" / "note.txt"));
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 1);
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "done");
  EXPECT_EQ(st["plan_log"][0]["verdict"].get<std::string>(), "done");
}

TEST(WfPlanL2, probe_missing_fails_and_does_not_advance) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  Json dp = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"写 .out/missing.txt","deps":[],
       "verify":{"kind":"probe","path":".out/missing.txt"}}]})json");
  f.model.replies.push_back(ai_msg("写好了"));
  auto wf = f.give(std::move(dp), true);
  ASSERT_TRUE(wf) << wf.error();
  FireOptions fo; fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 0);
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "failed");
  EXPECT_EQ(st["plan_log"][0]["verdict"].get<std::string>(), "fail");
}

TEST(WfPlanL2, rule_file_nonempty_and_json_key) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  Json dp = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"写 out2.json","deps":[],
       "verify":{"kind":"rule","op":"file_nonempty","path":".out2.json"}},
      {"id":"s2","goal":"校验键","deps":["s1"],
       "verify":{"kind":"rule","op":"json_key","key":"ok","path":".out2.json"}}]})json");
  f.model.replies.push_back(ai_call({ToolCall{"c1", "file_write",
                                               Json{{"path", ".out2.json"},
                                                    {"content", "{\"ok\":true}"}}}}));
  f.model.replies.push_back(ai_msg("看到了 yes"));
  auto wf = f.give(std::move(dp), true);
  ASSERT_TRUE(wf) << wf.error();
  FireOptions fo; fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 2);
  EXPECT_EQ(st["plan_log"][1]["verdict"].get<std::string>(), "done");
}

TEST(WfPlanL2, on_fail_rollback_deps_auto_rolls_back_to_dependency_once) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  Json dp = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"写 .out/s1.txt","deps":[],"verify":{"kind":"rule","op":"file_nonempty","path":".out/s1.txt"}},
      {"id":"s2","goal":"写 .out/s2.txt","deps":["s1"],"verify":{"kind":"rule","op":"file_nonempty","path":".out/s2.txt"}}]})json");
  f.model.replies.push_back(ai_call({ToolCall{"c1", "file_write", Json{{"path", ".out/s1.txt"}, {"content", "1"}}}}));
  f.model.replies.push_back(ai_msg("这一步先不写"));
  f.model.replies.push_back(ai_call({ToolCall{"c2", "file_write", Json{{"path", ".out/s1.txt"}, {"content", "1"}}}}));
  f.model.replies.push_back(ai_call({ToolCall{"c3", "file_write", Json{{"path", ".out/s2.txt"}, {"content", "2"}}}}));
  auto wf = f.give(std::move(dp), true, 6);  // max 留余量给"回退→重做"
  ASSERT_TRUE(wf) << wf.error();
  FireOptions fo; fo.thread_id = "t1";
  Json cfg{{"workspace", f.ws.dir.string()}, {"goal", "x"},
           {"plan_strategy", R"({"on_fail":"rollback_deps","max_auto_rollback":3})"}};
  auto const r = fire_once(*wf, std::move(cfg), fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 2) << st["plan"].dump();
  EXPECT_EQ(st["plan"]["auto_rollbacks"].get<long>(), 1) << st["plan"].dump();
  bool found = false;
  for (auto const& e : st["plan_log"])
    if (e.value("action", "") == "auto_rollback_deps") found = true;
  EXPECT_TRUE(found) << st["plan_log"].dump();
  EXPECT_TRUE(std::filesystem::exists(f.ws.dir / ".out" / "s2.txt"));
}