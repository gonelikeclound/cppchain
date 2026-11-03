// SAMOCK —— SA 专属 mock 共享库（SystemAgent 专题，problems/64）。
// 约定（用户 Round 2 拍板）：**所有给 SA 专门 mock 的代码都放这个独立 so 里**，
// 与 wfr 的 sidefx_notify.so（tests/plugin_demo/）同地同居同类纪律。
//
// S1 范围（world-only）：
//   · 世界模型 N2：内存 World（calls / messages / side_effects 只追加台账）+ 三诚实度开关；
//   · 虚拟时钟 N1-v1：**工具推进**的模拟钟（samock_clock_forward，确定性从 0 起步）；
//   · 能力 mock v0：make_call / send_message 最小形态；
//   · 场景控制：world_reset / world_state（唯一"查世界"入口）。
//
// S2 范围（本版新增，C7 假电话）：
//   · **平台侧电话 FSM**（07 §6.3 剧本驱动 + 派生推进，无后台线程）：每次拨号=一个 Session，
//     状态由 `(dialed_at + 剧本)` 在查询时 fold 出来（纯派生、可回放、逐字节确定）；
//   · **里程碑事件**：dialing / ringing / rejected / answered / ended —— 相位跳变时**原子发一次**（幂等），
//     事件时间用**精确时刻**（拨号点+偏移），不是查询时刻；samock_milestones 按 seq 游标读增量（观测归约）；
//   · **barge_in（控制平面注入）**：`samock_barge_in` 直灌用户改口（**仅通话中有效**，对应 C7 双平面）→
//     平台挂断并发 `ended{reason=user_redirected}`。
//
// 核心纪律（07 R4）：**断言只查世界，不查返回值** —— world_state 永远说真话（即使说谎模式），
// 会"说谎"的是能力工具（返回成功但世界没变）。samock 必须能说谎，否则测不出结果真实性。
//
// 纯 C descriptor + 裸函数指针（跨 DSO 安全，同 tool_plugin.hpp 契约）；不链 cppchain；
// 库进程期持有不 dlclose → static 世界在进程期常驻，宿主用 world_reset 隔离。
// 时域：虚拟钟从 0 起步（只依赖 g_w.clock_offset_s）—— 全程确定性，无系统时钟依赖。
#include "cppchain/tool/tool_plugin.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

// ---- 电话平台 FSM（S2）：一次拨号 = 一个 Session ----
// 相位：Dialing → Ringing → Talking（接通）| Failed（拒接等终态）。
// Session 的推进=纯派生：phase = fold(剧本, dialed_at, now)，无后台线程。
enum class Phase : int { Dialing, Ringing, Talking, Failed };

// 里程碑事件（SA/电话域的"全部感官"，04 §6 词汇表）：dialing|ringing|rejected|answered|ended。
// at_sec=精确事件时刻（拨号点+剧本偏移）；ended 携带 reason/disposition。
struct Milestone {
  std::int64_t at_sec;
  int attempt;
  std::string kind;
  std::string reason;       // ended：user_redirected 等；其余空
  std::string disposition;  // ended：redirect_to_messaging；其余空
};

struct Session {
  int n;                 // 第几次拨号
  std::int64_t dialed_at;
  std::int64_t ring_at;    // 相对 dialed_at；剧本固定 3s
  std::int64_t reject_at;  // -1 = 该次不拒接
  std::int64_t answer_at;  // -1 = 该次不接通
  Phase phase = Phase::Dialing;
};

// 通话对象：支持多 Session（重试 = 新 Session），里程碑全局全序（企图维度折叠在 kind/attempt）。
struct Call {
  std::string call_id;
  std::string number;
  std::int64_t started_at_sec = 0;  // 首次拨号
  std::vector<Session> sessions;
  std::vector<Milestone> milestones;
  int next_attempt = 1;
  bool redirect_pending = false;   // 控制平面 barge_in 已请求（C7 双平面）
  std::string redirect_intent;
  bool end_emitted = false;        // ended 只发一次（幂等）
  std::int64_t ended_at_sec = -1;
  std::string ended_reason;        // ended 事件的原因（user_redirected …）
  std::string ended_disposition;
};

// ---- 世界模型（N2，进程期常驻；mutex 守卫防并行读写）----
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

