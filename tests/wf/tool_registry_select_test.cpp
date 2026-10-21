// wfr-㊹ 治理（problems/34）· tool_registry 节点「内置选择」测试。
// 新增 select 源（{from:"builtin",type:"select",load:[内置名]}）：仅激活内置工具、不拷定义，
// 活动工具集发布进 results.active_tools；model/run_tools 从 context 取（共享优先、内置兜底）。
// 覆盖：schema==活动集 / 活动集优先 toolset / 无 registry 零回归 / so+select 合并（mock 顶替）/
// refuses 允许名单（非活动名 → error ToolMessage）拒绝发生在 guard.prepare 之前 / VN23 校验 / 往返。
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/tool/exec_guard.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/tool/tool_plugin.hpp"
#include "cppchain/state/checkpointer.hpp"
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
using cppchain::message::ToolCall;
using workflow::WorkflowError;

namespace {

using Json = nlohmann::json;

// 记录模型：拷贝最后一次 CallOptions（含 tools schema），并按预置队列吐回复。
class RecordingModel final : public model::ChatModel {
 public:
  model::CallOptions last_opts;   // 最后收到的调用选项（tools 数组可断言）
  std::size_t invoked = 0;
  std::deque<Message> replies;    // 预置回复（空则抛错）

  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const& opts) override {
    last_opts = opts;
    ++invoked;
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "scripted replies exhausted", {}});
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

Message ai_tool_calls(std::vector<ToolCall> tcs, std::string const& id = "ai1") {
  Message m = cppchain::message::ai_message("");
  for (ToolCall& tc : tcs) m.tool_calls.push_back(std::move(tc));
  if (!id.empty()) m.id = id;
  return m;
}

// 从 OpenAI tools 数组里取第 i 个的 function.name
std::string tool_name_at(Json const& tools, std::size_t i) {
  return tools.at(i).at("function").at("name").get<std::string>();
}
// 从 OpenAI tools 数组里取第 i 个的 function.description
std::string tool_desc_at(Json const& tools, std::size_t i) {
  return tools.at(i).at("function").at("description").get<std::string>();
}

// 从 state 里的一条消息 JSON 提取纯文本：content 是 content-blocks 数组 [{text,...}]
std::string message_text(Json const& m) {
  if (!m.is_object() || !m.contains("content") || !m["content"].is_array()) return "";
  std::string out;
  for (auto const& blk : m["content"])
    if (blk.is_object() && blk.contains("text") && blk["text"].is_string())
      out += blk["text"].get<std::string>();
  return out;
}

// 默认编排：entry=tool_registry → think(model) → route(rethink?) 简化成 → act(run_tools) → end。
// 模型回 tool_calls → act 执行；回 text → 直接 end。
Json wf_with_tool_registry(Json tools_src, std::string const& model_tools = "") {
  Json nodes = Json::array();
  nodes.push_back(Json{{"id", "treg"}, {"kind", "tool_registry"}, {"tools", tools_src},
                       {"next", "think"}});
  Json think = Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"next", "route"}};
  if (!model_tools.empty()) think["tools"] = model_tools;
  nodes.push_back(std::move(think));
  nodes.push_back(Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
                       {"cases", Json{{"tool_calls", "act"}, {"invalid_tool_calls", "_fail"}}},
                       {"default", "_turn_end"}});
  nodes.push_back(Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_turn_end"}});
  return Json{{"id", "wf_treg"}, {"nodes", std::move(nodes)}};
}

// 无 tool_registry 的对照编排（零回归路径：model 走 toolset 组件）
Json wf_without_registry(std::string const& model_tools) {
  Json nodes = Json::array(
      {Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"tools", model_tools},
            {"next", "route"}},
       Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
            {"cases", Json{{"tool_calls", "act"}, {"invalid_tool_calls", "_fail"}}},
            {"default", "_turn_end"}},
       Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_turn_end"}}});
  return Json{{"id", "wf_notreg"}, {"nodes", std::move(nodes)}};
}

struct Fixture {
  FakeToolSinks sinks;
  tool::ToolRegistry registry = make_fake_tools(sinks);  // 内置表（select 的解析源）
  tool::ToolRegistry shared;                              // CompileOptions::shared_registry（so 加载目标）
  RecordingModel model;
  ComponentRegistry components;
  std::unique_ptr<ExecutionGuard> guard;                  // 可选：副作用防护
  std::vector<std::string> lib_dirs;                      // 可选：动态库搜索目录（tool_lib_dirs）

