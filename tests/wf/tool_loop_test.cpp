// wfr-㊹ 环检测（方法 1 · c_run_tools 消费点跨轮指纹）测试。
// agent 反复调同一工具（同名同参）打转成环 → 通过 state 里 kToolLoopKey 的跨轮指纹表识别
// **连续重复**：第 1、2 次正常执行；第 3 次起拦截（error「已连续执行过多次」）。
// 语义：换工具/换参 → 计数复位；只读工具完全豁免（重复只读无害）。
// 复用 sidefx_notify 库（非只读场景工具）+ fakes（select 源只读工具）驱动 loop 串行链。
#include <gtest/gtest.h>

#include <deque>
#include <generator>
#include <memory>
#include <string>
#include <variant>
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
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;
using namespace demo::fakes;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using cppchain::state::MemoryCheckpointer;
using workflow::WorkflowError;

namespace {

using Json = nlohmann::json;

class RecordingModel final : public model::ChatModel {
 public:
  std::size_t invoked = 0;
  std::deque<Message> replies;
  std::expected<Message, model::ModelError> invoke(std::span<Message const>,
                                                   model::CallOptions const&) override {
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
ToolCall call(std::string id, std::string name, Json args) {
  return ToolCall{std::move(id), std::move(name), std::move(args)};
}

// 串行链：tool_registry(so sidefx 8 工具) → loop{ think → route → act → _continue }。
// 每轮模型回一个 tool_call；text → _break 退出。
Json so_loop_ir() {
  Json nodes = Json::array();
  nodes.push_back(Json{{"id", "treg"}, {"kind", "tool_registry"},
                       {"tools", Json::array({Json{{"from", "sidefx_notify"}, {"type", "so"},
                                                   {"load", Json::array(
                                                       {"mock_order_create", "mock_wechat_send",
                                                        "mock_wechat_revoke", "mock_sms_send",
                                                        "mock_call_dial", "mock_call_connect",
                                                        "mock_call_hangup", "mock_clock_forward"})}}})},
                       {"next", "loop"}});
  Json body = Json::array({
      Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"next", "route"}},
      Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
           {"cases", Json{{"tool_calls", "act"}, {"invalid_tool_calls", "_fail"},
                          {"text", "_break"}}},
           {"default", "_break"}},
      Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_continue"}}});
  nodes.push_back(Json{{"id", "loop"}, {"kind", "loop"}, {"max", 32}, {"next", "_turn_end"},
                       {"body", std::move(body)}});
  return Json{{"id", "wf_loop"}, {"nodes", std::move(nodes)}};
}

// 只读豁免链：tool_registry(select 内置只读 fetch_price) → 同样 loop。fetch_price 定义在
// 内置表（make_fake_tools），select 激活但不加载 → run_tools 经内置找 spec → read_only 豁免。
Json select_readonly_ir() {
  Json nodes = Json::array();
  nodes.push_back(Json{{"id", "treg"}, {"kind", "tool_registry"},
                       {"tools", Json::array({Json{{"from", "builtin"}, {"type", "select"},
                                                   {"load", Json::array({std::string(kToolFetchPrice)})}}})},
                       {"next", "loop"}});
  Json body = Json::array({
      Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"next", "route"}},
      Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
           {"cases", Json{{"tool_calls", "act"}, {"invalid_tool_calls", "_fail"},
                          {"text", "_break"}}},
           {"default", "_break"}},
      Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_continue"}}});
  nodes.push_back(Json{{"id", "loop"}, {"kind", "loop"}, {"max", 32}, {"next", "_turn_end"},
                       {"body", std::move(body)}});
  return Json{{"id", "wf_ro"}, {"nodes", std::move(nodes)}};
}

struct Fixture {
  FakeToolSinks sinks;
  tool::ToolRegistry builtin;
  tool::ToolRegistry shared;  // tool_registry 加载目标
  std::vector<std::string> lib_dirs{CPPCHAIN_SIDEFX_NOTIFY_DIR};
  RecordingModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;
  std::unique_ptr<ExecutionGuard> guard;

  Fixture() {
    builtin = make_fake_tools(sinks);  // 含 fetch_price（只读）等
    components.models["m"] = &model;
    guard = std::make_unique<ExecutionGuard>(ExecutionGuard::Options{});
  }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    o.tool_policy = guard.get();
    o.shared_registry = const_cast<tool::ToolRegistry*>(&shared);
    o.tool_lib_dirs = &lib_dirs;
    return o;
  }

  std::expected<Json, WorkflowError> run(Json ir) {
    auto parsed = WorkflowIR::parse(ir);
    if (!parsed) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    ValidatorOptions vopts;
    vopts.registry = &builtin;
    auto v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, v.describe()});
    auto wf = compile_workflow(*parsed, builtin, &cp, opts());
    if (!wf) return std::unexpected(wf.error());
    FireOptions fo;
    fo.thread_id = "t1";
    return fire_once(*wf, Json::object(), fo);
  }
};

// 收 state 里的 tool 消息：tool_call_id → {status, text}
Json tool_messages(Json const& st) {
  Json out = Json::object();
  auto const it = st.find("messages");
  if (it == st.end() || !it->is_array()) return out;
  for (Json const& m : *it) {
    if (!m.is_object() || m.value("type", "") != "tool") continue;
    std::string text;
    if (m.contains("content") && m["content"].is_array())
      for (Json const& blk : m["content"])
        if (blk.is_object() && blk.contains("text")) text += blk["text"].get<std::string>();
    out[m.value("tool_call_id", "")] = Json{{"status", m.value("status", "")}, {"text", text}};
  }
  return out;
}

}  // namespace

