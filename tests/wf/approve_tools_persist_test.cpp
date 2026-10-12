// wfr-㊸ @④ 测试：会话级 allow 前缀自动放行 + 范围不变量 + allowlist 落盘辅助。
//
// 节点语义：approve_tools 读 state.config.approved_prefixes（宿主从 <session>.allowlist.jsonl
// 灌入）+ sandbox_provider（当前沙箱身份）。NeedHuman 命令若「前缀命中 且 沙箱身份一致」→
// 直接进 approved（不悬挂）；否则照旧悬挂人审。
// 范围不变量（problems/22）：不跨会话（文件作用域）、前缀匹配（允许 P 即允许 P 开头任意命令）、
// 沙箱降级后旧前缀失效（provider 不一致不自动放行）。三者在这各有一条反例。
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
#include "cppchain/tool/exec_guard.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"
#include "cppchain/workflow/wfr.hpp"  // allowlist 辅助（wfr_load_allow_prefixes / append）
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;
using cppchain::graph::InterruptStatus;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using cppchain::state::MemoryCheckpointer;

namespace {

using Json = nlohmann::json;
using Res = std::expected<Json, ToolError>;

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

Message ai_tool_calls(std::vector<ToolCall> tcs, std::string const& id = "ai1") {
  Message m = cppchain::message::ai_message("");
  for (ToolCall& tc : tcs) m.tool_calls.push_back(std::move(tc));
  if (!id.empty()) m.id = id;
  return m;
}

// NeedHuman 命令工具（形如 bash：args.command）。执行 = 副作用计数器。
struct CommandTool {
  int calls = 0;
  ToolSpec spec;
  CommandTool() {
    spec = make_tool("command_op", "需人审命令",
                     SchemaBuilder().prop("command", "string", "命令").build(),
                     [this](Json const& args, ToolContext&) -> Res {
                       ++calls;
                       auto c = arg<std::string>(args, "command");
                       return Json{{"ok", true}, {"calls", calls}, {"cmd", c ? *c : "?"}};
                     },
                     /*read_only=*/false);
    spec.approval = ToolApprovalLevel::NeedHuman;
    spec.category = "action";
  }
};

ToolCall cmd_call(std::string id, std::string command) {
  return ToolCall{id, "command_op", Json{{"command", command}}};
}

// 单轮 workflow：think(model) → route → approve → act(run_tools)
Json persist_ir() {
  return Json{{"id", "wf_persist"},
              {"trigger",
               Json{{"source", "time"}, {"timing", Json{{"kind", "schedule"}, {"schedule", "0 8 * * *"}}}}},
              {"nodes", Json::array(
                            {Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"tools", "ts"},
                                  {"next", "route"}},
                             Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
                                  {"cases", Json{{"tool_calls", "approve"},
                                                 {"invalid_tool_calls", "_fail"}, {"text", "_turn_end"}}},
                                  {"default", "_fail"}},
                             Json{{"id", "approve"}, {"kind", "approve_tools"}, {"deny", Json::array()},
                                  {"next", "act"}},
                             Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "echo"}},
                             Json{{"id", "echo"}, {"kind", "echo"}, {"text", "done"}, {"next", "_turn_end"}}})}};
}

struct Fixture {
  CommandTool tool;
  ToolRegistry registry;
  ScriptedModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;
  std::unique_ptr<ExecutionGuard> guard;

  Fixture() {
    registry.add(tool.spec);
    components.models["m"] = &model;
    components.toolsets["ts"] = {"command_op"};
    ExecutionGuard::Options go;
    go.log = [](std::string const&) { return ExecutionGuard::LogState{}; };  // 纯审计，不防重放
    guard = std::make_unique<ExecutionGuard>(std::move(go));
  }

  std::expected<CompiledWorkflow, std::string> give() {
    auto parsed = WorkflowIR::parse(persist_ir());
    if (!parsed) return std::unexpected(parsed.error());
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto const v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(v.describe());
    auto wf = compile_workflow(*parsed, registry, &cp, CompileOptions{.components = &components,
                                                                      .tool_policy = guard.get()});
    if (!wf) return std::unexpected(wf.error().message);
    return *wf;
  }

  // 跑一轮，返回是否悬挂（approve_tools 是否 interrupt）与最终状态
  struct Run {
    bool interrupted = false;
    Json state = Json::object();
  };
  Run fire(CompiledWorkflow& wf, Json config) {
    FireOptions fo;
    fo.thread_id = "t1";
    auto const r = fire_once(wf, std::move(config), fo);
    EXPECT_TRUE(r) << r.error().message;
    Run out;
    if (r) {
      out.state = *r;
      out.interrupted = wf.graph.last_interrupt().interrupted;
    }
    return out;
  }
};