std::string call_state(Call const& c) {
  if (!c.ended_reason.empty()) return "ended";
  if (c.sessions.empty()) return "idle";
  switch (c.sessions.back().phase) {
    case Phase::Dialing: return "dialing";
    case Phase::Ringing: return "ringing";
    case Phase::Talking: return "talking";
    case Phase::Failed: return "failed";
  }
  return "unknown";
}

// 平台侧剧本（S2 定版）：attempt1 → 振铃@3s、拒接@10s（催重试）；attempt2 → 振铃@3s、接通@8s。
// -1 = 该事件不发生。微观：三线平台 FSM 的行为随 attempt 变，业务侧只收事件（01 §4.1）。
struct ScriptSpec {
  std::int64_t ring_at;
  std::int64_t reject_at;
  std::int64_t answer_at;
};
ScriptSpec script_for(int attempt) {
  if (attempt <= 1) return ScriptSpec{3, 10, -1};
  return ScriptSpec{3, -1, 8};
}

// 把通话 fold 到 now：按 (dialed_at + 剧本) 派生各 Session 相位。
// **相位跳变才发里程碑**（一次且仅一次）；事件时间 = 精确时刻（拨号点+偏移），非查询时刻。
void flush_call(Call& c, std::int64_t now) {
  for (Session& s : c.sessions) {
    std::int64_t const off = now - s.dialed_at;
    if (s.phase == Phase::Dialing && off >= s.ring_at) {
      s.phase = Phase::Ringing;
      c.milestones.push_back(Milestone{s.dialed_at + s.ring_at, s.n, "ringing", "", ""});
      if (s.ring_at <= 0) {  // 防御：剧本没有振铃也应推进（本版剧本恒定 3s，纯护栏）
        s.phase = Phase::Talking;
      }
    }
    if (s.phase == Phase::Ringing) {
      if (s.reject_at >= 0 && off >= s.reject_at) {
        s.phase = Phase::Failed;
        c.milestones.push_back(Milestone{s.dialed_at + s.reject_at, s.n, "rejected", "", ""});
      } else if (s.answer_at >= 0 && off >= s.answer_at) {
        s.phase = Phase::Talking;
        c.milestones.push_back(Milestone{s.dialed_at + s.answer_at, s.n, "answered", "", ""});
      }
    }
  }
  // 通话中收到改道（控制平面 barge_in）→ 平台挂断，发 ended{user_redirected}
  if (c.redirect_pending && !c.end_emitted) {
    for (Session const& s : c.sessions) {
      if (s.phase == Phase::Talking) {
        c.ended_at_sec = now;
        c.ended_reason = "user_redirected";
        c.ended_disposition = c.redirect_intent;
        c.milestones.push_back(
            Milestone{now, s.n, "ended", c.ended_reason, c.ended_disposition});
        c.end_emitted = true;
        break;
      }
    }
  }
}

void flush_all_calls() {
  for (auto& [id, c] : g_w.calls) {
    (void)id;
    flush_call(c, now_sec());
  }
}

