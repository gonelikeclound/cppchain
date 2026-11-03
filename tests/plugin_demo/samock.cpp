// SAMOCK —— SA 专属 mock 共享库（SystemAgent 专题，problems/64）。
// 约定（用户 Round 2 拍板）：**所有给 SA 专门 mock 的代码都放这个独立 so 里**，
// 与 wfr 的 sidefx_notify.so（tests/plugin_demo/）同地同居同类纪律。
//
// S1 范围（world-only，problems/64 §5.3）：
//   · 世界模型 N2：内存 World（calls / messages / side_effects 只追加台账）+ 三诚实度开关；
//   · 虚拟时钟 N1-v1：**工具推进**的模拟钟（samock_clock_forward，先例 mock_clock_forward）；
//   · 能力 mock v0：make_call / send_message 的两个最小形态（S2 再上完整假电话 FSM / barge_in）；
//   · 场景控制：world_reset（跨用例库内 static 隔离）、world_state（唯一"查世界"入口）。
//
// 核心纪律（07 R4）：**断言只查世界，不查返回值** —— world_state 永远说真话（即使说谎模式），
// 会"说谎"的是能力工具（返回成功但世界没变）。samock 必须能说谎，否则测不出结果真实性。
//
// 纯 C descriptor + 裸函数指针（跨 DSO 安全，同 tool_plugin.hpp 契约）；不链 cppchain；
// 库进程期持有不 dlclose → static 世界在进程期常驻，宿主用 world_reset 隔离。
// 时域：虚拟钟从 0 起步（只依赖 g_clock_offset_s）—— 全程确定性，无系统时钟依赖。
#include "cppchain/tool/tool_plugin.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Json = nlohmann::json;

namespace {

// ---- 世界三诚实度（07 §6.2）----
enum class Honesty : int { Honest = 0, Lying = 1, Partial = 2 };
constexpr char const* kHonestyNames[] = {"honest", "lying", "partial"};

// ---- 世界模型（N2，进程期常驻；mutex 守卫防并行读写）----
struct Call {
  std::string number;
  std::string state;  // S1 只有 dialing；S2 展开 Dialing→Ringing→Connected→Ended|Failed
  std::int64_t started_at_sec;
};
struct Msg {
  std::string id;
  std::string to;
  std::string text;
  std::int64_t sent_at_sec;
  bool delivered;  // partial 模式下 false（在途未送达 = 世界变了一半）
};
struct SideEffect {
  std::int64_t at_sec;
  std::string tool;
  std::string effect;
};
struct World {
  std::int64_t clock_offset_s = 0;  // 虚拟钟（确定性，起步 0）
  std::int64_t seq = 0;             // 世界变更单调序号（对账/审计用；幂等命中不 +1）
  Honesty honesty = Honesty::Honest;
  std::unordered_map<std::string, Call> calls;
  std::vector<Msg> messages;
  std::vector<SideEffect> side_effects;  // 只追加台账
};
World g_w;
std::mutex g_mu;

std::int64_t now_sec() { return g_w.clock_offset_s; }

// 世界快照 → JSON（world_state 的唯一实现；永远说真话）
Json world_snapshot() {
  Json j;
  j["now_sec"] = g_w.clock_offset_s;
  j["honesty"] = kHonestyNames[static_cast<int>(g_w.honesty)];
  j["seq"] = g_w.seq;
  Json calls = Json::object();
  for (auto const& [id, c] : g_w.calls)
    calls[id] = Json{{"number", c.number}, {"state", c.state}, {"started_at_sec", c.started_at_sec}};
  j["calls"] = std::move(calls);
  Json msgs = Json::array();
  for (auto const& m : g_w.messages)
    msgs.push_back(Json{{"id", m.id}, {"to", m.to}, {"text", m.text}, {"sent_at_sec", m.sent_at_sec},
                        {"delivered", m.delivered}});
  j["messages"] = std::move(msgs);
  Json se = Json::array();
  for (auto const& s : g_w.side_effects)
    se.push_back(Json{{"at_sec", s.at_sec}, {"tool", s.tool}, {"effect", s.effect}});
  j["side_effects"] = std::move(se);
  return j;
}

// ---- 通用助手（照 sidefx_notify.cpp 的纪律）----
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

// ---- 能力 mock v0：make_call（S1 最小形态；S2 换完整假电话 FSM + 脚本推进）----
int samock_make_call_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto call_id = a.value("call_id", "");
  auto number = a.value("number", "");
  if (call_id.empty()) return fail(err, "samock_make_call: 缺 call_id");
  if (number.empty()) return fail(err, "samock_make_call: 缺 number");
  std::lock_guard<std::mutex> lk(g_mu);
  switch (g_w.honesty) {
    case Honesty::Honest: {
      auto const it = g_w.calls.find(call_id);
      if (it != g_w.calls.end())
        return ok(out, Json{{"call_id", call_id}, {"status", "existing"}, {"state", it->second.state}});
      g_w.calls[call_id] = Call{number, "dialing", now_sec()};
      g_w.side_effects.push_back(SideEffect{now_sec(), "samock_make_call", "拨号 " + number});
      ++g_w.seq;
      return ok(out, Json{{"call_id", call_id}, {"status", "dialing"}, {"number", number},
                          {"state", "dialing"}});
    }
    case Honesty::Lying:
      // 说谎：返回成功，但世界没有变（这是 R4 的核心场景——断言查世界必抓）
      return ok(out, Json{{"call_id", call_id}, {"status", "dialing"}, {"number", number}});
    case Honesty::Partial: {
      // 世界变了一半：实体进了 calls，但**漏记台账**（side_effects 无该笔）→ 对账 miss 可测
      g_w.calls[call_id] = Call{number, "dialing", now_sec()};
      ++g_w.seq;
      return ok(out, Json{{"call_id", call_id}, {"status", "dialing"}, {"number", number}});
    }
  }
  return fail(err, "samock_make_call: 未知诚实度");
}

