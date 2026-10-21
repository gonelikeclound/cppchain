// wfr-㊹ 治理（problems/34）· 副作用场景 mock 库（libsidefx_notify.so）。
// 一组**可逆性梯度**工具，专门测试副作用治理各场景：
//   订单  = Idempotent。同 order_id 二次创建返回 existing（幂等防重发）。
//   微信  = 一定时间内可撤回（send 记 sent_at；revoke 判 age < 窗口；出窗 → error"已过撤回窗口"）。
//   短信  = **不可撤回**：只注册 send，无任何反向工具（语义由工具集保证）。
//   电话  = 任何状态可关闭：ring(=接通前取消，未发生副作用) / answered(=接通后关闭，已发生通话)。
// 时序由**模拟时钟**控制（mock_clock_forward 拨快、幂等台账），离线复现窗口过期不 sleep。
//
// 纯 C descriptor + 裸函数指针（跨 DSO 安全），不链 cppchain；库进程期持有不 dlclose →
// static 状态表在进程期常驻（测试用 mock_state_reset 清零；真实 world 由宿主决定）。
// 并发纪律：handler 可被并行调用、probe 可能与其并发读 → 状态表统一 std::mutex 守卫。
#include "cppchain/tool/tool_plugin.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

using Json = nlohmann::json;

namespace {

constexpr int64_t kWechatRevokeWindowSec = 120;  // 微信撤回窗口（模拟钟秒）

// ---- 库内状态表（进程期常驻；mutex 守卫防并行读写） ----
std::mutex g_mu;
std::unordered_map<std::string, int64_t> g_orders;    // order_id -> created_at(s)
struct Wechat { int64_t sent_at_s; std::string to, text; bool revoked = false; };
std::unordered_map<std::string, Wechat> g_wechat;      // msg_id -> ...
std::unordered_map<std::string, int64_t> g_sms;        // sms_id -> sent_at(s)（无反向工具）
struct Call { int64_t called_at_s; std::string status; };  // ring | answered | hungup
std::unordered_map<std::string, Call> g_calls;         // call_id -> ...
int64_t g_clock_offset_s = 0;                          // 模拟钟偏置（system_clock + offset）
std::unordered_map<int64_t, bool> g_clock_applied;     // clock_forward 幂等台账（distinct N）

// 当前模拟钟（全部工具共用的 now）
int64_t now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() + g_clock_offset_s;
}

// ---- 通用助手：JSON 字符串 → 对象 ----
Json parse_args(char const* s) {
  if (s == nullptr) return Json::object();
  auto j = Json::parse(s, nullptr, /*allow_exceptions=*/false);
  return j.is_discarded() ? Json::object() : j;
}

int ok(char** out, Json const& j) {
  std::string const s = j.dump();
  *out = ::strdup(s.c_str());
  return 0;
}
int fail(char** err, std::string const& why) {
  *err = ::strdup(why.c_str());
  return 1;
}

// ---- handler ----

// 订单：同 order_id 二次创建 → existing + count=1（幂等：不重复入表）
int mock_order_create_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto order_id = a.value("order_id", "");
  auto item = a.value("item", "");
  if (order_id.empty()) return fail(err, "mock_order_create: 缺 order_id");
  std::lock_guard<std::mutex> lk(g_mu);
  auto const it = g_orders.find(order_id);
  if (it != g_orders.end())
    return ok(out, Json{{"order_id", order_id}, {"status", "existing"},
                        {"count", static_cast<int64_t>(g_orders.size())},
                        {"created_at", it->second}, {"item", item}});
  g_orders[order_id] = now_s();
  return ok(out, Json{{"order_id", order_id}, {"status", "created"}, {"item", item},
                      {"created_at", g_orders[order_id]},
                      {"count", static_cast<int64_t>(g_orders.size())}});
}