// 世界快照 → JSON（world_state 的唯一实现；永远说真话）
Json world_snapshot() {
  flush_all_calls();
  Json j;
  j["now_sec"] = g_w.clock_offset_s;
  j["honesty"] = kHonestyNames[static_cast<int>(g_w.honesty)];
  j["seq"] = g_w.seq;
  Json calls = Json::object();
  for (auto const& [id, c] : g_w.calls) {
    Json jc = Json{{"number", c.number}, {"state", call_state(c)},
                   {"started_at_sec", c.started_at_sec},
                   {"attempts", static_cast<std::int64_t>(c.sessions.size())},
                   {"milestones_seq", static_cast<std::int64_t>(c.milestones.size())}};
    if (!c.ended_reason.empty()) {
      jc["ended_reason"] = c.ended_reason;
      jc["ended_disposition"] = c.ended_disposition;
    }
    calls[id] = std::move(jc);
  }
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

// ---- 能力 mock：make_call（S2 = 完整假电话 FSM 的最小版）----
int samock_make_call_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto call_id = a.value("call_id", "");
  auto number = a.value("number", "");
  std::int64_t const attempt = a.value("attempt", 0ll);
  if (call_id.empty()) return fail(err, "samock_make_call: 缺 call_id");
  if (number.empty()) return fail(err, "samock_make_call: 缺 number");
  std::lock_guard<std::mutex> lk(g_mu);

  switch (g_w.honesty) {
    case Honesty::Lying:
      // 说谎：返回成功，但世界没有变（这是 R4 的核心场景——断言查世界必抓）
      return ok(out, Json{{"call_id", call_id}, {"status", "dialing"}, {"number", number}});
    case Honesty::Partial: {
      // 世界变了一半：实体（Session）入水但**漏记台账**（side_effects 无该笔）→ 对账 miss 可测
      auto it = g_w.calls.find(call_id);
      if (it == g_w.calls.end()) {
        Call c;
        c.call_id = call_id;
        c.number = number;
        c.started_at_sec = now_sec();
        it = g_w.calls.emplace(call_id, std::move(c)).first;
      }
      Call& c = it->second;
      int const n = attempt > 0 ? static_cast<int>(attempt) : c.next_attempt;
      c.next_attempt = std::max(c.next_attempt, n + 1);
      ScriptSpec const sp = script_for(n);
      c.sessions.push_back(Session{n, now_sec(), sp.ring_at, sp.reject_at, sp.answer_at, Phase::Dialing});
      c.milestones.push_back(Milestone{now_sec(), n, "dialing", "", ""});
      ++g_w.seq;
      return ok(out, Json{{"call_id", call_id}, {"status", "dialing"}, {"number", number},
                          {"state", "dialing"}, {"attempt", n}});
    }
    case Honesty::Honest: {
      auto it = g_w.calls.find(call_id);
      if (it != g_w.calls.end()) {
        // 幂等/续拨语义（S1 保留）：
        //   · attempt==0（不指定）：同一 call_id 未结束时重拨 → existing（世界不变、seq 不推进）；
        //   · 显式 attempt：该次拨号已存在 → existing；否则=新一轮重试（新 Session）。
        if (!it->second.ended_reason.empty())
          return ok(out, Json{{"call_id", call_id}, {"status", "existing"}, {"state", "ended"}});
        if (attempt == 0)
          return ok(out, Json{{"call_id", call_id}, {"status", "existing"},
                              {"state", call_state(it->second)}});
        for (Session const& s : it->second.sessions)
          if (s.n == static_cast<int>(attempt))
            return ok(out, Json{{"call_id", call_id}, {"status", "existing"},
                                {"state", call_state(it->second)}});
      } else {
        Call c;
        c.call_id = call_id;
        c.number = number;
        c.started_at_sec = now_sec();
        it = g_w.calls.emplace(call_id, std::move(c)).first;
      }
      Call& c = it->second;
      int const n = attempt > 0 ? static_cast<int>(attempt) : c.next_attempt;
      c.next_attempt = std::max(c.next_attempt, n + 1);
      ScriptSpec const sp = script_for(n);
      c.sessions.push_back(Session{n, now_sec(), sp.ring_at, sp.reject_at, sp.answer_at, Phase::Dialing});
      c.milestones.push_back(Milestone{now_sec(), n, "dialing", "", ""});
      g_w.side_effects.push_back(
          SideEffect{now_sec(), "samock_make_call", "拨号 " + number + "（第 " + std::to_string(n) + " 次）"});
      ++g_w.seq;
      return ok(out, Json{{"call_id", call_id}, {"status", "dialing"}, {"number", number},
                          {"state", "dialing"}, {"attempt", n}});
    }
  }
  return fail(err, "samock_make_call: 未知诚实度");
}

// ---- 控制平面：barge_in（C7 双平面）—— 直接发控制事件；仅通话中有效 ----
int samock_barge_in_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto call_id = a.value("call_id", "");
  auto intent = a.value("intent", "");
  if (call_id.empty()) return fail(err, "samock_barge_in: 缺 call_id");
  if (intent.empty()) return fail(err, "samock_barge_in: 缺 intent");
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_w.calls.find(call_id);
  if (it == g_w.calls.end()) return fail(err, "samock_barge_in: 找不到通话 " + call_id);
  Call& c = it->second;
  if (!c.ended_reason.empty())
    return fail(err, "samock_barge_in: 通话已结束 " + call_id);
  bool talking = false;
  for (Session const& s : c.sessions)
    if (s.phase == Phase::Talking) talking = true;
  if (!talking)
    return fail(err, "samock_barge_in: 当前无进行中的通话（barge_in 仅通话中有效）—— 控制平面不得打断未接通状态");
  c.redirect_pending = true;
  c.redirect_intent = intent;
  ++g_w.seq;  // 控制平面动作也是世界变更（审计有据）
  flush_call(c, now_sec());
  return ok(out, Json{{"call_id", call_id}, {"status", "redirected"}, {"disposition", intent},
                      {"ended_reason", c.ended_reason}});
}

