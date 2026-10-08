// tests/wf —— wfr-㉙：model 节点的厂商参数透传（params）与出站 reasoning_echo 开关。
//   校验 VN13（params 保留键黑名单 / reasoning_echo 词表）、运行期 @config 解析、
//   "缺键不发该参数"的安全语义、以及 t_model 钩子让 params 参与 VR1/VR4。
#include <gtest/gtest.h>

#include <deque>
#include <expected>
#include <generator>
#include <span>
#include <string>
#include <vector>

#include "cppchain/message/chunk.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::message::Message;
using cppchain::message::Role;
using cppchain::message::TextBlock;

namespace {

// 记录最后一次 CallOptions 的桩模型（我们要断言的是"节点把什么传给了模型"）。
class CapturingModel final : public model::ChatModel {
 public:
  model::CallOptions last_opts;
  bool called = false;

  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const& opts) override {
    last_opts = opts;
    called = true;
    Message m;
    m.type = Role::Ai;
    m.content.push_back(TextBlock{"ok", std::nullopt});
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

struct Fixture {
  CapturingModel model;
  ComponentRegistry components;
  tool::ToolRegistry registry;

  Fixture() { components.models["m"] = &model; }

  static Json ir_of(Json model_node) {
    model_node["kind"] = "model";
    model_node["model"] = "m";
    if (!model_node.contains("next")) model_node["next"] = "_turn_end";
    return Json{{"id", "wf_mp"}, {"nodes", Json::array({model_node})}};
  }

  std::expected<CompiledWorkflow, WorkflowError> compile(
      Json const& ir_json, std::optional<std::vector<std::string>> config_keys = std::nullopt) {
    auto ir = WorkflowIR::parse(ir_json);
    if (!ir) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, ir.error()});
    CompileOptions opts;
    opts.components = &components;
    opts.config_keys = std::move(config_keys);
    return compile_workflow(*ir, registry, nullptr, opts);
  }
};

}  // namespace

// ---- 序列化往返 ----

TEST(WfModelParams, parse_write_parse_keeps_params_and_reasoning_echo) {
  Json const src = Fixture::ir_of(Json{{"id", "m0"},
                                       {"params", Json{{"reasoning_effort", "high"},
                                                       {"top_p", 0.9}}},
                                       {"reasoning_echo", "tool_calls"}});
  auto ir = WorkflowIR::parse(src);
  ASSERT_TRUE(ir) << ir.error();

  Json const round = ir->to_json();
  auto again = WorkflowIR::parse(round);
  ASSERT_TRUE(again) << again.error();

  NodeIR const& n = again->nodes.at(0);
  EXPECT_EQ(n.model_params.at("reasoning_effort"), "high");
  EXPECT_EQ(n.model_params.at("top_p"), 0.9);
  EXPECT_EQ(n.reasoning_echo, "tool_calls");
}

// ---- VN13：params 的保留键黑名单 + reasoning_echo 词表 ----

TEST(WfModelParams, vn13_rejects_non_object_params) {
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{{"id", "m0"}, {"params", "high"}}));
  ASSERT_FALSE(wf);
  EXPECT_NE(wf.error().message.find("VN13"), std::string::npos) << wf.error().message;
  EXPECT_NE(wf.error().message.find("必须是对象"), std::string::npos) << wf.error().message;
}

TEST(WfModelParams, vn13_rejects_reserved_keys_that_would_override_the_body) {
  // extra 合并是"右胜"（build_request），所以结构字段必须挡在编译期。
  for (char const* reserved : {"model", "messages", "stream", "tools", "temperature"}) {
    auto f = Fixture{};
    auto wf = f.compile(Fixture::ir_of(Json{{"id", "m0"}, {"params", Json{{reserved, "x"}}}}));
    ASSERT_FALSE(wf) << "保留键 " << reserved << " 竟然通过了";
    EXPECT_NE(wf.error().message.find("VN13"), std::string::npos) << wf.error().message;
    EXPECT_NE(wf.error().message.find("保留键"), std::string::npos) << wf.error().message;
  }
}

TEST(WfModelParams, vn13_accepts_lookalike_keys_that_are_not_reserved) {
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(
      Json{{"id", "m0"}, {"params", Json{{"reasoning_effort", "high"}, {"top_p", 1}}}}));
  EXPECT_TRUE(wf) << wf.error().message;
}

TEST(WfModelParams, vn13_rejects_unknown_reasoning_echo_literal) {
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{{"id", "m0"}, {"reasoning_echo", "sometimes"}}));
  ASSERT_FALSE(wf);
  EXPECT_NE(wf.error().message.find("VN13"), std::string::npos) << wf.error().message;
  EXPECT_NE(wf.error().message.find("词表"), std::string::npos) << wf.error().message;
}

TEST(WfModelParams, vn13_lets_a_config_reference_through_for_runtime_checking) {
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{{"id", "m0"}, {"reasoning_echo", "@config.echo"}}));
  EXPECT_TRUE(wf) << wf.error().message;  // 词表判定推迟到运行期
}