// ---- 能力 mock v0：send_message（S1 最小形态；S2 补不可逆/补偿语义）----
int samock_send_message_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto msg_id = a.value("msg_id", "");
  auto to = a.value("to", "");
  auto text = a.value("text", "");
  if (msg_id.empty()) return fail(err, "samock_send_message: 缺 msg_id");
  if (to.empty()) return fail(err, "samock_send_message: 缺 to");
  std::lock_guard<std::mutex> lk(g_mu);
  switch (g_w.honesty) {
    case Honesty::Honest: {
      for (auto const& m : g_w.messages)
        if (m.id == msg_id)
          return ok(out, Json{{"msg_id", msg_id}, {"status", "existing"}});
      g_w.messages.push_back(Msg{msg_id, to, text, now_sec(), /*delivered=*/true});
      g_w.side_effects.push_back(SideEffect{now_sec(), "samock_send_message", "发送消息给 " + to});
      ++g_w.seq;
      return ok(out, Json{{"msg_id", msg_id}, {"status", "sent"}, {"to", to}, {"delivered", true}});
    }
    case Honesty::Lying:
      // 说谎：返回"已送达"，但世界没有这条消息
      return ok(out, Json{{"msg_id", msg_id}, {"status", "sent"}, {"to", to}, {"delivered", true}});
    case Honesty::Partial: {
      // 世界变了一半：消息入水但 delivered=false（在途），且台账漏记（同 make_call 的 partial）
      g_w.messages.push_back(Msg{msg_id, to, text, now_sec(), /*delivered=*/false});
      ++g_w.seq;
      return ok(out, Json{{"msg_id", msg_id}, {"status", "sent"}, {"to", to}, {"delivered", false}});
    }
  }
  return fail(err, "samock_send_message: 未知诚实度");
}

// ---- 虚拟时钟（N1-v1）：递进式推进（与 sidefx 的"同 N 只推一次"不同：SA 的钟是连续时间）。----
int samock_clock_forward_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  std::int64_t const n = a.value("seconds", 0ll);
  if (n <= 0) return fail(err, "samock_clock_forward: seconds 须为正整数");
  std::lock_guard<std::mutex> lk(g_mu);
  g_w.clock_offset_s += n;
  ++g_w.seq;
  return ok(out, Json{{"seconds", n}, {"now_sec", g_w.clock_offset_s}});
}

// ---- 诚实度开关（R4 取证用：必须能说谎）----
int samock_set_honesty_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  std::string const mode = a.value("mode", "");
  if (mode != "honest" && mode != "lying" && mode != "partial")
    return fail(err, "samock_set_honesty: mode 须为 honest|lying|partial");
  std::lock_guard<std::mutex> lk(g_mu);
  g_w.honesty = mode == "honest" ? Honesty::Honest : mode == "lying" ? Honesty::Lying
                                                                      : Honesty::Partial;
  ++g_w.seq;
  return ok(out, Json{{"honesty", mode}});
}

