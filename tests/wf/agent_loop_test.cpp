// M12 测试：图上的 agent 循环（对齐 langgraph ReAct，见 02-arch-design/11）
//
// 验证的核心命题：**agent 循环是图上的回边，不是节点内部的循环** ——
//   [L:label] → [think:model] → [route] ─有 tool_calls→ [act:run_tools] → [loop:goto L]
//                                        └text→ _turn_end
// model 节点只发**一次**请求；多轮往返由 route + run_tools + goto 构成。
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <memory>
#include <string>
#include <vector>

#include "cppchain/message/chunk.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using namespace demo::fakes;
using cppchain::message::Message;

namespace {

// 脚本化模型：按顺序吐预置回复（每轮一次 invoke）
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

Message ai_tool_call(std::string const& name, Json const& args, std::string const& id = "c1") {
  Message m = cppchain::message::ai_message("");
  m.tool_calls.push_back(message::ToolCall{id, name, args});
  return m;
}

Message ai_invalid_tool_call(std::string const& name) {
  Message m = cppchain::message::ai_message("");
  m.invalid_tool_calls.push_back(
      message::InvalidToolCall{"bad1", name, "arguments not parseable"});
  return m;
}

// 规范 §4「M12：agent 循环的规范写法」的**逐字形态**（5 个节点；无 history，C 阶段刻意先不接）。
// 刻意用文档里的写法（goto 靠键判别、不带 kind），使"规范里的示例真能编译"被测试锁住 ——
// 上一版规范就吃过与实现脱节的亏（$result vs $_.result）。
Json react_ir() {
  return Json{
      {"id", "wf_react"},
      {"trigger", Json{{"source", "time"},
                       {"timing", Json{{"kind", "schedule"}, {"schedule", "0 8 * * *"}}}}},
      {"nodes", Json::array(
                    {Json{{"id", "L"}, {"kind", "label"}},
                     Json{{"id", "think"},
                          {"kind", "model"},
                          {"model", "m"},
                          {"tools", "ts"},
                          {"next", "route"}},
                     Json{{"id", "route"},
                          {"kind", "route"},
                          {"on", "last_ai_outcome"},
                          {"cases", Json{{"tool_calls", "act"},
                                         {"invalid_tool_calls", "_fail"},
                                         {"text", "_turn_end"}}},
                          {"default", "_fail"}},
                     Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "loop"}},
                     Json{{"id", "loop"}, {"goto", "L"}, {"max", 4}}})}};
}

struct Fixture {
  FakeToolSinks sinks;
  tool::ToolRegistry registry = make_fake_tools(sinks);
  ScriptedModel model;
  ComponentRegistry components;

  Fixture() {
    components.models["m"] = &model;
    components.toolsets["ts"] = {"fetch_price"};
  }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    return o;
  }
};

}  // namespace

// 核心：模型先请求工具 → 工具执行 → 再发一次 → 拿到文本 → 结束。
// 关键断言：**模型被调用了 2 次**（证明 model 节点只发一次、循环靠回边），
// 且 messages 通道按 add_messages 累积了 [ai(tool_call), tool, ai(text)]。
TEST(WfAgentLoop, react_cycle_runs_tool_then_terminates) {
  Fixture f;
  f.sinks.price_lookup = [](std::string const&) {
    return Json{{"price", 599}, {"in_stock", true}};
  };
  f.model.replies.push_back(ai_tool_call("fetch_price", Json{{"item_id", "P1"}}));
  f.model.replies.push_back(cppchain::message::ai_message("价格 599，可以买"));

  auto ir = WorkflowIR::parse(react_ir());
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, f.registry, nullptr, f.opts());
  ASSERT_TRUE(wf) << wf.error().message;

  auto r = fire_once(*wf, Json::object());
  ASSERT_TRUE(r) << r.error().message;

  EXPECT_EQ(f.model.invoked, 2u) << "model 节点应各发一次：一轮工具往返 = 2 次调用";
  ASSERT_TRUE(r->contains("messages"));
  Json const& msgs = (*r)["messages"];
  ASSERT_EQ(msgs.size(), 3u) << "应为 [ai(tool_call), tool(result), ai(text)]";
  EXPECT_EQ(msgs[0].at("type").get<std::string>(), "ai");
  EXPECT_EQ(msgs[1].at("type").get<std::string>(), "tool");
  EXPECT_EQ(msgs[2].at("type").get<std::string>(), "ai");
  EXPECT_EQ(msgs[1].at("tool_call_id").get<std::string>(), "c1");  // 协议闭环
  EXPECT_EQ(msgs[2].at("content")[0].at("text").get<std::string>(), "价格 599，可以买");
}

// 纯文本回复：一轮就结束（model 只调 1 次），不碰工具
TEST(WfAgentLoop, text_reply_ends_without_tool_round) {
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("没什么要查的"));

  auto ir = WorkflowIR::parse(react_ir());
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, f.registry, nullptr, f.opts());
  ASSERT_TRUE(wf) << wf.error().message;

  auto r = fire_once(*wf, Json::object());
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(f.model.invoked, 1u);
  EXPECT_EQ((*r)["messages"].size(), 1u);
}