  Fixture() { components.models["m"] = &model; }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    o.tool_policy = guard.get();
    o.shared_registry = const_cast<tool::ToolRegistry*>(&shared);
    if (!lib_dirs.empty()) o.tool_lib_dirs = &lib_dirs;
    return o;
  }

  std::expected<Json, WorkflowError> run(Json ir) {
    auto parsed = WorkflowIR::parse(ir);
    if (!parsed) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    auto wf = compile_workflow(*parsed, registry, nullptr, opts());
    if (!wf) return std::unexpected(wf.error());
    return fire_once(*wf, Json::object());
  }
};

}  // namespace

// 1) model 收到的 schema 恰好 == 活动集（select 3 内置，无 toolset 组件）
TEST(WfToolRegistrySelect, model_receives_schema_equals_active_set) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));  // text → route → _turn_end
  auto st = f.run(wf_with_tool_registry(
      Json::array({Json{{"from", "builtin"}, {"type", "select"},
                        {"load", Json::array({std::string(kToolSum),
                                              std::string(kToolEcho),
                                              std::string(kToolRead)})}}})));
  ASSERT_TRUE(st) << st.error().message;
  // results 发布活动集（保序：声明序）
  EXPECT_EQ((*st)["results"]["treg"]["active_tools"],
            Json::array({std::string(kToolSum), std::string(kToolEcho), std::string(kToolRead)}));
  // model 收到的 schema：恰 3 把、名字依次（注意：没调过回 text → route → end，invoke 1 次）
  EXPECT_EQ(f.model.invoked, 1u);
  ASSERT_EQ(f.model.last_opts.tools.size(), 3u);
  EXPECT_EQ(tool_name_at(f.model.last_opts.tools, 0), std::string(kToolSum));
  EXPECT_EQ(tool_name_at(f.model.last_opts.tools, 1), std::string(kToolEcho));
  EXPECT_EQ(tool_name_at(f.model.last_opts.tools, 2), std::string(kToolRead));
}

// 2) 活动集优先于 toolset 组件：model 同时给了 toolset（sum）但 select 只选 echo → 只看 echo
TEST(WfToolRegistrySelect, active_set_overrides_toolset) {
  Fixture f;
  f.components.toolsets["ts"] = {std::string(kToolSum)};
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  auto st = f.run(wf_with_tool_registry(
      Json::array({Json{{"from", "builtin"}, {"type", "select"},
                        {"load", Json::array({std::string(kToolEcho)})}}}),
      /*model_tools=*/"ts"));
  ASSERT_TRUE(st) << st.error().message;
  ASSERT_EQ(f.model.last_opts.tools.size(), 1u);
  EXPECT_EQ(tool_name_at(f.model.last_opts.tools, 0), std::string(kToolEcho));
}

// 3) 零回归：无 tool_registry → 模型走 toolset 组件名单（sum+echo），results 无 active_tools
TEST(WfToolRegistrySelect, no_registry_falls_back_to_toolset) {
  Fixture f;
  f.components.toolsets["ts"] = {std::string(kToolSum), std::string(kToolEcho)};
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  auto st = f.run(wf_without_registry("ts"));
  ASSERT_TRUE(st) << st.error().message;
  ASSERT_EQ(f.model.last_opts.tools.size(), 2u);
  EXPECT_EQ(tool_name_at(f.model.last_opts.tools, 0), std::string(kToolSum));
  EXPECT_EQ(tool_name_at(f.model.last_opts.tools, 1), std::string(kToolEcho));
  // 活动路径未被误触发：无任何帧带 active_tools
  auto const res = st->find("results");
  bool saw_active = false;
  if (res != st->end() && res->is_object())
    for (auto const& [k, v] : res->items())
      if (v.is_object() && v.contains("active_tools")) saw_active = true;
  EXPECT_FALSE(saw_active);
}