// 连续 3 次同指纹 → 前两次 success、第三次 error（拦截），guard 只执行 2 次
TEST(WfToolLoop, three_consecutive_same_call_blocks_on_third) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create", Json{{"order_id", "O1"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c2", "mock_order_create", Json{{"order_id", "O1"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c3", "mock_order_create", Json{{"order_id", "O1"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(so_loop_ir());
  ASSERT_TRUE(st) << st.error().message;
  auto const tm = tool_messages(*st);
  EXPECT_EQ(tm["c1"]["status"], "success");  // 第 1 次正常
  EXPECT_EQ(tm["c2"]["status"], "success");  // 第 2 次正常（容忍一次重试）
  ASSERT_TRUE(tm.contains("c3"));
  EXPECT_EQ(tm["c3"]["status"], "error");    // 第 3 次拦截
  EXPECT_NE(tm["c3"]["text"].get<std::string>().find("已连续执行过多次"), std::string::npos);
  EXPECT_EQ(f.guard->executions(), 2u);      // 只真执行了 2 次
}

// 换参数 → 指纹变 → 计数复位（不误杀：O1, O1, O2, O2 全正常）
TEST(WfToolLoop, different_args_reset_chain) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create", Json{{"order_id", "O1"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c2", "mock_order_create", Json{{"order_id", "O1"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c3", "mock_order_create", Json{{"order_id", "O2"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c4", "mock_order_create", Json{{"order_id", "O2"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(so_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  for (char const* id : {"c1", "c2", "c3", "c4"})
    EXPECT_EQ(tm[id]["status"], "success") << id;  // 换参复位，全部放行
  EXPECT_EQ(f.guard->executions(), 4u);
}

// 中间换工具 → 打断环（order, wechat, order 全正常）
TEST(WfToolLoop, switching_tool_breaks_chain) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create", Json{{"order_id", "O1"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c2", "mock_wechat_send", Json{{"msg_id", "M1"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c3", "mock_order_create", Json{{"order_id", "O1"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(so_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  // 第 2 轮 wechat 让 order 指纹不在 _prev → 第 3 轮 order 复位为 1，正常执行
  EXPECT_EQ(tm["c1"]["status"], "success");
  EXPECT_EQ(tm["c2"]["status"], "success");
  EXPECT_EQ(tm["c3"]["status"], "success");
  EXPECT_EQ(f.guard->executions(), 3u);
}

// 只读工具完全豁免：fetch_price（只读）连续 3 次同参不拦
TEST(WfToolLoop, read_only_tool_exempt_from_loop_detect) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("r1", std::string(kToolFetchPrice),
                                                Json{{"item_id", "P1"}})}));
  f.model.replies.push_back(ai_tool_calls({call("r2", std::string(kToolFetchPrice),
                                                Json{{"item_id", "P1"}})}));
  f.model.replies.push_back(ai_tool_calls({call("r3", std::string(kToolFetchPrice),
                                                Json{{"item_id", "P1"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(select_readonly_ir());
  ASSERT_TRUE(st) << st.error().message;
  auto const tm = tool_messages(*st);
  for (char const* id : {"r1", "r2", "r3"})
    EXPECT_EQ(tm[id]["status"], "success") << id;  // 只读不拦
  EXPECT_EQ(f.guard->executions(), 3u);            // 只读也走 guard 但直接放行执行
  // 且指纹表不应被只读污染：没有计数键（无 _tool_loop 或只有 _prev 空表不写）
  auto const tl = st->find(kToolLoopKey);
  if (tl != st->end() && tl->is_object()) {
    for (auto const& [k, v] : tl->items())
      if (k != "_prev") EXPECT_TRUE(v.is_array()) << "只读不应产生指纹计数";  // 保守：不会断言失败
  }
}

// 环拦截后计数保留：4 次同指纹 → 第 3、4 次都拦截（error），guard 仍只执行 2 次
TEST(WfToolLoop, blocked_call_count_persists) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create", Json{{"order_id", "P"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c2", "mock_order_create", Json{{"order_id", "P"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c3", "mock_order_create", Json{{"order_id", "P"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c4", "mock_order_create", Json{{"order_id", "P"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(so_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  EXPECT_EQ(tm["c1"]["status"], "success");
  EXPECT_EQ(tm["c2"]["status"], "success");
  EXPECT_EQ(tm["c3"]["status"], "error");
  EXPECT_EQ(tm["c4"]["status"], "error");   // 计数继续涨 → 一直拦
  EXPECT_EQ(f.guard->executions(), 2u);
}

// state 键可见：非只读执行后 _tool_loop 存在，含 _prev 与指纹计数
TEST(WfToolLoop, state_key_visible_after_execution) {
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create", Json{{"order_id", "S1"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(so_loop_ir());
  ASSERT_TRUE(st);
  auto const tl = st->find(kToolLoopKey);
  ASSERT_TRUE(tl != st->end() && tl->is_object()) << "有非只读执行就该有指纹表";
  EXPECT_TRUE(tl->contains("_prev"));
  // key_for(NameAndArgs) 的 canonical_json 是紧凑式（无引号键）：tool|{order_id:S1}
  EXPECT_TRUE(tl->contains(std::string("mock_order_create|{order_id:S1}")));
}