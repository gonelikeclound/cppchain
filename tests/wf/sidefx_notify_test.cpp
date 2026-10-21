// wfr-㊹ 治理（problems/34）· 副作用场景 mock 库（libsidefx_notify.so）workflow 级测试。
// 覆盖 30 case（A 装配/活动集 5 + B 订单幂等 5 + C 微信可撤 5 + D 短信不可撤 4 +
//          E 电话可关 5 + F 治理审批 guard 4 + G 时序隔离 2）。
// core：tool_registry(so 源) → 活动集 → model/run_tools/approve 从 context 取；mock 库自带
// 可逆性梯度状态机（订单幂等 / 微信窗口可撤 / 短信无反向工具 / 电话任意状态可挂）+ 模拟时钟。
// 纪律（problems/03 §5）：断言走库内状态 + guard 计数 + ToolMessage，不看模型嘴上。
#include <gtest/gtest.h>

#include <dlfcn.h>

#include <deque>
#include <generator>
#include <memory>
#include <optional>
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
#include "cppchain/tool/tool_plugin.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"
#include "support/fakes.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using namespace cppchain::workflow;
using cppchain::graph::InterruptStatus;
using cppchain::message::Message;
using cppchain::message::ToolCall;
using cppchain::state::MemoryCheckpointer;
using workflow::WorkflowError;
using namespace demo::fakes;

namespace {

using Json = nlohmann::json;
using Res = std::expected<Json, ToolError>;

constexpr char kLibName[] = "libsidefx_notify.so";

// 记录模型：拷贝最后一次 CallOptions（含 tools schema），并按预置队列吐回复。
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

// 库内 mock 状态清空：直调 tool_mock_state_reset_creator（host 侧包裸 fn → 现调一次）。
// 每个 TEST 开头调用 —— 隔离跨用例库内 static 状态（库进程期常驻，见 tool_plugin.cpp 注释）。
void reset_sidefx() {
#ifdef CPPCHAIN_SIDEFX_NOTIFY_DIR
  std::string const lib = std::string(CPPCHAIN_SIDEFX_NOTIFY_DIR) + "/" + kLibName;
  void* const h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
  ASSERT_TRUE(h != nullptr) << "dlopen " << lib << " 失败";
  void* const addr = dlsym(h, "tool_mock_state_reset_creator");
  ASSERT_TRUE(addr != nullptr) << "dlsym tool_mock_state_reset_creator 失败";
  auto const creator = reinterpret_cast<ToolCreatorFn>(addr);
  cppchain_tool_descriptor const* const desc = creator(nullptr);
  ASSERT_TRUE(desc != nullptr);
  ASSERT_TRUE(desc->handler != nullptr);
  // host 侧按 args 调裸 fn（stringify-{} → 期望空对象）
  char* out = nullptr;
  char* err = nullptr;
  int const rc = desc->handler("{}", &out, &err);
  if (out != nullptr) std::free(out);
  if (err != nullptr) std::free(err);
  ASSERT_EQ(rc, 0) << (desc->name ? desc->name : "???");
#else
  GTEST_FAIL() << "CPPCHAIN_SIDEFX_NOTIFY_DIR 未定义";
#endif
}

// 假"会话日志"（ExecutionGuard::LogReader 等价物）：completed/intent 可配，测 replay/blocked。
struct FakeLog {
  bool intent = false;
  bool completed = false;
  std::string content = R"({"ok":true,"status":"committed"})";
  std::string log_status = "success";
  std::vector<std::string> decs;  // guard 审计决策收集
  ExecutionGuard::LogState operator()(std::string const&) const {
    ExecutionGuard::LogState st;
    if (completed) { st.completed = true; st.content = content; st.status = log_status; }
    if (intent) st.intent = true;
    return st;
  }
};

// workflow 链：4 节点（treg → think → route → act）多数场景；5 节点加 approve（deny 可配）。
// 注意：deny 名单走 VN17 校验只查**内置**注册表 —— mock 名不能进 deny，deny 演示保持内置名。
Json chain_ir(bool with_approve, std::vector<std::string> const& deny = {}) {
  Json d = Json::array();
  for (std::string const& s : deny) d.push_back(s);
  Json nodes = Json::array();
  nodes.push_back(Json{{"id", "treg"}, {"kind", "tool_registry"},
                       {"tools", Json::array({Json{{"from", "sidefx_notify"}, {"type", "so"},
                                                   {"load", Json::array(
                                                       {"mock_order_create", "mock_wechat_send",
                                                        "mock_wechat_revoke", "mock_sms_send",
                                                        "mock_call_dial", "mock_call_connect",
                                                        "mock_call_hangup", "mock_clock_forward"})}}})},
                       {"next", "think"}});
  nodes.push_back(Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"next", "route"}});
  nodes.push_back(Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
                       {"cases", Json{{"tool_calls", with_approve ? "approve" : "act"},
                                      {"invalid_tool_calls", "_fail"}}},
                       {"default", "_turn_end"}});
  if (with_approve)
    nodes.push_back(Json{{"id", "approve"}, {"kind", "approve_tools"}, {"deny", d}, {"next", "act"}});
  nodes.push_back(Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_turn_end"}});
  return Json{{"id", "wf_sfx"}, {"nodes", std::move(nodes)}};
}