// ---- 世界重置：跨用例隔离（库内 static 常驻必需）----
int samock_world_reset_run(char const*, char** out, char**) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_w = World{};
  return ok(out, Json{{"reset", true}, {"now_sec", 0}});
}

// ---- 只读观测：世界快照（断言唯一入口，永远说真话）----
int samock_world_state_run(char const*, char** out, char**) {
  std::lock_guard<std::mutex> lk(g_mu);
  return ok(out, world_snapshot());
}

// ---- probe（崩溃缺环兜底；S1 能力工具的产物判定）----
int probe_call(char const* args) {
  auto const a = parse_args(args);
  std::string const id = a.value("call_id", "");
  std::lock_guard<std::mutex> lk(g_mu);
  auto const it = g_w.calls.find(id);
  if (it == g_w.calls.end()) return 2;  // Absent
  return 0;                             // Complete
}
int probe_message(char const* args) {
  auto const a = parse_args(args);
  std::string const id = a.value("msg_id", "");
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto const& m : g_w.messages)
    if (m.id == id) return m.delivered ? 0 : 1;  // Complete / Partial
  return 2;                                      // Absent
}
int probe_always_absent(char const*) { return 2; }

// ---- descriptor 工厂（照 sidefx_notify.cpp：进程期 static 稳定指针，禁止 thread_local）----
cppchain_tool_descriptor const* desc_for(char const* name, char const* desc, char const* params,
                                          int read_only, int effect, int approval,
                                          ToolRunFn handler, ToolProbeFn probe = nullptr) {
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

// ---- 导出：tool_<name>_creator（tool_plugin.hpp 契约）----
// 全部 write 工具 approval=Never：samock 是测试基建，不需要审批摩擦（07 R2 机制测试直灌）。

extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_samock_world_state_creator(void*) {
  return desc_for("samock_world_state",
                  "samock：序列化整个世界（calls/messages/side_effects/虚拟钟/诚实度）——**断言唯一入口**，永远说真话",
                  R"({"type":"object","properties":{}})",
                  /*read_only=*/1, kToolEffectRead, kToolApprovalNever, samock_world_state_run,
                  probe_always_absent);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_samock_clock_forward_creator(void*) {
  return desc_for("samock_clock_forward",
                  "samock：推进虚拟时钟 N 秒（测试时序控制件；SA 的钟是连续时间，每次调用累加）",
                  R"({"type":"object","properties":{"seconds":{"type":"integer","description":"推进的秒数，须为正"}},"required":["seconds"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, samock_clock_forward_run,
                  probe_always_absent);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_samock_set_honesty_creator(void*) {
  return desc_for("samock_set_honesty",
                  "samock：设置世界诚实度（honest 返回成功⇔世界真变 / lying 返回成功但世界没变 / partial 世界变一半）——mock 必须能说谎（07 R4）",
                  R"({"type":"object","properties":{"mode":{"type":"string","enum":["honest","lying","partial"]}},"required":["mode"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, samock_set_honesty_run,
                  probe_always_absent);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_samock_world_reset_creator(void*) {
  return desc_for("samock_world_reset",
                  "samock：清空整个世界 + 虚拟钟 + 诚实度（跨用例隔离控制件）",
                  R"({"type":"object","properties":{}})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, samock_world_reset_run,
                  probe_always_absent);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_samock_make_call_creator(void*) {
  return desc_for("samock_make_call",
                  "samock：拨打电话（v0 仅建 dialing 通话；诚实度语义：honest=真建+记台账 / lying=返回成功但世界无变化 / partial=实体入水但漏记台账）",
                  R"({"type":"object","properties":{"call_id":{"type":"string"},"number":{"type":"string"}},"required":["call_id","number"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, samock_make_call_run,
                  probe_call);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_samock_send_message_creator(void*) {
  return desc_for("samock_send_message",
                  "samock：发送消息（v0 最小形态；honest=入水+送达 / lying=返回已送达但世界无该消息 / partial=在途未送达 delivered=false）",
                  R"({"type":"object","properties":{"msg_id":{"type":"string"},"to":{"type":"string"},"text":{"type":"string"}},"required":["msg_id","to"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, samock_send_message_run,
                  probe_message);
}