// 4) 无 tool_registry 但共享 registry 注入了 → 仍走 toolset 路径（共享只是"动态工具库"，
//    不发布活动集，消费方不会误触发活动分支）
TEST(WfToolRegistrySelect, no_registry_but_shared_injected_still_toolset_path) {
  Fixture f;
  f.components.toolsets["ts"] = {std::string(kToolSum)};
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  auto st = f.run(wf_without_registry("ts"));
  ASSERT_TRUE(st) << st.error().message;
  ASSERT_EQ(f.model.last_opts.tools.size(), 1u);
  EXPECT_EQ(tool_name_at(f.model.last_opts.tools, 0), std::string(kToolSum));
}

// 5) run_tools 允许名单：活动集只选 sum；模型同时调 send_notification（非活动）+ sum（活动）
//    → 非活动名拒绝执行（error ToolMessage 回喂），副作用 sinks 为空；sum 正常执行。
TEST(WfToolRegistrySelect, run_tools_executes_only_active_and_denies_other) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls(
      {ToolCall{"call_send", std::string(kToolSendNotification),
                Json{{"title", "不该发"}}},
       ToolCall{"call_sum", std::string(kToolSum), Json{{"a", 2}, {"b", 3}}}}));
  auto st = f.run(wf_with_tool_registry(
      Json::array({Json{{"from", "builtin"}, {"type", "select"},
                        {"load", Json::array({std::string(kToolSum)})}}})));
  ASSERT_TRUE(st) << st.error().message;
  // 被拒调用未执行 → 收件箱空（副作用不落地）
  EXPECT_TRUE(f.sinks.notifications.empty()) << "非活动工具不应真实执行";
  // 消息序列里：一条 sum 的 success ToolMessage + 一条 send_notification 的 error ToolMessage
  std::vector<std::string> tool_statuses;  // {tool_call_id: status}
  std::vector<std::string> tool_texts;
  for (auto const& m : (*st)["messages"])
    if (m.is_object() && m.value("type", "") == "tool") {
      tool_statuses.push_back(m.value("status", ""));
      tool_texts.push_back(message_text(m));
    }
  ASSERT_EQ(tool_statuses.size(), 2u);
  // 顺序与调用序一致：send 先（被拒 error）、sum 后（success）
  EXPECT_EQ(tool_statuses[0], "error");
  EXPECT_EQ(tool_statuses[1], "success");
  // 拒绝文案点名工具名 + 说明理由
  EXPECT_NE(tool_texts[0].find("send_notification"), std::string::npos);
  EXPECT_NE(tool_texts[0].find("不在本 workflow 的活动工具名单"), std::string::npos);
}

// 6) 拒绝发生在 guard.prepare 之前：打开 guard，sum 正常经 prepare/run 执行一次；
//    被拒调用不进 guard（不产生 intent/executions——guard 无从见到它）。
TEST(WfToolRegistrySelect, denied_under_guard_path_skips_guard) {
  Fixture f;
  f.guard = std::make_unique<ExecutionGuard>(ExecutionGuard::Options{});
  // 活动工具用**非只读** send_notification：guard.prepare 对只读工具直接放行不审计，
  // 非只读才走 audit —— 这样才能证明"被拒调用连 guard 都见不到"。
  f.model.replies.push_back(ai_tool_calls(
      {ToolCall{"call_other", "mock_weather", Json{{"city", "SH"}}},
       ToolCall{"call_send", std::string(kToolSendNotification),
                Json{{"title", "活动工具发一条"}}}}));
  auto st = f.run(wf_with_tool_registry(
      Json::array({Json{{"from", "builtin"}, {"type", "select"},
                        {"load", Json::array({std::string(kToolSendNotification)})}}})));
  ASSERT_TRUE(st) << st.error().message;
  // guard 只见 send（活动）一次准备+执行；被拒的 mock_weather 从未进入 prepare
  //（guard.prepare 对非只读每调用一次审计一次 —— 只有 send 一行）
  EXPECT_EQ(f.guard->audit_lines(), 1u);
  EXPECT_EQ(f.guard->executions(), 1u);
  // 副作用只落在活动工具上
  EXPECT_EQ(f.sinks.notifications.size(), 1u);
  EXPECT_EQ(f.sinks.notifications[0], "活动工具发一条");
  // 消息两条：一条 error（mock_weather 被拒）+ 一条 success（send 真发）
  std::size_t errors = 0, successes = 0;
  for (auto const& m : (*st)["messages"])
    if (m.is_object() && m.value("type", "") == "tool")
      (m.value("status", "") == "error" ? errors : successes)++;
  EXPECT_EQ(errors, 1u);
  EXPECT_EQ(successes, 1u);
}