Json approve_record(std::string prefix, std::string provider) {
  return Json{{"prefix", prefix}, {"provider", provider}};
}

}  // namespace

// ---- 命中：同前缀 + 同沙箱 → 自动放行，不悬挂 ----

TEST(WfApprovePersist, matching_session_prefix_auto_allows_without_suspend) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({cmd_call("c1", "echo ok; date")}));
  auto wf = f.give();
  ASSERT_TRUE(wf) << wf.error();

  Json config = Json{{"approved_prefixes", Json::array({approve_record("echo ok", "subprocess")})},
                     {"sandbox_provider", "subprocess"}};
  auto const run = f.fire(*wf, std::move(config));
  EXPECT_FALSE(run.interrupted) << "前缀命中 → 不悬挂，直接放行";
  EXPECT_EQ(f.tool.calls, 1) << "命令真被执行了一次（副作用计数器证明路径被走到）";
}

// ---- 不命中：就算是同一会话，命令不以前缀开头 → 照旧悬挂 ----

TEST(WfApprovePersist, non_matching_command_still_suspends) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({cmd_call("c1", "rm -rf /tmp/x")}));
  auto wf = f.give();
  ASSERT_TRUE(wf) << wf.error();

  Json config = Json{{"approved_prefixes", Json::array({approve_record("echo ok", "subprocess")})},
                     {"sandbox_provider", "subprocess"}};
  auto const run = f.fire(*wf, std::move(config));
  EXPECT_TRUE(run.interrupted) << "命令不以前缀开头 → 仍要人审";
  EXPECT_EQ(f.tool.calls, 0) << "未执行";
}

// ---- 范围不变量·沙箱：provider 不一致 → 前缀失效（沙箱降级后旧批准不自动放行） ----

TEST(WfApprovePersist, provider_mismatch_invalidates_the_session_prefix) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({cmd_call("c1", "echo ok; date")}));
  auto wf = f.give();
  ASSERT_TRUE(wf) << wf.error();

  // 记录是在 bwrap 沙箱下批准的；当前是受限子进程（沙箱降级）
  Json config = Json{{"approved_prefixes", Json::array({approve_record("echo ok", "bwrap")})},
                     {"sandbox_provider", "subprocess"}};
  auto const run = f.fire(*wf, std::move(config));
  EXPECT_TRUE(run.interrupted) << "沙箱身份不一致 → 不得自动放行（范围不变量）";
  EXPECT_EQ(f.tool.calls, 0);
}

// ---- 缺省：不开 allow_prefixes → 无配置 → 行为与批② 完全一致（全部悬挂） ----

TEST(WfApprovePersist, absent_config_behaves_like_before) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({cmd_call("c1", "echo ok; date")}));
  auto wf = f.give();
  ASSERT_TRUE(wf) << wf.error();

  auto const run = f.fire(*wf, Json::object());  // 无 approved_prefixes / sandbox_provider
  EXPECT_TRUE(run.interrupted);
  EXPECT_EQ(f.tool.calls, 0);
}

// ---- allowlist 落盘辅助：append → load 往返，坏行 fail-open ----

TEST(WfApprovePersist, allowlist_helpers_roundtrip_and_skip_corrupt_lines) {
  std::filesystem::path const workdir =
      std::filesystem::temp_directory_path() /
      ("wf_allowlist_" + std::to_string(static_cast<unsigned>(std::random_device{}())));

  // 空 → 空
  EXPECT_TRUE(wfr_load_allow_prefixes(workdir.string(), "s").empty());

  // append 两条（含跨 provider）
  wfr_append_allow_prefix(workdir.string(), "s", "echo ok", "subprocess");
  wfr_append_allow_prefix(workdir.string(), "s", "ls -la", "bwrap");
  // 人为塞一条坏行（fail-open：跳过不崩溃）
  {
    std::ofstream out(wfr_allowlist_path(workdir.string(), "s"), std::ios::app);
    out << "{corrupt json\n";
  }
  auto const records = wfr_load_allow_prefixes(workdir.string(), "s");
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].at("prefix").get<std::string>(), "echo ok");
  EXPECT_EQ(records[0].at("provider").get<std::string>(), "subprocess");
  EXPECT_EQ(records[1].at("prefix").get<std::string>(), "ls -la");
  EXPECT_EQ(records[1].at("provider").get<std::string>(), "bwrap");

  std::error_code ec;
  std::filesystem::remove_all(workdir, ec);
}