// A6（problems/39 优先级 1）：verify 节点 kind=agent 判据测试。
// 独立验收 subagent（只读工具真检查）三态 done/fail/unverified + 判不了推进 + 验收者只读约束。
// 骨架复用 plan_execute_l3（ScriptedModel 按序吐 step 回复 + auditor 终答；RecordingModel 记录 tools 断言只读）。
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/tool/tool.hpp"
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
using cppchain::state::MemoryCheckpointer;
using workflow::WorkflowError;

namespace {

using Json = nlohmann::json;

// 记录模型：拷贝最后一次 CallOptions（断言"验收者只见只读工具"用），并按预置队列吐回复。
class RecordingModel final : public model::ChatModel {
 public:
  model::CallOptions last_opts;
  std::size_t invoked = 0;
  std::deque<Message> replies;

  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const& opts) override {
    last_opts = opts;
    ++invoked;
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "replies exhausted", {}});
    Message m = replies.front();
    replies.pop_front();
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

// L3 环：1 步带 verify kind=agent；judge 节点复用同一模型 m + 可选 toolset（验收者只读面）。
Json agent_ir(std::string const& judge_model, std::string const& verify_ro_toolset) {
  Json judge = Json{{"id", "judge"}, {"kind", "verify"}, {"phase", "judge"},
                    {"model", judge_model}, {"next", "_continue"}};
  if (!verify_ro_toolset.empty()) judge["toolset"] = verify_ro_toolset;
  Json body = Json::array();
  body.push_back(Json{{"id", "step"}, {"kind", "verify"}, {"phase", "announce"}, {"next", "think"}});
  body.push_back(Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"next", "route"}});
  body.push_back(Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
                     {"cases", Json{{"text", "judge"}, {"tool_calls", "act"},
                                    {"invalid_tool_calls", "_fail"}}},
                     {"default", "_fail"}});
  body.push_back(Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "judge"}});
  body.push_back(std::move(judge));
  Json nodes = Json::array();
  nodes.push_back(Json{{"id", "plan"}, {"kind", "plan"}, {"goal", "@config.goal"},
                       {"default_plan", nullptr}, {"next", "loop"}});
  nodes.push_back(Json{{"id", "loop"}, {"kind", "loop"}, {"max", 1}, {"next", "_turn_end"},
                       {"on_max", "_turn_end"}, {"body", std::move(body)}});
  return Json{{"id", "wf_a6"}, {"nodes", std::move(nodes)}};
}

struct Fixture {
  FakeToolSinks sinks;
  ToolRegistry registry;
  RecordingModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;
  Fixture() {
    registry = make_fake_tools(sinks);        // 含 read（只读）、file_write 等
    components.models["m"] = &model;
    components.toolsets["verify_ro"] = {std::string(kToolRead)};  // A6：验收者只读面
  }

  std::expected<Json, WorkflowError> run(Json ir) {
    auto parsed = WorkflowIR::parse(ir);
    if (!parsed) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto const v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, v.describe()});
    auto wf = compile_workflow(*parsed, registry, &cp,
                               CompileOptions{.components = &components});
    if (!wf) return std::unexpected(wf.error());
    FireOptions fo;
    fo.thread_id = "t1";
    return fire_once(*wf, Json{{"goal", "写一个含 X 的文件"}}, fo);
  }
};

// 从 OpenAI tools 数组取 function.name 列表
std::vector<std::string> tool_names(model::CallOptions const& o) {
  std::vector<std::string> out;
  for (Json const& t : o.tools) out.push_back(t["function"]["name"]);
  return out;
}

}  // namespace

// 1) agent 判据 done：auditor 回 {"verdict":"done"} → 推进、status=done、evidence 进 why
TEST(WfVerifyAgent, auditor_done_advances) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("已写 .out/x.txt（含 X）"));
  f.model.replies.push_back(cppchain::message::ai_message(
      R"({"verdict":"done","evidence":"read 到 .out/x.txt 且含 X"})"));
  Json ir = agent_ir("m", "verify_ro");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps":[
      {"id":"s1","goal":"写 .out/x.txt 且含 X","deps":[],
       "verify":{"kind":"agent","criteria":".out/x.txt 存在且含 X","paths":[".out/x.txt"]}}]})json");
  auto st = f.run(std::move(ir));
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ((*st)["plan"]["cursor"].get<long>(), 1);
  EXPECT_EQ((*st)["plan"]["steps"][0]["status"].get<std::string>(), "done");
  EXPECT_EQ((*st)["plan_log"][0]["kind"].get<std::string>(), "agent");
  EXPECT_EQ((*st)["plan_log"][0]["verdict"].get<std::string>(), "done");
  EXPECT_NE((*st)["plan_log"][0]["why"].get<std::string>().find("含 X"),
            std::string::npos);
}