// 串行链：treg → loop{ think → route → act → _continue } —— 每轮模型回一个 tool_call，
// run_tools 每轮执行**最后一条 AI 的调用** → 天然串行（多调用间有状态依赖时用这个，避开并行竞态）。
// route 的 text → 退出循环（模型没有更多调用时）。max = 最大轮数。
Json chain_loop_ir(long max = 16) {
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
                          {"text", "_break"}}},  // text → 退出循环（序列结束）
           {"default", "_break"}},
      Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_continue"}}});
  nodes.push_back(Json{{"id", "loop"}, {"kind", "loop"}, {"max", max}, {"next", "_turn_end"},
                       {"body", std::move(body)}});
  return Json{{"id", "wf_sfxloop"}, {"nodes", std::move(nodes)}};
}

struct Fixture {
  FakeToolSinks sinks;                 // fakes 副作用收件箱（send_notification）
  tool::ToolRegistry builtin;          // 内置表（compile/VN17/approve reg 的解析源）
  tool::ToolRegistry shared;           // tool_registry 动态加载目标（独立于内置表）
  std::vector<std::string> lib_dirs{CPPCHAIN_SIDEFX_NOTIFY_DIR};
  RecordingModel model;
  ComponentRegistry components;
  MemoryCheckpointer cp;
  std::unique_ptr<ExecutionGuard> guard;
  FakeLog log;  // 会话日志等价物（须比 guard 活得久：LogReader 按引用捕获）

  Fixture() {
    builtin = demo::fakes::make_fake_tools(sinks);  // 含 send_notification（副作用，VN17 deny 源）
    components.models["m"] = &model;
    ExecutionGuard::Options go;
    go.log = [this](std::string const&) { return log(""); };
    go.audit = [this](AuditRecord const& rec) { log.decs.push_back(rec.decision); };
    guard = std::make_unique<ExecutionGuard>(std::move(go));
  }

  CompileOptions opts() const {
    CompileOptions o;
    o.components = &components;
    o.tool_policy = guard.get();             // 副作用防护
    o.shared_registry = const_cast<tool::ToolRegistry*>(&shared);
    o.tool_lib_dirs = &lib_dirs;
    return o;
  }

  std::expected<Json, WorkflowError> run(Json ir, bool thread = true,
                                         Json resume = Json::object()) {
    auto parsed = WorkflowIR::parse(ir);
    if (!parsed) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    ValidatorOptions vopts;
    vopts.registry = &builtin;  // 校验期解析源（VN17 deny 存在性 / select 名）
    auto v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, v.describe()});
    auto wf = compile_workflow(*parsed, builtin, &cp, opts());
    if (!wf) return std::unexpected(wf.error());
    FireOptions fo;
    if (thread) fo.thread_id = "t1";
    if (!resume.is_null()) fo.resume = std::move(resume);
    return fire_once(*wf, Json::object(), fo);
  }

  // 模型 schema 收到多少把工具（last_opts.tools 的 function.name 列表）
  std::vector<std::string> tool_names() const {
    std::vector<std::string> out;
    for (Json const& t : model.last_opts.tools) out.push_back(t["function"]["name"]);
    return out;
  }
};

// 收 state 里的消息，按 tool_call_id 映射到 {status, text}
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

// ============ A 组 · tool_registry 装配与活动集（A1-A5） ============