// **这一档就是文档 §5.2.1 查出的缺口**：模型请求了工具但参数坏了。
// 修复前：tool_calls 为空 → 被当成"文本回复"→ 静默当最终答复返回。
// 修复后：route 走 invalid_tool_calls 档 → _fail → 以错误终止（fail-closed）。
TEST(WfAgentLoop, invalid_tool_call_fails_closed_instead_of_silently_ending) {
  Fixture f;
  f.model.replies.push_back(ai_invalid_tool_call("fetch_price"));

  auto ir = WorkflowIR::parse(react_ir());
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, f.registry, nullptr, f.opts());
  ASSERT_TRUE(wf) << wf.error().message;

  auto r = fire_once(*wf, Json::object());
  ASSERT_FALSE(r) << "非法工具调用不得被当成正常结束";
  EXPECT_NE(r.error().message.find("_fail"), std::string::npos) << r.error().message;
}

// 循环上界：模型一直要工具 → goto 的 max 把它截停（不是无限循环）
TEST(WfAgentLoop, loop_is_bounded_by_goto_max) {
  Fixture f;
  for (int i = 0; i < 20; ++i)
    f.model.replies.push_back(ai_tool_call("fetch_price", Json{{"item_id", "P"}}));

  auto ir = WorkflowIR::parse(react_ir());
  ASSERT_TRUE(ir) << ir.error();
  auto wf = compile_workflow(*ir, f.registry, nullptr, f.opts());
  ASSERT_TRUE(wf) << wf.error().message;

  auto r = fire_once(*wf, Json::object());
  // max=4 → 最多 4 轮工具往返（4×2=8 次模型调用），不会跑满 20 条脚本
  EXPECT_LE(f.model.invoked, 8u) << "有界回跳失效";
}

// ---- 校验规则（VN10）----

TEST(WfAgentLoop, validator_rejects_unknown_component) {
  FakeToolSinks sinks;
  auto reg = make_fake_tools(sinks);
  ScriptedModel model;
  ComponentRegistry comps;
  comps.models["m"] = &model;
  comps.toolsets["ts"] = {"fetch_price"};

  auto ir_json = react_ir();
  ir_json["nodes"][1]["model"] = "不存在的组件";

  ValidatorOptions o{&reg};
  o.components = &comps;
  auto r = validate_json(ir_json, o);
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VN10"), std::string::npos) << r.describe();
}

TEST(WfAgentLoop, validator_requires_route_to_cover_all_outcomes_or_default) {
  FakeToolSinks sinks;
  auto reg = make_fake_tools(sinks);

  auto ir_json = react_ir();
  ir_json["nodes"][2] = Json{{"id", "route"},
                             {"kind", "route"},
                             {"on", "last_ai_outcome"},
                             {"cases", Json{{"tool_calls", "act"}}},  // 只覆盖一档
                             {"default", ""}};                        // 且没有 default

  auto r = validate_json(ir_json, ValidatorOptions{&reg});
  ASSERT_FALSE(r.ok()) << "漏档必须被拒（否则会静默走错分支）";
  EXPECT_NE(r.describe().find("VN10"), std::string::npos) << r.describe();
}

TEST(WfAgentLoop, validator_rejects_unknown_outcome_key) {
  FakeToolSinks sinks;
  auto reg = make_fake_tools(sinks);

  auto ir_json = react_ir();
  ir_json["nodes"][2]["cases"]["编造的档"] = "act";

  auto r = validate_json(ir_json, ValidatorOptions{&reg});
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.describe().find("VN10"), std::string::npos) << r.describe();
}

// 规范 §3：kind 是通用判别键，历史写法（靠键判别）继续支持 —— 两者必须等价。
// （这条是回归：我一度只给新 kind 加显式判别，导致 {"kind":"goto"} 反被拒。）
TEST(WfAgentLoop, kind_key_and_legacy_discrimination_agree) {
  auto k = [](Json const& node) { return WorkflowIR::classify(node); };

  EXPECT_EQ(*k(Json{{"id", "x"}, {"goto", "L"}, {"max", 1}}), NodeKind::Goto);
  EXPECT_EQ(*k(Json{{"id", "x"}, {"kind", "goto"}, {"goto", "L"}, {"max", 1}}), NodeKind::Goto);
  EXPECT_EQ(*k(Json{{"id", "x"}, {"kind", "label"}}), NodeKind::Label);
  EXPECT_EQ(*k(Json{{"id", "x"}, {"kind", "model"}, {"model", "m"}, {"next", "y"}}),
            NodeKind::Model);

  // 未知 kind 值 → 拒绝
  EXPECT_FALSE(k(Json{{"id", "x"}, {"kind", "编造的"}}).has_value());
  // 与声明**冲突**的键 → 拒绝（fail-closed）
  EXPECT_FALSE(k(Json{{"id", "x"}, {"kind", "tool"}, {"expr", "true"}}).has_value());
  // 与声明**一致**的冗余键 → 允许
  EXPECT_TRUE(k(Json{{"id", "x"}, {"kind", "tool"}, {"tool", "echo"},
                     {"next", "_turn_end"}}).has_value());
}