// 微信发送：落 outbox（sent_at=模拟钟）
int mock_wechat_send_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto msg_id = a.value("msg_id", "");
  auto to = a.value("to", "");
  auto text = a.value("text", "");
  if (msg_id.empty()) return fail(err, "mock_wechat_send: 缺 msg_id");
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_wechat.count(msg_id))
    return ok(out, Json{{"msg_id", msg_id}, {"status", "existing"},
                        {"sent_at", g_wechat[msg_id].sent_at_s}});
  g_wechat[msg_id] = Wechat{now_s(), to, text};
  return ok(out, Json{{"msg_id", msg_id}, {"status", "sent"}, {"to", to},
                      {"sent_at", g_wechat[msg_id].sent_at_s},
                      {"revoke_window_sec", kWechatRevokeWindowSec}});
}

// 微信撤回：age < 窗口 → revoked；已撤回 → error"已撤回"；出窗 → error
int mock_wechat_revoke_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto msg_id = a.value("msg_id", "");
  if (msg_id.empty()) return fail(err, "mock_wechat_revoke: 缺 msg_id");
  std::lock_guard<std::mutex> lk(g_mu);
  auto const it = g_wechat.find(msg_id);
  if (it == g_wechat.end()) return fail(err, "mock_wechat_revoke: 找不到消息 " + msg_id);
  if (it->second.revoked) return fail(err, "mock_wechat_revoke: 消息已撤回 " + msg_id);
  int64_t const age = now_s() - it->second.sent_at_s;
  if (age >= kWechatRevokeWindowSec)
    return fail(err, "mock_wechat_revoke: 已过撤回窗口（age=" + std::to_string(age) +
                     "s ≥ " + std::to_string(kWechatRevokeWindowSec) + "s），不可撤回");
  it->second.revoked = true;
  return ok(out, Json{{"msg_id", msg_id}, {"status", "revoked"}, {"age_s", age}});
}

// 短信发送：不可撤（无反向工具）；同 sim_id 二次发送 → existing
int mock_sms_send_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto sms_id = a.value("sms_id", "");
  auto phone = a.value("phone", "");
  auto text = a.value("text", "");
  if (sms_id.empty()) return fail(err, "mock_sms_send: 缺 sms_id");
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_sms.count(sms_id))
    return ok(out, Json{{"sms_id", sms_id}, {"status", "existing"}, {"to", phone},
                        {"sent_at", g_sms[sms_id]}});
  g_sms[sms_id] = now_s();
  return ok(out, Json{{"sms_id", sms_id}, {"status", "sent"}, {"to", phone},
                      {"sent_at", g_sms[sms_id]}, {"irreversible", true}});
}

// 电话拨打：建通话到 ring 状态（同 call_id 二次拨 → existing）
int mock_call_dial_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto call_id = a.value("call_id", "");
  auto number = a.value("number", "");
  if (call_id.empty()) return fail(err, "mock_call_dial: 缺 call_id");
  std::lock_guard<std::mutex> lk(g_mu);
  auto const it = g_calls.find(call_id);
  if (it != g_calls.end())
    return ok(out, Json{{"call_id", call_id}, {"status", it->second.status}});
  g_calls[call_id] = Call{now_s(), "ring"};
  return ok(out, Json{{"call_id", call_id}, {"status", "ring"}, {"number", number},
                      {"called_at", g_calls[call_id].called_at_s}});
}

// 模拟接通：ring → answered
int mock_call_connect_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto call_id = a.value("call_id", "");
  if (call_id.empty()) return fail(err, "mock_call_connect: 缺 call_id");
  std::lock_guard<std::mutex> lk(g_mu);
  auto const it = g_calls.find(call_id);
  if (it == g_calls.end()) return fail(err, "mock_call_connect: 找不到通话 " + call_id);
  if (it->second.status == "hungup") return fail(err, "mock_call_connect: 通话已结束 " + call_id);
  if (it->second.status == "answered") return ok(out, Json{{"call_id", call_id}, {"status", "answered"}});
  it->second.status = "answered";
  return ok(out, Json{{"call_id", call_id}, {"status", "answered"}});
}