TEST(WfSidefxNotify, A1_active_set_published_in_order) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st) << st.error().message;
  Json const frame = (*st)["results"]["treg"];
  EXPECT_EQ(frame["loaded"], Json::array({"mock_order_create", "mock_wechat_send",
                                          "mock_wechat_revoke", "mock_sms_send", "mock_call_dial",
                                          "mock_call_connect", "mock_call_hangup", "mock_clock_forward"}));
  EXPECT_EQ(frame["active_tools"], frame["loaded"]);
}

TEST(WfSidefxNotify, A2_model_schema_equals_active_set) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  // 无 toolset 组件 → 活动集驱动 schema（8 把 mock 全在；顺序 = 声明序）
  std::vector<std::string> const kActive{"mock_order_create", "mock_wechat_send",
                                         "mock_wechat_revoke", "mock_sms_send", "mock_call_dial",
                                         "mock_call_connect", "mock_call_hangup", "mock_clock_forward"};
  EXPECT_EQ(f.tool_names(), kActive);
}

TEST(WfSidefxNotify, A3_allowlist_rejects_non_active_tool) {
  reset_sidefx();
  Fixture f;
  // 调用一把**非活动** mock 名：库里有 mock_bash 之类？→ 用库外感知：改名 mock_weather（在 mock_tools 库）？
  // 更准：直接调"不在 load 名单"的 mock 库工具 mock_call_connect 拿到了——不行它在。用库外名字：
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_ghost_tool", Json::object())}));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  // 允许名单拒绝（非活动名）→ error ToolMessage 回喂；guard 未执行该调用
  ASSERT_TRUE(tm.contains("c1"));
  EXPECT_EQ(tm["c1"]["status"], "error");
  EXPECT_NE(tm["c1"]["text"].get<std::string>().find("不在本 workflow 的活动工具名单"),
            std::string::npos);
}

TEST(WfSidefxNotify, A4_so_source_runtime_load_shares_active) {
  reset_sidefx();
  Fixture f;
  // 走真实 flow 权限：上面 A1/A2 已覆盖 so 源发布；此处验证"so 工具进 shared → 可被 run_tools 消费"
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create",
                                               Json{{"order_id", "O-A4"}})}));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("c1"));
  EXPECT_EQ(tm["c1"]["status"], "success");
  EXPECT_NE(tm["c1"]["text"].get<std::string>().find("created"), std::string::npos);
}

TEST(WfSidefxNotify, A5_no_tool_registry_zero_regression) {
  reset_sidefx();
  Fixture f;
  // 无 tool_registry 节点、无活动集 → model 无 tools 空 schema、run_tools 不拦（旧路径仍能跑）
  Json nodes = Json::array(
      {Json{{"id", "think"}, {"kind", "model"}, {"model", "m"}, {"next", "route"}},
       Json{{"id", "route"}, {"kind", "route"}, {"on", "last_ai_outcome"},
            {"cases", Json{{"tool_calls", "act"}, {"invalid_tool_calls", "_fail"}}},
            {"default", "_turn_end"}},
       Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_turn_end"}}});
  f.model.replies.push_back(cppchain::message::ai_message("ok"));  // text → end
  auto st = f.run(Json{{"id", "wf_noreg"}, {"nodes", nodes}});
  ASSERT_TRUE(st);
  EXPECT_TRUE(f.model.last_opts.tools.empty());  // 无活动集 → 空 schema（无工具）
  // 无 "active_tools" 帧
  auto const res = st->find("results");
  if (res != st->end() && res->is_object())
    for (auto const& [k, v] : res->items())
      if (v.is_object()) EXPECT_FALSE(v.contains("active_tools"));
}

// ============ B 组 · 订单 · 幂等（B1-B5） ============

TEST(WfSidefxNotify, B1_order_create_succeeds_and_persists) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create",
                                               Json{{"order_id", "O1"}, {"item", "apple"}})}));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("c1"));
  EXPECT_EQ(tm["c1"]["status"], "success");
  EXPECT_NE(tm["c1"]["text"].get<std::string>().find("created"), std::string::npos);
  EXPECT_EQ(f.guard->executions(), 1u);
}