// ---- 能力 mock：send_message（S2 保持 v0；不可逆/补偿语义留待后续）----
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

// ---- 里程碑游标读（观测归约：SA/电话域只读声明的里程碑，不读原始音频/转写）----
int samock_milestones_run(char const* args, char** out, char** err) {
  Json const a = parse_args(args);
  auto call_id = a.value("call_id", "");
  std::int64_t since = a.value("since", 0ll);
  if (call_id.empty()) return fail(err, "samock_milestones: 缺 call_id");
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_w.calls.find(call_id);
  if (it == g_w.calls.end()) return fail(err, "samock_milestones: 找不到通话 " + call_id);
  flush_call(it->second, now_sec());  // 读前先 fold 到当前时刻
  std::size_t const from = since < 0 ? 0 : static_cast<std::size_t>(since);
  Json arr = Json::array();
  std::size_t const total = it->second.milestones.size();
  for (std::size_t i = from; i < total; ++i) {
    Milestone const& mi = it->second.milestones[i];
    Json mj = Json{{"seq", static_cast<std::int64_t>(i)}, {"at_sec", mi.at_sec},
                   {"attempt", mi.attempt}, {"kind", mi.kind}};
    if (!mi.reason.empty()) mj["reason"] = mi.reason;
    if (!mi.disposition.empty()) mj["disposition"] = mi.disposition;
    arr.push_back(std::move(mj));
  }
  return ok(out, Json{{"seq", static_cast<std::int64_t>(total)}, {"milestones", std::move(arr)}});
}

// ---- 虚拟时钟（N1-v1）：递进式推进（SA 的钟是连续时间，每次调用累加）----
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

// ---- probe（崩溃缺环兜底）----
int probe_call(char const* args) {
  auto const a = parse_args(args);
  std::string const id = a.value("call_id", "");
  std::lock_guard<std::mutex> lk(g_mu);
  auto const it = g_w.calls.find(id);
  if (it == g_w.calls.end()) return 2;  // Absent
  return 0;                             // Complete（对象在即视为发生过）
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
                  "samock：拨打电话（S2 假电话 FSM：每次拨号=一个 Session，Dialing→Ringing→Talking|Failed；attempt 指定第几次拨号，剧本=attempt1 拒接 / attempt2 接通）",
                  R"({"type":"object","properties":{"call_id":{"type":"string"},"number":{"type":"string"},"attempt":{"type":"integer","description":"第几次拨号（0=自动，1 起显式）"}},"required":["call_id","number"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, samock_make_call_run,
                  probe_call);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_samock_barge_in_creator(void*) {
  return desc_for("samock_barge_in",
                  "samock：控制平面注入用户改口（C7 双平面）——**仅通话中有效**；平台收到后挂断并发 ended{reason=user_redirected}",
                  R"({"type":"object","properties":{"call_id":{"type":"string"},"intent":{"type":"string"}},"required":["call_id","intent"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, samock_barge_in_run,
                  probe_always_absent);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_samock_send_message_creator(void*) {
  return desc_for("samock_send_message",
                  "samock：发送消息（v0 最小形态；honest=入水+送达 / lying=返回已送达但世界无该消息 / partial=在途未送达 delivered=false）",
                  R"({"type":"object","properties":{"msg_id":{"type":"string"},"to":{"type":"string"},"text":{"type":"string"}},"required":["msg_id","to"]})",
                  /*read_only=*/0, kToolEffectIdempotent, kToolApprovalNever, samock_send_message_run,
                  probe_message);
}
extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_samock_milestones_creator(void*) {
  return desc_for("samock_milestones",
                  "samock：读通话的里程碑事件增量（观测归约——SA 只看得见声明的词汇表，不读原始转写）。since=上次读到的 seq，返回 [since,∞) 的里程碑与最新 seq",
                  R"({"type":"object","properties":{"call_id":{"type":"string"},"since":{"type":"integer","description":"上次读到的 seq（0 起）"}},"required":["call_id"]})",
                  /*read_only=*/1, kToolEffectRead, kToolApprovalNever, samock_milestones_run,
                  probe_always_absent);
}