// 挂断：任何状态可挂 —— ring=接通前取消（未发生副作用）、answered=接通后关闭（已发生通话）
int mock_call_hangup_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto call_id = a.value("call_id", "");
  if (call_id.empty()) return fail(err, "mock_call_hangup: 缺 call_id");
  std::lock_guard<std::mutex> lk(g_mu);
  auto const it = g_calls.find(call_id);
  if (it == g_calls.end()) return fail(err, "mock_call_hangup: 找不到通话 " + call_id);
  if (it->second.status == "hungup") return fail(err, "mock_call_hangup: 通话已结束 " + call_id);
  bool const was_answered = (it->second.status == "answered");
  it->second.status = "hungup";
  return ok(out, Json{{"call_id", call_id}, {"status", "hungup"},
                      {"was_answered", was_answered},
                      {"closed_reason", was_answered ? "接通后关闭" : "接通前取消"}});
}

// 模拟时钟推进：断言的**相对 sent_at 的 age**依赖"同一 now 全库共用"；此处幂等（distinct N 只推一次）
int mock_clock_forward_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  int64_t const n = a.value("seconds", 0ll);
  if (n <= 0) return fail(err, "mock_clock_forward: seconds 须为正整数");
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_clock_applied.count(n))
    return ok(out, Json{{"seconds", n}, {"status", "already_applied"},
                        {"offset_s", g_clock_offset_s}});
  g_clock_applied[n] = true;
  g_clock_offset_s += n;
  return ok(out, Json{{"seconds", n}, {"status", "forwarded"},
                      {"offset_s", g_clock_offset_s}});
}

// 测试专用控制件：清空全部 mock 状态 + 时钟（隔离跨用例库内 static）
int mock_state_reset_run(char const*, char** out, char**) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_orders.clear();
  g_wechat.clear();
  g_sms.clear();
  g_calls.clear();
  g_clock_offset_s = 0;
  g_clock_applied.clear();
  return ok(out, Json{{"reset", true}});
}

// ---- probe：读库内状态表（崩溃缺环时 guard 判定"已发生/未发生"） ----
// 判据 = 对象在库里已存在 → Complete(0)；不存在 → Absent(2)（未发生，可安全重跑）。
// 各工具优先按自己登记的内容判定（wechat 还看 revoked —— 已撤回也视为"发生过"）。
// **故障注入档**（专供 F4/B5 测 probe 四格）：调用参数里 id 前缀为 `?` → Unknown(3)（模拟
// 不可探测的执行状态）；前缀为 `#` → Partial(1)（模拟"产物不完整/写一半"）。正常场景不受影响。
int probe_order(char const* args) {
  auto const a = parse_args(args);
  std::string const id = a.value("order_id", "");
  if (id.starts_with("?")) return 3;
  if (id.starts_with("#")) return 1;
  std::lock_guard<std::mutex> lk(g_mu);
  return g_orders.count(id) ? 0 : 2;
}
int probe_wechat(char const* args) {
  auto const a = parse_args(args);
  std::string const id = a.value("msg_id", "");
  if (id.starts_with("?")) return 3;
  if (id.starts_with("#")) return 1;
  std::lock_guard<std::mutex> lk(g_mu);
  return g_wechat.count(id) ? 0 : 2;
}
int probe_sms(char const* args) {
  auto const a = parse_args(args);
  std::string const id = a.value("sms_id", "");
  if (id.starts_with("?")) return 3;
  if (id.starts_with("#")) return 1;
  std::lock_guard<std::mutex> lk(g_mu);
  return g_sms.count(id) ? 0 : 2;
}
int probe_call(char const* args) {
  auto const a = parse_args(args);
  std::string const id = a.value("call_id", "");
  if (id.starts_with("?")) return 3;
  if (id.starts_with("#")) return 1;
  std::lock_guard<std::mutex> lk(g_mu);
  return g_calls.count(id) ? 0 : 2;
}
int probe_always_absent(char const*) { return 2; }  // clock/state_reset：无持久产物

