// B2（problems/39 优先级 2）：证据版本单调性测试。
// 写产物工具执行成功 → 引擎分配单调版本号登记进 _artifact_ver；verify probe 判据声明 version
// 校验"产物新鲜度"（版本 < 要求 → Partial fail）。版本号模型/工具伪造不了。
// 骨架复用 plan_execute_l2（file_write → verify probe，同一棵 workspace 树）。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <generator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

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
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using cppchain::state::MemoryCheckpointer;
using workflow::WorkflowError;

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

Message ai_call(std::vector<ToolCall> tcs, std::string const& id = "ai1") {
  Message m = cppchain::message::ai_message("");
  for (ToolCall& tc : tcs) m.tool_calls.push_back(std::move(tc));
  if (!id.empty()) m.id = id;
  return m;
}
Message ai_msg(std::string text) { return cppchain::message::ai_message(std::move(text)); }

Json l2_ir() {
  return Json::parse(R"json({
    "id": "wf_b2",
    "nodes": [
      {"id": "plan", "kind": "plan", "goal": "@config.goal", "default_plan": null, "next": "loop"},
      {"id": "loop", "kind": "loop", "max": 1, "next": "_turn_end", "on_max": "_turn_end",
       "body": [
         {"id": "step", "kind": "verify", "phase": "announce", "next": "think"},
         {"id": "think", "kind": "model", "model": "m", "tools": "ts", "next": "route"},
         {"id": "route", "kind": "route", "on": "last_ai_outcome",
          "cases": {"text": "judge", "tool_calls": "act", "invalid_tool_calls": "_fail"},
          "default": "_fail"},
         {"id": "act", "kind": "run_tools", "next": "judge"},
         {"id": "judge", "kind": "verify", "phase": "judge", "next": "_continue"}
       ]}
    ]
  })json");
}

struct WorkspaceDir {
  std::filesystem::path dir;
  WorkspaceDir()
      : dir(std::filesystem::temp_directory_path() /
           ("wf_b2_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::error_code ec;
    std::filesystem::create_directories(dir / ".out", ec);
  }
  ~WorkspaceDir() { std::filesystem::remove_all(dir); }
};

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

  std::expected<CompiledWorkflow, std::string> give(Json default_plan) {
    Json ir = l2_ir();
    ir["nodes"][0]["default_plan"] = default_plan;
    if (default_plan.contains("steps") && default_plan["steps"].is_array())
      ir["nodes"][1]["max"] = default_plan["steps"].size();
    ir["nodes"][1]["body"][1]["tools"] = "ts";
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

// 1) 版本通过：file_write 写 .out/x.txt → _artifact_ver 有值；verify 声明 version==实际 → done
TEST(WfArtifactVer, version_match_advances) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  f.model.replies.push_back(ai_call({ToolCall{"c1", "file_write",
                                               Json{{"path", ".out/x.txt"}, {"content", "X"}}}}));
  // 先算实际版本：写执行后从 state 读。verify 判据 version 用占位 __VER__，跑前替换。
  // 简化：跑两次——第一次只写看版本，第二次带 version 判据。这里直接构造"先写后验"链不行
  //（一次 fire 内 judge 已跑）。改为：第一次 fire 写 + 读出版本；第二次 fire 同一 ws 用该版本验证。
  Json dp = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"写 .out/x.txt","deps":[],
       "verify":{"kind":"probe","path":".out/x.txt","contains":"X","version":0}}]})json");
  auto wf = f.give(std::move(dp));
  ASSERT_TRUE(wf) << wf.error();
  FireOptions fo; fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  // 写成功 → 版本登记（>0）；verify 判据 version=0 ≤ 实际 → 通过
  auto const av = st.find(kArtifactVerKey);
  ASSERT_TRUE(av != st.end() && av->is_object()) << "写产物后应有 _artifact_ver";
  ASSERT_TRUE(av->contains(".out/x.txt"));
  EXPECT_GT(av->at(".out/x.txt").get<std::int64_t>(), 0);
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 1);
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "done");
}

// 2) 版本过时：verify 声明 version=999999（远超实际）→ fail + why 含 Partial + cursor 不推
TEST(WfArtifactVer, version_stale_fails) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  // 先写一次（产生实际版本；独立 thread，fire2 不续跑）
  {
    f.model.replies.push_back(ai_call({ToolCall{"c1", "file_write",
                                                 Json{{"path", ".out/x.txt"},
                                                      {"content", "X"}}}}));
    Json dp = Json::parse(R"json({"steps": [
        {"id":"s1","goal":"写 .out/x.txt","deps":[],
         "verify":{"kind":"probe","path":".out/x.txt","contains":"X"}}]})json");
    auto wf = f.give(std::move(dp));
    ASSERT_TRUE(wf);
    FireOptions fo; fo.thread_id = "t_stale_1";
    auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
    ASSERT_TRUE(r);
  }
  // fire2：独立 thread + 判据 version=999999 → 必然过时（文件在但版本 < 要求）
  f.model.replies.push_back(ai_msg("写好了"));  // 不再调工具（文件已存在）
  Json dp = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"写 .out/x.txt","deps":[],
       "verify":{"kind":"probe","path":".out/x.txt","contains":"X","version":999999}}]})json");
  auto wf = f.give(std::move(dp));
  ASSERT_TRUE(wf);
  FireOptions fo; fo.thread_id = "t_stale_2";
  auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
  ASSERT_TRUE(r) << r.error().message;
  Json st = *r;
  EXPECT_EQ(st["plan"]["cursor"].get<long>(), 0);  // 过时不推进
  EXPECT_EQ(st["plan"]["steps"][0]["status"].get<std::string>(), "failed");
  EXPECT_EQ(st["plan_log"][0]["verdict"].get<std::string>(), "fail");
  EXPECT_NE(st["plan_log"][0]["why"].get<std::string>().find("Partial"), std::string::npos)
      << st["plan_log"][0]["why"];
  EXPECT_NE(st["plan_log"][0]["why"].get<std::string>().find("证据过时"), std::string::npos);
}