// ---- 运行期：params 解析与"缺键不发" ----

TEST(WfModelParams, params_resolve_config_references_at_runtime) {
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{
      {"id", "m0"},
      {"params", Json{{"reasoning_effort", "@config.effort"}, {"top_p", 0.5}}}}));
  ASSERT_TRUE(wf) << wf.error().message;

  auto r = fire_once(*wf, Json{{"effort", "high"}});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(f.model.last_opts.extra.at("reasoning_effort"), "high");
  EXPECT_EQ(f.model.last_opts.extra.at("top_p"), 0.5);
}

TEST(WfModelParams, missing_config_key_drops_the_param_instead_of_sending_null) {
  // @config 缺键 → resolve_ref 得 null；若照发就会变成 "reasoning_effort": null（显式设值）。
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{
      {"id", "m0"},
      {"params", Json{{"reasoning_effort", "@config.effort"}, {"top_p", 0.5}}}}));
  ASSERT_TRUE(wf) << wf.error().message;

  auto r = fire_once(*wf, Json::object());  // 不给 effort
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_FALSE(f.model.last_opts.extra.contains("reasoning_effort"));
  EXPECT_EQ(f.model.last_opts.extra.at("top_p"), 0.5);  // 字面量不受影响
}

// ---- 运行期：reasoning_echo 三档 + 缺省/非法语义 ----

TEST(WfModelParams, reasoning_echo_defaults_to_drop_when_absent) {
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{{"id", "m0"}}));
  ASSERT_TRUE(wf) << wf.error().message;
  ASSERT_TRUE(fire_once(*wf, Json::object()));
  EXPECT_EQ(f.model.last_opts.reasoning_echo, model::ReasoningEcho::Drop);
}

TEST(WfModelParams, reasoning_echo_literal_wires_through) {
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{{"id", "m0"}, {"reasoning_echo", "always"}}));
  ASSERT_TRUE(wf) << wf.error().message;
  ASSERT_TRUE(fire_once(*wf, Json::object()));
  EXPECT_EQ(f.model.last_opts.reasoning_echo, model::ReasoningEcho::Always);
}

TEST(WfModelParams, reasoning_echo_from_config_wires_through) {
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{{"id", "m0"}, {"reasoning_echo", "@config.echo"}}));
  ASSERT_TRUE(wf) << wf.error().message;
  ASSERT_TRUE(fire_once(*wf, Json{{"echo", "tool_calls"}}));
  EXPECT_EQ(f.model.last_opts.reasoning_echo, model::ReasoningEcho::ToolCalls);
}

TEST(WfModelParams, reasoning_echo_falls_back_to_drop_when_the_key_is_missing) {
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{{"id", "m0"}, {"reasoning_echo", "@config.echo"}}));
  ASSERT_TRUE(wf) << wf.error().message;
  ASSERT_TRUE(fire_once(*wf, Json::object()));
  EXPECT_EQ(f.model.last_opts.reasoning_echo, model::ReasoningEcho::Drop);
}

TEST(WfModelParams, reasoning_echo_typo_at_runtime_is_a_hard_error) {
  // fail-closed：`--set reasoning_echo=alway` 不该被静默吞成默认档。
  auto f = Fixture{};
  auto wf = f.compile(Fixture::ir_of(Json{{"id", "m0"}, {"reasoning_echo", "@config.echo"}}));
  ASSERT_TRUE(wf) << wf.error().message;

  auto r = fire_once(*wf, Json{{"echo", "alway"}});
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("不在词表里"), std::string::npos) << r.error().message;
}

// ---- t_model 钩子：params/reasoning_echo 参与 VR1/VR4 ----

TEST(WfModelParams, params_references_take_part_in_the_config_contract) {
  // 少了 t_model 钩子，params 里的 @config 引用对校验器是隐形的 —— 这条会失败。
  Json const ir_json = Fixture::ir_of(Json{
      {"id", "m0"}, {"params", Json{{"reasoning_effort", "@config.wrokspace"}}}});
  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_TRUE(ir) << ir.error();

  ValidatorOptions opts;
  opts.config_keys = std::vector<std::string>{"workspace"};
  auto const vr = validate(*ir, opts);
  ASSERT_FALSE(vr.ok());
  bool saw_vr4 = false;
  for (auto const& e : vr.errors)
    if (e.rule == "VR4") saw_vr4 = true;
  EXPECT_TRUE(saw_vr4) << vr.describe();
}

TEST(WfModelParams, reasoning_echo_reference_takes_part_in_the_config_contract) {
  Json const ir_json = Fixture::ir_of(Json{{"id", "m0"}, {"reasoning_echo", "@config.echo"}});
  auto ir = WorkflowIR::parse(ir_json);
  ASSERT_TRUE(ir) << ir.error();

  ValidatorOptions opts;
  opts.config_keys = std::vector<std::string>{"workspace"};
  auto const vr = validate(*ir, opts);
  ASSERT_FALSE(vr.ok());
  EXPECT_NE(vr.describe().find("VR4"), std::string::npos) << vr.describe();
}