// 7) 校验：select 引用内置注册表不存在的工具 → VN23
TEST(WfToolRegistrySelect, vn23_rejects_select_unknown_builtin) {
  Fixture f;
  auto parsed = WorkflowIR::parse(wf_with_tool_registry(
      Json::array({Json{{"from", "builtin"}, {"type", "select"},
                        {"load", Json::array({"no_such_tool"})}}})));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  vopts.components = &f.components;
  auto const v = validate(*parsed, vopts);
  ASSERT_FALSE(v.ok());
  EXPECT_NE(v.describe().find("VN23"), std::string::npos);
  EXPECT_NE(v.describe().find("内置注册表里不存在"), std::string::npos);
}

// 8) 校验：select 源 from 必须为 builtin
TEST(WfToolRegistrySelect, vn23_rejects_select_from_not_builtin) {
  Fixture f;
  auto parsed = WorkflowIR::parse(wf_with_tool_registry(
      Json::array({Json{{"from", "other_lib"}, {"type", "select"},
                        {"load", Json::array({std::string(kToolSum)})}}})));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  auto const v = validate(*parsed, vopts);
  ASSERT_FALSE(v.ok());
  EXPECT_NE(v.describe().find("VN23"), std::string::npos);
  EXPECT_NE(v.describe().find("必须为 \"builtin\""), std::string::npos);
}

// 9) 校验：type 词表只认 so/select
TEST(WfToolRegistrySelect, vn23_rejects_unknown_type_vocabulary) {
  Fixture f;
  auto parsed = WorkflowIR::parse(wf_with_tool_registry(
      Json::array({Json{{"from", "x"}, {"type", "dlopen"},
                        {"load", Json::array({"xx"})}}})));
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.registry;
  auto const v = validate(*parsed, vopts);
  ASSERT_FALSE(v.ok());
  EXPECT_NE(v.describe().find("VN23"), std::string::npos);
  EXPECT_NE(v.describe().find("不在词表里"), std::string::npos);
}

// 10) so + select 合并：动态 mock 加载进共享表 + 内置激活；active_tools 两者并集；
//     model schema 两者都有；run 执行动态工具（mock_file_read，只读）。
TEST(WfToolRegistrySelect, so_and_select_merge_with_mock_via_dynamic_load) {
#ifdef CPPCHAIN_MOCK_TOOLS_DIR
  Fixture f;
  f.lib_dirs.push_back(CPPCHAIN_MOCK_TOOLS_DIR);  // 目录宏 → 按 "libmock_tools.so" 找库
  f.model.replies.push_back(ai_tool_calls(
      {ToolCall{"call_mread", "mock_file_read", Json{{"path", "a.txt"}}},
       ToolCall{"call_sum", std::string(kToolSum), Json{{"a", 2}, {"b", 3}}}}));
  auto st = f.run(wf_with_tool_registry(Json::array(
      {Json{{"from", "mock_tools"}, {"type", "so"},
            {"load", Json::array({"mock_file_read"})}},
       Json{{"from", "builtin"}, {"type", "select"},
            {"load", Json::array({std::string(kToolSum)})}}})));
  ASSERT_TRUE(st) << st.error().message;
  // 发布：loaded=仅 so 实际加载名；active_tools=两者并集（声明序）
  EXPECT_EQ((*st)["results"]["treg"]["loaded"], Json::array({"mock_file_read"}));
  EXPECT_EQ((*st)["results"]["treg"]["active_tools"],
            Json::array({"mock_file_read", std::string(kToolSum)}));
  // model schema 两把（mock 优先解析自共享表，description 带 "mock"）
  ASSERT_EQ(f.model.last_opts.tools.size(), 2u);
  EXPECT_EQ(tool_name_at(f.model.last_opts.tools, 0), "mock_file_read");
  EXPECT_TRUE(tool_desc_at(f.model.last_opts.tools, 0).find("mock") != std::string::npos);
  EXPECT_EQ(tool_name_at(f.model.last_opts.tools, 1), std::string(kToolSum));
  // run 执行：两条 ToolMessage —— sum 成功；mock_file_read 被**允许执行**（读不存在的
  // 文件 → error 是 handler 的错，不是"不在活动名单"的拒绝），文案应点名"文件不存在"。
  std::size_t successes = 0, errors = 0;
  bool read_err_saw = false;
  for (auto const& m : (*st)["messages"])
    if (m.is_object() && m.value("type", "") == "tool") {
      if (m.value("status", "") == "error") {
        ++errors;
        if (message_text(m).find("文件不存在") != std::string::npos) read_err_saw = true;
      } else {
        ++successes;
      }
    }
  EXPECT_EQ(successes, 1u);
  EXPECT_EQ(errors, 1u);
  EXPECT_TRUE(read_err_saw) << "mock_file_read 应被允许执行（error 来自读文件，而非活动名单拒绝）"
                            << (*st)["messages"].dump();
  // 无"不在活动名单"的拒绝
  for (auto const& m : (*st)["messages"])
    if (m.is_object() && m.value("type", "") == "tool")
      EXPECT_EQ(message_text(m).find("不在本 workflow 的活动工具名单"), std::string::npos);
#else
  GTEST_SKIP() << "CPPCHAIN_MOCK_TOOLS_DIR 未定义（mock_tools 本地库未构建）";
#endif
}