// 2) agent 判据 fail：auditor 回 {"verdict":"fail"} → 不推进、status=failed
TEST(WfVerifyAgent, auditor_fail_blocks) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("做了点事"));
  f.model.replies.push_back(cppchain::message::ai_message(
      R"({"verdict":"fail","evidence":".out/x.txt 不存在"})"));
  Json ir = agent_ir("m", "verify_ro");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps":[
      {"id":"s1","goal":"写 .out/x.txt","deps":[],
       "verify":{"kind":"agent","criteria":".out/x.txt 存在","paths":[".out/x.txt"]}}]})json");
  auto st = f.run(std::move(ir));
  ASSERT_TRUE(st);
  EXPECT_EQ((*st)["plan"]["cursor"].get<long>(), 0);
  EXPECT_EQ((*st)["plan"]["steps"][0]["status"].get<std::string>(), "failed");
  EXPECT_EQ((*st)["plan_log"][0]["verdict"].get<std::string>(), "fail");
}

// 3) agent 判据 unverified：auditor 回非 JSON（解析失败）→ 推进 + status=unverified（诚实标注）
TEST(WfVerifyAgent, auditor_unverifiable_advances_as_unverified) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("做了点事"));
  f.model.replies.push_back(cppchain::message::ai_message("我不确定文件是否符合"));
  Json ir = agent_ir("m", "verify_ro");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps":[
      {"id":"s1","goal":"写文件","deps":[],
       "verify":{"kind":"agent","criteria":"文件存在","paths":[".out/x.txt"]}}]})json");
  auto st = f.run(std::move(ir));
  ASSERT_TRUE(st);
  EXPECT_EQ((*st)["plan"]["cursor"].get<long>(), 1);  // 推进不卡死
  EXPECT_EQ((*st)["plan"]["steps"][0]["status"].get<std::string>(), "unverified");
  EXPECT_EQ((*st)["plan_log"][0]["verdict"].get<std::string>(), "unverified");
}

// 4) 验收者只见只读工具：model 收到的 tools 只有 read（无 file_write/bash 等写工具）
TEST(WfVerifyAgent, auditor_tools_are_read_only) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("做了点事"));
  f.model.replies.push_back(cppchain::message::ai_message(
      R"({"verdict":"done","evidence":"read 到了"})"));
  Json ir = agent_ir("m", "verify_ro");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps":[
      {"id":"s1","goal":"写文件","deps":[],
       "verify":{"kind":"agent","criteria":"文件存在","paths":[".out/x.txt"]}}]})json");
  auto st = f.run(std::move(ir));
  ASSERT_TRUE(st);
  // 记录最后一次调用（auditor 的 Agent 内部 invoke）→ tools 应为只读集
  auto const names = tool_names(f.model.last_opts);
  ASSERT_EQ(names.size(), 1u) << "验收者只应拿到只读工具集";
  EXPECT_EQ(names[0], std::string(kToolRead));
  for (std::string const& n : names)
    EXPECT_NE(n, "file_write") << "验收者不应拿到写工具";
}

// 5) 无 toolset → 验收者空工具集（只能看 workspace 字符串，判不了 → unverified 推进）
TEST(WfVerifyAgent, auditor_without_toolset_gets_no_tools) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("做了点事"));
  f.model.replies.push_back(cppchain::message::ai_message(
      R"({"verdict":"unverified","evidence":"没有工具无法检查"})"));
  Json ir = agent_ir("m", /*verify_ro_toolset=*/"");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps":[
      {"id":"s1","goal":"写文件","deps":[],
       "verify":{"kind":"agent","criteria":"文件存在","paths":[".out/x.txt"]}}]})json");
  auto st = f.run(std::move(ir));
  ASSERT_TRUE(st);
  EXPECT_TRUE(tool_names(f.model.last_opts).empty()) << "无 toolset → 验收者空工具";
  EXPECT_EQ((*st)["plan_log"][0]["verdict"].get<std::string>(), "unverified");
  EXPECT_EQ((*st)["plan"]["cursor"].get<long>(), 1);  // 推进
}

// 6) 模型错 fail-closed：auditor 队列耗尽（invoke 返回错误）→ verdict=fail、不推进
TEST(WfVerifyAgent, auditor_model_error_fail_closed) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("做了点事"));
  // 不预置 auditor 回复 → invoke 返回 "replies exhausted" → judge_agent_review fail
  Json ir = agent_ir("m", "verify_ro");
  ir["nodes"][0]["default_plan"] = Json::parse(R"json({"steps":[
      {"id":"s1","goal":"写文件","deps":[],
       "verify":{"kind":"agent","criteria":"文件存在","paths":[".out/x.txt"]}}]})json");
  auto st = f.run(std::move(ir));
  ASSERT_TRUE(st);
  EXPECT_EQ((*st)["plan_log"][0]["verdict"].get<std::string>(), "fail");
  EXPECT_EQ((*st)["plan"]["cursor"].get<long>(), 0);
  EXPECT_EQ((*st)["plan"]["steps"][0]["status"].get<std::string>(), "failed");
}