// ---- descriptor 工厂（仿 mock_tools.cpp）----
// 注意：必须给每个工具**稳定的 descriptor 指针**。不用 thread_local —— 批量 dlsym 时
// pending 阶段逐个调用 creator，thread_local 会让所有 creator 复用同一个 d（最后一次覆写），
// 导致全部工具串成同一 handler。用进程期 static（加载阶段宿主串行 creator()，无并发）。
cppchain_tool_descriptor const* desc_for(char const* name, char const* desc,
                                        char const* params, int read_only, int effect,
                                        int approval, ToolRunFn handler, ToolProbeFn probe = nullptr) {
  static std::unordered_map<std::string, cppchain_tool_descriptor> by_name;
  cppchain_tool_descriptor& d = by_name[std::string(name)];
  d = cppchain_tool_descriptor{};
  d.name = name;
  d.version = kToolApiVersion;
  d.context = nullptr;
  d.description = desc;
  d.parameters_json = params;
  d.read_only = read_only;
  d.effect_class = effect;
  d.approval = approval;
  d.ttl_ms = 0;
  d.handler = handler;
  d.probe = probe;
  return &d;
}

}  // namespace

extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_order_create_creator(void*) {
  return desc_for("mock_order_create", "mock：下订单（幂等——同 order_id 二次创建返回 existing）",
                  R"({"type":"object","properties":{"order_id":{"type":"string"},"item":{"type":"string"},"qty":{"type":"integer"}},"required":["order_id"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalPolicy, mock_order_create_run,
                  probe_order);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_wechat_send_creator(void*) {
  return desc_for("mock_wechat_send", "mock：发送微信（一定时间内可撤回，撤回须调用 mock_wechat_revoke）",
                  R"({"type":"object","properties":{"msg_id":{"type":"string"},"to":{"type":"string"},"text":{"type":"string"}},"required":["msg_id"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalPolicy, mock_wechat_send_run,
                  probe_wechat);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_wechat_revoke_creator(void*) {
  return desc_for("mock_wechat_revoke", "mock：撤回微信（仅撤回窗口内有效，出窗后不可撤回）",
                  R"({"type":"object","properties":{"msg_id":{"type":"string"}},"required":["msg_id"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalPolicy, mock_wechat_revoke_run,
                  probe_wechat);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_sms_send_creator(void*) {
  return desc_for("mock_sms_send", "mock：发送短信（**不可撤回**——无任何反向工具，发出即生效）",
                  R"({"type":"object","properties":{"sms_id":{"type":"string"},"phone":{"type":"string"},"text":{"type":"string"}},"required":["sms_id"]})",
                  /*read_only=*/0, kToolEffectIrreversible, kToolApprovalPolicy, mock_sms_send_run,
                  probe_sms);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_call_dial_creator(void*) {
  return desc_for("mock_call_dial", "mock：拨打电话（含人工审批；接通前可挂断、接通后也可关闭）",
                  R"({"type":"object","properties":{"call_id":{"type":"string"},"number":{"type":"string"}},"required":["call_id"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNeedHuman, mock_call_dial_run,
                  probe_call);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_call_connect_creator(void*) {
  return desc_for("mock_call_connect", "mock：模拟电话接通（ring→answered）",
                  R"({"type":"object","properties":{"call_id":{"type":"string"}},"required":["call_id"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalPolicy, mock_call_connect_run,
                  probe_call);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_call_hangup_creator(void*) {
  return desc_for("mock_call_hangup", "mock：挂断电话（任何状态可挂：接通前=取消，接通后=关闭）",
                  R"({"type":"object","properties":{"call_id":{"type":"string"}},"required":["call_id"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNeedHuman, mock_call_hangup_run,
                  probe_call);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_clock_forward_creator(void*) {
  return desc_for("mock_clock_forward", "mock：推进模拟时钟 N 秒（测试时序控制件，幂等——重复同 N 不推进）",
                  R"({"type":"object","properties":{"seconds":{"type":"integer"}},"required":["seconds"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, mock_clock_forward_run,
                  probe_always_absent);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_mock_state_reset_creator(void*) {
  return desc_for("mock_state_reset", "mock：清空全部 mock 状态与模拟时钟（测试隔离控制件）",
                  R"({"type":"object","properties":{}})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, mock_state_reset_run,
                  probe_always_absent);
}