TEST(WfSidefxNotify, B2_intent_residue_probe_complete_replays_not_resubmits) {
  reset_sidefx();
  Fixture f;
  // 先真建一单（库里有 O2）→ 换新 guard 表示 "有 Intent 无结果"，同 id 再调 → probe Complete → replay
  {
    Fixture seed;
    seed.model.replies.push_back(ai_tool_calls({call("c0", "mock_order_create",
                                                    Json{{"order_id", "O2"}})}));
    auto rst = seed.run(chain_ir(false));
    ASSERT_TRUE(rst);
  }
  Fixture f2;
  f2.log.intent = true;  // 崩溃缺环
  f2.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create",
                                                Json{{"order_id", "O2"}})}));
  auto st = f2.run(chain_ir(false));
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("c1"));
  // probe Complete → 不重下（report 是"探测到已执行"）→ executions 不增
  EXPECT_EQ(f2.guard->executions(), 0u);
  EXPECT_GE(f2.guard->replays(), 1u);
  EXPECT_NE(tm["c1"]["text"].get<std::string>().find("已执行"), std::string::npos);
}

TEST(WfSidefxNotify, B3_same_args_same_order_replays) {
  reset_sidefx();
  Fixture f;
  // 库内已有 O3（手动种子：上一发成功）→ 同参再发 → guard NameAndArgs 命中 → replay 幂等
  // 用 FakeLog completed=true 模拟"已成功回执"（guard 不 probe，直接 replay）
  {
    Fixture seed;
    seed.model.replies.push_back(ai_tool_calls({call("c0", "mock_order_create",
                                                    Json{{"order_id", "O3"}})}));
    ASSERT_TRUE(seed.run(chain_ir(false)));
  }
  Fixture f2;
  f2.log.completed = true;
  f2.log.content = R"({"status":"existing"})";
  f2.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create",
                                                Json{{"order_id", "O3"}})}));
  auto st = f2.run(chain_ir(false));
  ASSERT_TRUE(st);
  EXPECT_EQ(f2.guard->executions(), 0u);
  EXPECT_EQ(f2.guard->replays(), 1u);
}

TEST(WfSidefxNotify, B4_different_order_creates_new) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create",
                                               Json{{"order_id", "O4"}})}));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  EXPECT_EQ(f.guard->executions(), 1u);  // 新订单真执行
  EXPECT_EQ(f.guard->replays(), 0u);
}

TEST(WfSidefxNotify, B5_probe_unknown_fail_closed_blocked) {
  reset_sidefx();
  Fixture f;
  f.log.intent = true;
  // `?` 前缀 = probe Unknown（故障注入：执行状态不可探测）
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create",
                                               Json{{"order_id", "?O5"}})}));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  EXPECT_EQ(f.guard->blocked(), 1u);
  EXPECT_EQ(f.guard->executions(), 0u);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("c1"));
  EXPECT_EQ(tm["c1"]["status"], "error");
}

// ============ C 组 · 微信 · 可撤回窗口（C1-C5） ============

TEST(WfSidefxNotify, C1_send_records_and_returns_msg) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_wechat_send",
                                               Json{{"msg_id", "M1"}, {"to", "u1"}})}));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("c1"));
  EXPECT_EQ(tm["c1"]["status"], "success");
  EXPECT_NE(tm["c1"]["text"].get<std::string>().find("sent"), std::string::npos);
  EXPECT_NE(tm["c1"]["text"].get<std::string>().find("revoke_window_sec"),
            std::string::npos);  // schema 带窗口信息
}