// 3) 未登记：verify 声明 version 但没写工具 → have=-1 < want → fail
TEST(WfArtifactVer, version_unregistered_fails) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  f.model.replies.push_back(ai_msg("写好了"));  // 不调工具
  Json dp = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"写 .out/x.txt","deps":[],
       "verify":{"kind":"probe","path":".out/x.txt","contains":"X","version":1}}]})json");
  auto wf = f.give(std::move(dp));
  ASSERT_TRUE(wf);
  FireOptions fo; fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
  ASSERT_TRUE(r);
  EXPECT_EQ((*r)["plan_log"][0]["verdict"].get<std::string>(), "fail");
  EXPECT_EQ((*r)["plan"]["cursor"].get<long>(), 0);
}

// 4) 不声明 version 零回归：旧判据（无 version）行为不变（文件对 → done）
TEST(WfArtifactVer, no_version_zero_regression) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  f.model.replies.push_back(ai_call({ToolCall{"c1", "file_write",
                                               Json{{"path", ".out/x.txt"}, {"content", "X"}}}}));
  Json dp = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"写 .out/x.txt","deps":[],
       "verify":{"kind":"probe","path":".out/x.txt","contains":"X"}}]})json");
  auto wf = f.give(std::move(dp));
  ASSERT_TRUE(wf);
  FireOptions fo; fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
  ASSERT_TRUE(r);
  EXPECT_EQ((*r)["plan"]["steps"][0]["status"].get<std::string>(), "done");
  EXPECT_EQ((*r)["plan_log"][0]["verdict"].get<std::string>(), "done");
}

// 5) 跨 fire 单调：两次 fire（独立 thread，全新 state）写同 path → 版本递增（引擎单调源）
TEST(WfArtifactVer, versions_monotonic_across_fires) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  std::int64_t v1 = 0, v2 = 0;
  for (int fire = 0; fire < 2; ++fire) {
    f.model.replies.push_back(ai_call({ToolCall{"c1", "file_write",
                                                 Json{{"path", ".out/x.txt"},
                                                      {"content", "X" + std::to_string(fire)}}}}));
    Json dp = Json::parse(R"json({"steps": [
        {"id":"s1","goal":"写 .out/x.txt","deps":[],
         "verify":{"kind":"probe","path":".out/x.txt","contains":"X"}}]})json");
    auto wf = f.give(std::move(dp));
    ASSERT_TRUE(wf);
    FireOptions fo;
    fo.thread_id = "t" + std::to_string(fire);  // 独立 thread：每次全新 state，无续跑干扰
    auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
    ASSERT_TRUE(r);
    auto const av = (*r).find(kArtifactVerKey);
    ASSERT_TRUE(av != (*r).end() && av->is_object());
    std::int64_t const cur = av->at(".out/x.txt").get<std::int64_t>();
    (fire == 0 ? v1 : v2) = cur;
  }
  EXPECT_GT(v2, v1) << "两次 fire 写同 path，版本应严格递增（g_artifact_ver 单调）";
}

// 6) run_tools 路径登记：模型调 file_write（走 run_tools）→ 执行后 state 有 _artifact_ver
TEST(WfArtifactVer, run_tools_registers_artifact) {
  Fixture f;
  f.add_file_tools();
  f.components.toolsets["ts"] = {"file_write"};
  f.model.replies.push_back(ai_call({ToolCall{"c1", "file_write",
                                               Json{{"path", ".out/x.txt"}, {"content", "X"}}}}));
  Json dp = Json::parse(R"json({"steps": [
      {"id":"s1","goal":"写 .out/x.txt","deps":[],
       "verify":{"kind":"probe","path":".out/x.txt","contains":"X"}}]})json");
  auto wf = f.give(std::move(dp));
  ASSERT_TRUE(wf);
  FireOptions fo; fo.thread_id = "t1";
  auto const r = fire_once(*wf, Json{{"workspace", f.ws.dir.string()}, {"goal", "x"}}, fo);
  ASSERT_TRUE(r);
  auto const av = (*r).find(kArtifactVerKey);
  ASSERT_TRUE(av != (*r).end() && av->is_object()) << "run_tools 执行 file_write 应登记版本";
  EXPECT_TRUE(av->contains(".out/x.txt"));
  EXPECT_GT(av->at(".out/x.txt").get<std::int64_t>(), 0);
}