// 11) 共享表预加同名 sum（mock 顶替）→ 共享优先：model schema 描述命中 mock，执行也走 mock
TEST(WfToolRegistrySelect, select_shadowed_by_shared_mock_wins) {
  Fixture f;
  // 预置一个同名 mock sum（内置表也存在 sum）——共享优先应命中此定义
  f.shared.add(make_tool("sum", "MOCK SUM（共享表顶替）",
                         SchemaBuilder().prop("a", "number", "", true)
                             .prop("b", "number", "", true)
                             .build(),
                         [](Json const&, ToolContext&) -> std::expected<Json, ToolError> {
                           return Json{{"sum", 999}};
                         }));
  f.model.replies.push_back(ai_tool_calls(
      {ToolCall{"call_sum", std::string(kToolSum), Json{{"a", 2}, {"b", 3}}}}));
  auto st = f.run(wf_with_tool_registry(
      Json::array({Json{{"from", "builtin"}, {"type", "select"},
                        {"load", Json::array({std::string(kToolSum)})}}})));
  ASSERT_TRUE(st) << st.error().message;
  ASSERT_EQ(f.model.last_opts.tools.size(), 1u);
  // 共享优先：schema 描述是 mock 的（而非内置 sum 的）
  EXPECT_NE(tool_desc_at(f.model.last_opts.tools, 0).find("MOCK SUM"),
            std::string::npos);
  // 执行走共享 mock：结果 999 而非内置和
  bool saw_999 = false;
  for (auto const& m : (*st)["messages"])
    if (m.is_object() && m.value("type", "") == "tool")
      if (message_text(m).find("999") != std::string::npos)
        saw_999 = true;
  EXPECT_TRUE(saw_999) << (*st)["messages"].dump();
}

// 12) parse → to_json → parse 往返保真（守卫 ToolSource 结构不改，select 源三字段不丢）
TEST(WfToolRegistrySelect, parse_write_roundtrips_select_source) {
  Json ir = wf_with_tool_registry(
      Json::array({Json{{"from", "builtin"}, {"type", "select"},
                        {"load", Json::array({std::string(kToolSum)})}}}));
  auto p1 = WorkflowIR::parse(ir);
  ASSERT_TRUE(p1);
  Json out = p1->to_json();
  auto p2 = WorkflowIR::parse(out);
  ASSERT_TRUE(p2);
  ASSERT_EQ(p2->nodes.size(), p1->nodes.size());
  ASSERT_GE(p2->nodes[0].tool_sources.size(), 1u);
  EXPECT_EQ(p2->nodes[0].tool_sources[0].from, "builtin");
  EXPECT_EQ(p2->nodes[0].tool_sources[0].type, "select");
  ASSERT_EQ(p2->nodes[0].tool_sources[0].load.size(), 1u);
  EXPECT_EQ(p2->nodes[0].tool_sources[0].load[0], std::string(kToolSum));
}