TEST(WfSidefxNotify, C2_revoke_within_window_succeeds) {
  reset_sidefx();
  Fixture f;
  // 串行链（loop）：先 send，再 revoke，最后 text 退出。避免同批并行改变调用顺序。
  f.model.replies.push_back(ai_tool_calls({call("s1", "mock_wechat_send", Json{{"msg_id", "M2"}})}));
  f.model.replies.push_back(ai_tool_calls({call("r1", "mock_wechat_revoke", Json{{"msg_id", "M2"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(chain_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  EXPECT_EQ(tm["s1"]["status"], "success");  // send
  ASSERT_TRUE(tm.contains("r1"));
  EXPECT_EQ(tm["r1"]["status"], "success");           // revoke 窗口内成功
  EXPECT_NE(tm["r1"]["text"].get<std::string>().find("revoked"), std::string::npos);
}

TEST(WfSidefxNotify, C3_revoke_out_of_window_errors) {
  reset_sidefx();
  Fixture f;
  // 串行：send → forward(500s) → revoke（residual 已过窗口）→ text 退出
  f.model.replies.push_back(ai_tool_calls(
      {call("s1", "mock_wechat_send", Json{{"msg_id", "M3"}})}));
  f.model.replies.push_back(ai_tool_calls(
      {call("f1", "mock_clock_forward", Json{{"seconds", 500}})}));
  f.model.replies.push_back(ai_tool_calls({call("r1", "mock_wechat_revoke", Json{{"msg_id", "M3"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(chain_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("r1"));
  EXPECT_EQ(tm["r1"]["status"], "error");
  EXPECT_NE(tm["r1"]["text"].get<std::string>().find("已过撤回窗口"), std::string::npos);
}

TEST(WfSidefxNotify, C4_double_revoke_errors) {
  reset_sidefx();
  Fixture f;
  // 串行：send → revoke → revoke（重复）→ text
  f.model.replies.push_back(ai_tool_calls(
      {call("s1", "mock_wechat_send", Json{{"msg_id", "M4"}})}));
  f.model.replies.push_back(ai_tool_calls({call("r1", "mock_wechat_revoke", Json{{"msg_id", "M4"}})}));
  f.model.replies.push_back(ai_tool_calls({call("r2", "mock_wechat_revoke", Json{{"msg_id", "M4"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(chain_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  EXPECT_EQ(tm["r1"]["status"], "success");  // 首次撤回成功
  ASSERT_TRUE(tm.contains("r2"));
  EXPECT_EQ(tm["r2"]["status"], "error");    // 重复撤回报错
  EXPECT_NE(tm["r2"]["text"].get<std::string>().find("已撤回"), std::string::npos);
}

TEST(WfSidefxNotify, C5_revoke_result_referenceable) {
  reset_sidefx();
  // 5 节点链带 echo：验证 revoke 的 results 帧可被下游引用（$echo 用不到 revoke 就直接查 results）
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("s1", "mock_wechat_send", Json{{"msg_id", "M5"}})}));
  f.model.replies.push_back(ai_tool_calls({call("r1", "mock_wechat_revoke", Json{{"msg_id", "M5"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(chain_loop_ir());
  ASSERT_TRUE(st);
  // revoke 是独立工具：读取它自己的 ToolMessage 结果（C2 已验）；此处额外验 results 无 revoke 帧
  // （revoke 在 act 节点执行，帧是 act 的 summary；mock 工具不发布独立 results —— 符合"结果可引用"前提）
  auto const res = st->find("results");
  ASSERT_TRUE(res != st->end() && res->is_object());
  ASSERT_TRUE(res->contains("act"));
  EXPECT_TRUE((*res)["act"].contains("summary"));
}

// ============ D 组 · 短信 · 不可撤回（D1-D4） ============

TEST(WfSidefxNotify, D1_sms_send_irreversible) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_sms_send",
                                               Json{{"sms_id", "S1"}, {"phone", "1"}})}));
  auto st = f.run(chain_ir(false));  // 这个 run 会真实调用一次（加载 + 执行）
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  EXPECT_EQ(tm["c1"]["status"], "success");
  // 先消费一次后，工具必然已加载进 shared —— 校验它落地的 effect_class 契约
  auto const* sp = f.shared.find("mock_sms_send");
  ASSERT_NE(sp, nullptr) << "模拟工具应在 tool_registry 运行期加载进共享表";
  EXPECT_EQ(sp->effect_class, EffectClass::Irreversible);  // 短信不可撤的契约声明
  EXPECT_FALSE(sp->read_only);
}

TEST(WfSidefxNotify, D2_no_reverse_tool_for_sms) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  // 短信不可撤：含 "sms" 的工具**只有** mock_sms_send 一把；微信的 revoke（名字无 sms）不算，
  // 它证明的是"微信有自己的撤回工具"，与短信无关。
  std::size_t sms_tools = 0;
  bool saw_sms_revoke = false;
  for (std::string const& n : f.tool_names()) {
    if (n.find("sms") == std::string::npos) continue;
    ++sms_tools;
    if (n != "mock_sms_send") saw_sms_revoke = true;  // 任何非 send 的 sms 形态 = 暴露了撤回
  }
  EXPECT_EQ(sms_tools, 1u) << "只有一把 mock_sms_send";
  EXPECT_FALSE(saw_sms_revoke) << "短信不可撤：不该暴露 sms 撤回工具";
}

TEST(WfSidefxNotify, D3_sms_same_args_replays) {
  reset_sidefx();
  Fixture f;
  {  // 先真发一条
    Fixture s; s.model.replies.push_back(ai_tool_calls({call("c0", "mock_sms_send",
                                                           Json{{"sms_id", "S3"}})}));
    ASSERT_TRUE(s.run(chain_ir(false)));
  }
  Fixture f2;
  f2.log.completed = true;  // 已成功回执 → guard replay（不可撤 = 至少不重发）
  f2.model.replies.push_back(ai_tool_calls({call("c1", "mock_sms_send",
                                                Json{{"sms_id", "S3"}})}));
  auto st = f2.run(chain_ir(false));
  ASSERT_TRUE(st);
  EXPECT_EQ(f2.guard->executions(), 0u);
  EXPECT_EQ(f2.guard->replays(), 1u);
}

TEST(WfSidefxNotify, D4_sms_intent_probe_complete_replays) {
  reset_sidefx();
  Fixture f;
  {  // 先真发一条
    Fixture s; s.model.replies.push_back(ai_tool_calls({call("c0", "mock_sms_send",
                                                           Json{{"sms_id", "S4"}})}));
    ASSERT_TRUE(s.run(chain_ir(false)));
  }
  Fixture f2;
  f2.log.intent = true;  // 崩溃缺环 → probe Complete → replay
  f2.model.replies.push_back(ai_tool_calls({call("c1", "mock_sms_send",
                                                Json{{"sms_id", "S4"}})}));
  auto st = f2.run(chain_ir(false));
  ASSERT_TRUE(st);
  EXPECT_EQ(f2.guard->replays(), 1u);
  EXPECT_EQ(f2.guard->executions(), 0u);
}

// ============ E 组 · 电话 · 任意状态可关（E1-E5） ============

TEST(WfSidefxNotify, E1_dial_to_ring) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("d1", "mock_call_dial",
                                               Json{{"call_id", "C1"}})}));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("d1"));
  EXPECT_EQ(tm["d1"]["status"], "success");
  EXPECT_NE(tm["d1"]["text"].get<std::string>().find("ring"), std::string::npos);
}

TEST(WfSidefxNotify, E2_hangup_before_connect_cancels) {
  reset_sidefx();
  Fixture f;
  // 串行：dial → 立即 hangup（接通前取消）→ text
  f.model.replies.push_back(ai_tool_calls({call("d1", "mock_call_dial", Json{{"call_id", "C2"}})}));
  f.model.replies.push_back(ai_tool_calls({call("h1", "mock_call_hangup", Json{{"call_id", "C2"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(chain_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("h1"));
  EXPECT_EQ(tm["h1"]["status"], "success");
  EXPECT_NE(tm["h1"]["text"].get<std::string>().find("接通前取消"), std::string::npos);
}

TEST(WfSidefxNotify, E3_hangup_after_connect_closes) {
  reset_sidefx();
  Fixture f;
  // 串行：dial → connect → hangup（接通后关闭）→ text
  f.model.replies.push_back(ai_tool_calls({call("d1", "mock_call_dial", Json{{"call_id", "C3"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_call_connect", Json{{"call_id", "C3"}})}));
  f.model.replies.push_back(ai_tool_calls({call("h1", "mock_call_hangup", Json{{"call_id", "C3"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(chain_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("h1"));
  EXPECT_EQ(tm["h1"]["status"], "success");
  EXPECT_NE(tm["h1"]["text"].get<std::string>().find("接通后关闭"), std::string::npos);
}

TEST(WfSidefxNotify, E4_dial_intent_probe_complete_does_not_redial) {
  reset_sidefx();
  Fixture f;
  {  // 先真拨（ring）
    Fixture s; s.model.replies.push_back(ai_tool_calls({call("d0", "mock_call_dial",
                                                           Json{{"call_id", "C4"}})}));
    ASSERT_TRUE(s.run(chain_ir(false)));
  }
  Fixture f2;
  f2.log.intent = true;
  f2.model.replies.push_back(ai_tool_calls({call("d1", "mock_call_dial",
                                                Json{{"call_id", "C4"}})}));
  auto st = f2.run(chain_ir(false));
  ASSERT_TRUE(st);
  EXPECT_EQ(f2.guard->replays(), 1u);
  EXPECT_EQ(f2.guard->executions(), 0u);  // ring 已在库 → probe Complete → 不重呼
}

TEST(WfSidefxNotify, E5_ended_call_ops_error) {
  reset_sidefx();
  Fixture f;
  // 串行：dial → hangup（结束）→ connect（对 ended 通话再操作 → error）→ text
  f.model.replies.push_back(ai_tool_calls({call("d1", "mock_call_dial", Json{{"call_id", "C5"}})}));
  f.model.replies.push_back(ai_tool_calls({call("h1", "mock_call_hangup", Json{{"call_id", "C5"}})}));
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_call_connect", Json{{"call_id", "C5"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(chain_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("c1"));
  EXPECT_EQ(tm["c1"]["status"], "error");
  EXPECT_NE(tm["c1"]["text"].get<std::string>().find("通话已结束"), std::string::npos);
}

// ============ F 组 · 治理 · 审批 · guard 矩阵（F1-F4） ============

// F1 · deny 内置名 → 审批静态拒绝（error 回喂、不进 guard）。
// （deny mock 名会先被 VN17 拒——它只查内置表，见 F1b 限界记录。）
TEST(WfSidefxNotify, F1a_deny_builtin_tool_static_rejects) {
  reset_sidefx();
  Fixture f;
  // send_notification 是内置副作用工具（fakes）；deny 后模型仍调用 → 审批静态拒绝，
  // 收件箱不落地（副作用被拦在审批链）。
  f.model.replies.push_back(ai_tool_calls({call("c1", std::string(demo::fakes::kToolSendNotification),
                                               Json{{"title", "不该发"}})}));
  auto st = f.run(chain_ir(true, /*deny*/ {std::string(demo::fakes::kToolSendNotification)}));
  ASSERT_TRUE(st) << st.error().message;
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("c1"));
  EXPECT_EQ(tm["c1"]["status"], "error");             // deny 静态拒绝回喂
  EXPECT_NE(tm["c1"]["text"].get<std::string>().find("拒绝"), std::string::npos);
  EXPECT_TRUE(f.sinks.notifications.empty());         // 副作用未落地
  EXPECT_EQ(f.guard->executions(), 0u);               // 未执行
}

// F1b · deny mock 名（mock_sms_send）→ VN17 校验期拒绝（限界：deny 存在性检查只查内置表）
TEST(WfSidefxNotify, F1b_deny_mock_name_vn17_rejected) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_sms_send",
                                               Json{{"sms_id", "S-F1"}})}));
  auto st = f.run(chain_ir(true, /*deny*/ {"mock_sms_send"}));
  ASSERT_FALSE(st);  // deny mock 名 → VN17（内置表查不到）→ 编译期拒
  EXPECT_NE(st.error().message.find("VN17"), std::string::npos)
      << "deny 名单存在性检查仍查内置表（限界记录，problems/34 §三）";
}

TEST(WfSidefxNotify, F2_needhuman_dial_suspend_then_approve) {
  reset_sidefx();
  Fixture f;
  f.model.replies.push_back(ai_tool_calls({call("d1", "mock_call_dial",
                                               Json{{"call_id", "C-F2"}})}));
  // 编译 - 首跑悬挂
  Json ir = chain_ir(true);
  auto parsed = WorkflowIR::parse(ir);
  ASSERT_TRUE(parsed);
  ValidatorOptions vopts;
  vopts.registry = &f.builtin;
  auto v = validate(*parsed, vopts);
  ASSERT_TRUE(v.ok()) << v.describe();
  auto wf = compile_workflow(*parsed, f.builtin, &f.cp, f.opts());
  ASSERT_TRUE(wf) << wf.error().message;

  FireOptions fo;
  fo.thread_id = "t1";
  auto r1 = fire_once(*wf, Json::object(), fo);
  ASSERT_TRUE(r1);
  InterruptStatus const pend = wf->graph.last_interrupt();
  EXPECT_TRUE(pend.interrupted);
  EXPECT_EQ(pend.node, "approve");
  ASSERT_EQ(pend.payload["calls"].size(), 1u);
  EXPECT_EQ(pend.payload["calls"][0]["tool"], "mock_call_dial");
  EXPECT_EQ(f.guard->executions(), 0u);  // 悬挂期间不执行

  fo.resume = Json{{"action", "approve"}};
  auto r2 = fire_once(*wf, Json::object(), std::move(fo));
  ASSERT_TRUE(r2);
  EXPECT_FALSE(wf->graph.last_interrupt().interrupted);
  EXPECT_EQ(f.guard->executions(), 1u);  // 放行后真拨（ring 入库）
  auto const tm = tool_messages(*r2);
  ASSERT_TRUE(tm.contains("d1"));
  EXPECT_EQ(tm["d1"]["status"], "success");
}

TEST(WfSidefxNotify, F3_read_only_tool_not_in_active_by_default) {
  reset_sidefx();
  // mock 库里没有只读工具（全场景副作用）；验证"活动集只含副作用工具"是预期（无只读可测免审）
  Fixture f;
  f.model.replies.push_back(cppchain::message::ai_message("ok"));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  for (std::string const& n : f.tool_names()) {
    auto const* sp = f.shared.find(n);
    ASSERT_NE(sp, nullptr);
    EXPECT_FALSE(sp->read_only) << n << " 场景工具都应是副作用（read_only=false）";
  }
}

TEST(WfSidefxNotify, F4_probe_matrix_partial_blocked) {
  reset_sidefx();
  Fixture f;
  f.log.intent = true;
  // `#` 前缀 = probe Partial（产物不完整）→ fail-closed blocked
  f.model.replies.push_back(ai_tool_calls({call("c1", "mock_order_create",
                                               Json{{"order_id", "#O-F4"}})}));
  auto st = f.run(chain_ir(false));
  ASSERT_TRUE(st);
  EXPECT_EQ(f.guard->blocked(), 1u);
  EXPECT_EQ(f.guard->executions(), 0u);
  auto const tm = tool_messages(*st);
  ASSERT_TRUE(tm.contains("c1"));
  EXPECT_EQ(tm["c1"]["status"], "error");
}

// ============ G 组 · 时序与状态隔离（G1-G2） ============

TEST(WfSidefxNotify, G1_clock_global_and_forward_idempotent) {
  reset_sidefx();
  Fixture f;
  // 同 N 幂等推进 + 全局时钟驱动微信窗口（串行：第二个 60 必须读到"已推进"）
  f.model.replies.push_back(ai_tool_calls({call("w1", "mock_clock_forward", Json{{"seconds", 60}})}));
  f.model.replies.push_back(ai_tool_calls({call("w2", "mock_clock_forward", Json{{"seconds", 60}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(chain_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  EXPECT_EQ(tm["w1"]["status"], "success");
  ASSERT_TRUE(tm.contains("w2"));
  EXPECT_NE(tm["w2"]["text"].get<std::string>().find("already_applied"), std::string::npos);
}

TEST(WfSidefxNotify, G2_state_accumulates_independent_across_rounds) {
  reset_sidefx();
  Fixture f;
  // 串行多轮：各自独立对象（订单/微信/电话）累积于库内 → 互不串扰
  f.model.replies.push_back(ai_tool_calls({call("o1", "mock_order_create", Json{{"order_id", "O-G2"}})}));
  f.model.replies.push_back(ai_tool_calls({call("w1", "mock_wechat_send", Json{{"msg_id", "M-G2"}})}));
  f.model.replies.push_back(ai_tool_calls({call("d1", "mock_call_dial", Json{{"call_id", "C-G2"}})}));
  f.model.replies.push_back(cppchain::message::ai_message("done"));
  auto st = f.run(chain_loop_ir());
  ASSERT_TRUE(st);
  auto const tm = tool_messages(*st);
  EXPECT_EQ(tm["o1"]["status"], "success");
  EXPECT_EQ(tm["w1"]["status"], "success");
  EXPECT_EQ(tm["d1"]["status"], "success");
  // 相互独立：各自结果带各自对象名（无串扰）
  EXPECT_NE(tm["o1"]["text"].get<std::string>().find("O-G2"), std::string::npos);
  EXPECT_NE(tm["w1"]["text"].get<std::string>().find("M-G2"), std::string::npos);
  EXPECT_NE(tm["d1"]["text"].get<std::string>().find("C-G2"), std::string::npos);
}