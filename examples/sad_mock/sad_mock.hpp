// examples/sad_mock/sad_mock.hpp
// SystemAgent Daemon（SAD）S10 mock —— 三层：SAD 粗循环 / Task Manager / Task Agent。
//
//   SAD（粗 DAG，固定）：事件 → 理解（LLM gate）→ 任务规格 → 提交 → 监控 → 收尾
//   Task Manager：创建/去重/执行/状态（任务生命周期）
//   Task Agent：每个任务独立执行一个预编排 runbook（后续可接任务内规划）
//
// 纪律：
//   · SAD 只处理"需要全局协调"的事件；本地事件直达 Task Manager。
//   · DAG 全部预编排（静态注册表），运行时只选/绑定参数，不生成图。
//   · 理解层用真机 LLM（见 sad_understand.hpp）；测试注入 scripted_understand。
//   · 每道门/每次丢弃/每个任务都可解释、可计数。
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cppchain::sad {

using Json = nlohmann::json;

struct Event {
  std::string id;
  std::string source;
  std::string type;
  std::string severity;  // info|warn|crit
  long t_sec = 0;
  std::string dedup_key;
  std::vector<std::string> correlation_keys;
  std::string scope;
  std::string actionability;
  Json payload = Json::object();
  bool shadow_probe = false;
};

struct GateConfig {
  std::set<std::string> known_sources{"notification", "battery", "calendar", "security",
                                      "health",  "download", "screen",   "app",
                                      "network", "media",    "sensor",   "system",
                                      "payment", "device",   "noise"};
  std::set<std::string> noise_types{"sensor.motion", "sensor.raw"};
  std::set<std::string> whitelist_prefixes{"security.", "health.", "payment."};
  std::set<std::string> active_waits{"download.done"};
  long window_sec = 60;
  long rate_limit = 5;
  long sad_analysis_budget = 5;
  double shadow_sample_rate = 0.25;  // 演示放大；生产 0.5% 由预算反推
};

struct Drop {
  std::string event_id;
  std::string gate;
  std::string rule_id;
  int version = 1;
  std::string reason;
};

struct DigestItem {
  std::string event_id;
  std::string type;
  std::string reason;
};

struct DagStep {
  std::string tool;
  Json args = Json::object();
  Json output = Json::object();
};

struct DagDef {
  std::string name;
  std::string domain;  // ops | device
  std::vector<DagStep> steps;
};

struct DagRun {
  std::string dag;
  std::string event_id;
  std::vector<DagStep> steps;
  bool ok = true;
};

struct EventDisposition {
  std::string id, source, type, severity;
  std::string layer;     // global|local|dropped
  std::string decision;  // sad_task|direct_task|digest|inbox|dropped|escalated
  std::string reason;
  std::string task_id;
};

struct Task {
  std::string id, parent_event, template_id, state;  // completed|failed
  Json spec = Json::object();
  std::vector<DagStep> outputs;
  int merged = 1;
};

struct SadStep {
  std::string node;
  std::string state;
  std::string detail;
};

struct SadTrace {
  std::string event_id, layer, decision, task_id, model;
  int attempts = 0;
  Json spec = Json::object();
  std::vector<SadStep> steps;
};

struct Report {
  long total = 0;
  long dropped_schema = 0;
  long dropped_unknown_source = 0;
  long dropped_noise = 0;
  long dropped_dedup = 0;
  long dropped_rate = 0;
  long candidates = 0;
  long l0b_passed = 0;
  long l0b_rejected = 0;
  long whitelist_bypass = 0;
  long exact_wait_bypass = 0;
  long global_events = 0;
  long local_events = 0;
  long sad_analyses = 0;
  long tasks_created = 0;
  long tasks_merged = 0;
  long llm_calls = 0;
  long shadow_sampled = 0;
  long shadow_miss = 0;
  double miss_rate = 0.0;
  std::vector<Drop> drops;
  std::vector<DigestItem> digest;
  std::vector<EventDisposition> events;
  std::vector<Task> tasks;
  std::vector<SadTrace> sad_traces;
};

struct Check {
  bool ok = false;
  std::string what;
};

struct ScenarioResult {
  Report r;
  std::vector<Check> checks;
  bool ok = true;
};

// ---- 预编排任务模板（Task Agent 的 runbook；运行时只选不生成）----
inline std::map<std::string, DagDef> const& dag_registry() {
  static std::map<std::string, DagDef> const kDags = [] {
    std::map<std::string, DagDef> m;
    auto add = [&](std::string name, std::string domain, std::vector<DagStep> steps) {
      DagDef def;
      def.name = name;
      def.domain = std::move(domain);
      def.steps = std::move(steps);
      m[name] = std::move(def);
    };
    add("reply_and_remind", "comm",
        {DagStep{"read_message", Json{{"app", "wechat"}, {"from", "妈妈"}},
                 Json{{"topic", "明天来吃饭吗"}}},
         DagStep{"draft_reply", Json{{"to", "妈妈"}, {"text", "好的，我明天过来。"}},
                 Json{{"draft", true}}},
         DagStep{"send_message", Json{{"to", "妈妈"}, {"text", "好的，我明天过来。"}},
                 Json{{"delivered", true}}},
         DagStep{"check_calendar", Json{{"date", "tomorrow"}}, Json{{"conflict", false}}},
         DagStep{"create_reminder", Json{{"text", "明天去妈妈家吃饭"}},
                 Json{{"reminder_id", "r1"}}}});
    add("power_saving", "device",
        {DagStep{"check_battery", Json::object(), Json{{"battery", 15}, {"charging", false}}},
         DagStep{"enable_power_saver", Json::object(), Json{{"power_saver", true}}},
         DagStep{"set_brightness", Json{{"level", 20}}, Json{{"brightness", 20}}},
         DagStep{"send_notification", Json{{"text", "已开启省电模式"}},
                 Json{{"delivered", true}}}});
    add("leave_now", "commute",
        {DagStep{"check_route", Json{{"from", "current"}, {"to", "office"}},
                 Json{{"travel_minutes", 40}}},
         DagStep{"notify_leave", Json{{"text", "现在出发还来得及"}},
                 Json{{"delivered", true}}},
         DagStep{"set_alarm", Json{{"time", "now+5m"}}, Json{{"alarm", true}}}});
    add("security_lockdown", "security",
        {DagStep{"lock_device", Json::object(), Json{{"locked", true}}},
         DagStep{"revoke_sessions", Json::object(), Json{{"revoked", 3}}},
         DagStep{"send_notification", Json{{"text", "检测到异常登录，已锁定设备"}},
                 Json{{"delivered", true}}}});
    add("health_alert", "health",
        {DagStep{"check_health_context", Json::object(), Json{{"kind", "heart_rate"}}},
         DagStep{"call_contact", Json{{"contact", "妈妈"}}, Json{{"calling", true}}},
         DagStep{"send_alert", Json{{"text", "检测到健康异常，已联系紧急联系人"}},
                 Json{{"delivered", true}}}});
    add("download_notify", "system",
        {DagStep{"check_download", Json{{"file", "report.pdf"}},
                 Json{{"file", "report.pdf"}, {"size_mb", 12}}},
         DagStep{"send_notification", Json{{"text", "report.pdf 下载完成"}},
                 Json{{"delivered", true}}}});
    add("focus_mode", "device",
        {DagStep{"set_volume", Json{{"level", 2}}, Json{{"volume", 2}}},
         DagStep{"set_brightness", Json{{"level", 30}}, Json{{"brightness", 30}}},
         DagStep{"enable_dnd", Json{{"until", "07:00"}}, Json{{"dnd", true}}},
         DagStep{"screen_off", Json::object(), Json{{"screen", "off"}}},
         DagStep{"send_notification", Json{{"text", "已进入专注模式"}},
                 Json{{"delivered", true}}}});
    return m;
  }();
  return kDags;
}

inline std::set<std::string> template_names() {
  std::set<std::string> out;
  for (auto const& [name, _] : dag_registry()) out.insert(name);
  return out;
}

inline bool is_whitelisted(std::string const& type, GateConfig const& cfg) {
  for (std::string const& p : cfg.whitelist_prefixes)
    if (type.starts_with(p)) return true;
  return false;
}

struct Classified {
  bool relevant = false;
  std::string reason;
  double confidence = 0.0;
};

// L0-b 的确定性替身（生产换小模型/分类器，禁用主模型）。
inline Classified mock_classify(Event const& e) {
  if (e.type == "notification.message")
    return {e.payload.value("intent", "") == "question" ||
                e.payload.value("from_is_contact", false),
            "notification message from contact/question", 0.90};
  if (e.type == "notification.batch") return {false, "batch noise, no action", 0.60};
  if (e.type == "battery.low") return {true, "battery.low", 0.92};
  if (e.type == "calendar.upcoming") return {true, "calendar.upcoming", 0.88};
  if (e.type == "security.alert") return {true, "security.alert", 0.99};
  if (e.type == "health.alert") return {true, "health.alert", 0.99};
  if (e.type == "download.done") return {true, "download.done", 0.95};
  if (e.type == "media.play") return {true, "media.play", 0.55};
  if (e.type == "payment.notification") return {true, "payment.notification", 0.90};
  if (e.type == "device.context")
    return {e.payload.value("mode", "") == "sleep", "device.context mode=sleep", 0.88};
  return {false, "no semantic match", 0.10};
}

struct RuleHit {
  std::string id;
  std::string dag;  // 空 = 只进 inbox
  bool auto_action = true;
};

// 本地事件的规则路由（全局事件由 SAD 理解层决定模板）。
inline std::optional<RuleHit> match_rule(Event const& e) {
  if (e.type == "battery.low") return RuleHit{"battery-low", "power_saving", true};
  if (e.type == "download.done") return RuleHit{"download-done", "download_notify", true};
  if (e.type == "device.context" && e.payload.value("mode", "") == "sleep")
    return RuleHit{"device-sleep", "focus_mode", true};
  if (e.type == "notification.batch") return RuleHit{"notification-batch", "", false};
  if (e.type == "calendar.upcoming") return RuleHit{"calendar-notice", "", false};
  if (e.type == "notification.message") return RuleHit{"message-notice", "", false};
  return std::nullopt;
}

inline DagRun run_dag(std::string const& dag, Event const& e) {
  DagRun run;
  run.dag = dag;
  run.event_id = e.id;
  auto const it = dag_registry().find(dag);
  if (it != dag_registry().end()) {
    for (DagStep const& base : it->second.steps) {
      DagStep step = base;
      for (auto& [k, v] : step.args.items())
        if (e.payload.contains(k)) v = e.payload[k];
      run.steps.push_back(std::move(step));
    }
  }
  run.ok = !run.steps.empty();
  return run;
}

// 哪些事件需要 SAD 做全局协调（其余直达 Task Manager）。
inline std::string classify_layer(Event const& e) {
  if (e.type == "notification.message" && e.payload.value("intent", "") == "question")
    return "global";
  if (e.type == "battery.low" && e.payload.value("commuting", false)) return "global";
  if (e.type == "calendar.upcoming" &&
      e.payload.value("travel_minutes", 0) > e.payload.value("free_minutes", 999))
    return "global";
  if (e.type == "security.alert" && e.severity == "crit") return "global";
  if (e.type == "health.alert" && e.severity == "crit") return "global";
  return "local";
}

// ---- 任务规格校验（理解层的输出门槛）----
inline std::vector<std::string> validate_task_spec(Json const& s) {
  std::vector<std::string> issues;
  if (!s.is_object()) {
    issues.push_back("task spec 必须是 JSON 对象");
    return issues;
  }
  if (!s.contains("goal") || !s["goal"].is_string() || s["goal"].get<std::string>().empty())
    issues.push_back("缺 goal");
  std::string const tmpl = s.value("template", std::string());
  if (tmpl.empty() || template_names().count(tmpl) == 0)
    issues.push_back("template 不在模板清单：" + tmpl);
  std::string const prio = s.value("priority", std::string());
  if (prio != "P0" && prio != "P1" && prio != "P2" && prio != "P3")
    issues.push_back("priority 必须是 P0..P3");
  if (s.value("dedup_key", std::string()).empty()) issues.push_back("缺 dedup_key");
  if (s.contains("scope") && !s["scope"].is_array()) issues.push_back("scope 必须是数组");
  if (s.contains("acceptance") && !s["acceptance"].is_array())
    issues.push_back("acceptance 必须是数组");
  return issues;
}

inline std::string task_template_catalog_prompt() {
  std::string out;
  for (auto const& [name, def] : dag_registry()) {
    out += "- " + name + "（" + def.domain + "）：";
    for (std::size_t i = 0; i < def.steps.size(); ++i)
      out += (i == 0 ? "" : " → ") + def.steps[i].tool;
    out += "\n";
  }
  return out;
}

inline Json local_task_spec(Event const& e, RuleHit const& r) {
  return Json{{"goal", "处理 " + e.type},
              {"template", r.dag},
              {"scope", Json::array({e.source})},
              {"domains", Json::array({r.dag == "focus_mode" ? "device" : "ops"})},
              {"priority", e.severity == "crit" ? "P1" : "P2"},
              {"budget", Json{{"tokens", 20000}, {"minutes", 15}, {"wakeups", 3}}},
              {"acceptance", Json::array({"任务完成并验证"})},
              {"constraints", Json::array()},
              {"dedup_key", e.dedup_key.empty() ? e.id : e.dedup_key}};
}

// ---- SAD 粗 DAG（固定，供 UI 渲染）----
inline Json sad_dag_json() {
  return Json{
      {"nodes", Json::array({Json{{"id", "evt"}, {"label", "事件"}},
                             Json{{"id", "understand"}, {"label", "理解（LLM）"}},
                             Json{{"id", "spec"}, {"label", "任务规格"}},
                             Json{{"id", "submit"}, {"label", "提交任务"}},
                             Json{{"id", "monitor"}, {"label", "监控"}},
                             Json{{"id", "close"}, {"label", "收尾"}}})},
      {"edges", Json::array({Json{{"from", "evt"}, {"to", "understand"}, {"kind", "flow"}},
                             Json{{"from", "understand"}, {"to", "spec"}, {"kind", "flow"}},
                             Json{{"from", "spec"}, {"to", "submit"}, {"kind", "flow"}},
                             Json{{"from", "submit"}, {"to", "monitor"}, {"kind", "flow"}},
                             Json{{"from", "monitor"}, {"to", "close"}, {"kind", "flow"}},
                             Json{{"from", "monitor"}, {"to", "understand"},
                                  {"kind", "loop"}}})}};
}

// ---- Task Manager：任务生命周期（创建/去重/执行/状态）----
class TaskManager {
 public:
  std::vector<Task> tasks;
  std::map<std::string, std::string> by_dedup;

  std::string submit(Json const& spec, Event const& e) {
    std::string const key = spec.value("dedup_key", e.dedup_key);
    auto const it = by_dedup.find(key);
    if (it != by_dedup.end()) {
      if (Task* t = find(it->second)) ++t->merged;
      return it->second;
    }
    Task t;
    t.id = "task-" + std::to_string(tasks.size() + 1);
    t.parent_event = e.id;
    t.template_id = spec.value("template", "");
    t.spec = spec;
    DagRun const run = run_dag(t.template_id, e);
    t.outputs = run.steps;
    t.state = run.ok ? "completed" : "failed";
    tasks.push_back(std::move(t));
    by_dedup[key] = tasks.back().id;
    return tasks.back().id;
  }

  [[nodiscard]] Task* find(std::string const& id) {
    for (Task& t : tasks)
      if (t.id == id) return &t;
    return nullptr;
  }
};

// 理解层：输入全局事件，输出任务规格（JSON）。live 实现见 sad_understand.hpp。
using UnderstandFn =
    std::function<std::optional<Json>(Event const&, std::string& error, int& attempts)>;

// 离线测试替身（CI 不碰真模型）：按事件类型映射模板。
inline UnderstandFn scripted_understand() {
  return [](Event const& e, std::string& error, int& attempts) -> std::optional<Json> {
    attempts = 1;
    std::string tmpl;
    if (e.type == "notification.message") tmpl = "reply_and_remind";
    else if (e.type == "battery.low") tmpl = "power_saving";
    else if (e.type == "calendar.upcoming") tmpl = "leave_now";
    else if (e.type == "security.alert") tmpl = "security_lockdown";
    else if (e.type == "health.alert") tmpl = "health_alert";
    else if (e.type == "download.done") tmpl = "download_notify";
    else {
      error = "no scripted understanding for " + e.type;
      return std::nullopt;
    }
    return Json{{"goal", "处理 " + e.type},
                {"template", tmpl},
                {"scope", Json::array({e.source})},
                {"domains", Json::array({tmpl == "focus_mode" ? "device" : "ops"})},
                {"priority", e.severity == "crit" ? "P1" : "P2"},
                {"budget", Json{{"tokens", 20000}, {"minutes", 15}, {"wakeups", 3}}},
                {"acceptance", Json::array({"任务完成并验证"})},
                {"constraints", Json::array()},
                {"dedup_key", e.dedup_key.empty() ? e.id : e.dedup_key}};
  };
}

struct HandleResult {
  bool ok = false;
  std::string layer, decision, reason, task_id, error;
  Json spec = Json::object();
  std::vector<SadStep> steps;
  int attempts = 0;
};

inline HandleResult sad_handle(Event const& e, TaskManager& tm,
                               UnderstandFn const& understand) {
  HandleResult h;
  h.layer = classify_layer(e);
  auto step = [&](std::string node, std::string state, std::string detail = {}) {
    h.steps.push_back(SadStep{std::move(node), std::move(state), std::move(detail)});
  };
  step("event", "done", e.id);

  if (h.layer == "global") {
    std::string err;
    int attempts = 0;
    auto const spec = understand(e, err, attempts);
    h.attempts = attempts;
    if (!spec) {
      h.decision = "digest";
      h.reason = "understand_failed: " + err;
      step("understand", "failed", err);
      step("close", "done");
      return h;
    }
    auto const issues = validate_task_spec(*spec);
    if (!issues.empty()) {
      h.decision = "digest";
      h.reason = "invalid_task_spec: " + issues.front();
      step("understand", "done", "未通过任务规格校验");
      step("close", "done");
      return h;
    }
    h.spec = *spec;
    step("understand", "done", "attempts=" + std::to_string(attempts));
    step("spec", "done", spec->value("template", ""));
    h.task_id = tm.submit(*spec, e);
    step("submit", "done", h.task_id);
    if (Task const* t = tm.find(h.task_id))
      step("monitor", "done", t->state);
    step("close", "done");
    h.decision = "sad_task";
    h.ok = true;
    return h;
  }

  // local：规则直达 Task Manager，不经 SAD 理解层。
  auto const rule = match_rule(e);
  if (!rule) {
    h.decision = "digest";
    h.reason = "no_rule";
    step("decision", "done", "digest");
    step("close", "done");
    return h;
  }
  if (!rule->auto_action) {
    h.decision = "inbox";
    h.reason = "rule_notify_only: " + rule->id;
    step("decision", "done", "inbox");
    step("close", "done");
    return h;
  }
  h.spec = local_task_spec(e, *rule);
  h.task_id = tm.submit(h.spec, e);
  step("direct_submit", "done", h.task_id);
  if (Task const* t = tm.find(h.task_id)) step("monitor", "done", t->state);
  step("close", "done");
  h.decision = "direct_task";
  h.ok = true;
  return h;
}

// ---- mock 事件源：984 条 ----
inline std::vector<Event> make_events() {
  std::vector<Event> ev;
  auto add = [&](std::string id, std::string src, std::string type, std::string sev, long t,
                 std::string dedup, Json payload = Json::object()) {
    Event e;
    e.id = std::move(id);
    e.source = std::move(src);
    e.type = std::move(type);
    e.severity = std::move(sev);
    e.t_sec = t;
    e.dedup_key = std::move(dedup);
    e.payload = std::move(payload);
    ev.push_back(std::move(e));
  };

  // 900 条传感器噪声（源侧应聚合；这里直接演示被 L0-a 丢弃）
  for (int i = 0; i < 900; ++i)
    add("motion-" + std::to_string(i), "sensor", "sensor.motion", "info", i * 30,
        "motion:" + std::to_string(i), Json{{"axis", i}});

  // 50 条亮屏（速率限制专项：同 source|type 5/60s）
  for (int i = 0; i < 50; ++i)
    add("screen-" + std::to_string(i), "screen", "screen.on", "info", i,
        "screen:" + std::to_string(i), Json{{"on", true}});

  // 20 条同 dedup_key 的群消息（去重/合并专项；规则只进 inbox）
  for (int i = 0; i < 20; ++i)
    add("group-msg-" + std::to_string(i), "notification", "notification.batch", "info",
        100 + i, "notif:group:alumni", Json{{"app", "wechat"}, {"group", "校友群"}});

  // 5 条 schema 非法
  for (int i = 0; i < 5; ++i) {
    Event e;
    e.source = "notification";
    e.severity = "warn";
    e.t_sec = 300 + i;
    e.id = (i == 0 ? "" : "bad-" + std::to_string(i));
    e.type = (i == 1 ? "" : "notification.batch");
    ev.push_back(std::move(e));
  }

  // 全局事件（需要跨 app / 用户上下文协调 → SAD）
  add("msg-mom-1", "notification", "notification.message", "warn", 2000, "msg:mom:1",
      Json{{"app", "wechat"}, {"from", "妈妈"}, {"from_is_contact", true},
           {"intent", "question"}, {"text", "明天来吃饭吗？"}});
  add("battery-commute-1", "battery", "battery.low", "warn", 2100, "battery:commute",
      Json{{"battery", 15}, {"charging", false}, {"commuting", true}});
  add("msg-friend-1", "notification", "notification.message", "info", 2050, "msg:friend:1",
      Json{{"app", "wechat"}, {"from", "李雷"}, {"from_is_contact", true},
           {"intent", "info"}, {"text", "在吗"}});
  add("meeting-soon-1", "calendar", "calendar.upcoming", "warn", 2200, "cal:weekly",
      Json{{"title", "周会"}, {"minutes_until", 10}, {"travel_minutes", 25},
           {"free_minutes", 5}});
  add("security-1", "security", "security.alert", "crit", 2300, "security:login",
      Json{{"kind", "unusual_login"}});
  add("health-1", "health", "health.alert", "crit", 2350, "health:hr",
      Json{{"kind", "heart_rate"}, {"value", 150}});

  // 本地事件（直达 Task Manager / inbox）
  add("download-1", "download", "download.done", "info", 2400, "download:report",
      Json{{"file", "report.pdf"}});
  add("battery-local-1", "battery", "battery.low", "info", 2450, "battery:local",
      Json{{"battery", 18}, {"charging", false}, {"commuting", false}});
  add("media-1", "media", "media.play", "info", 2500, "media:play", Json{{"app", "music"}});

  // 注入漏报（强制影子审计）：legacy 来源 + 低电+归途（本应全局）
  {
    Event e;
    e.id = "shadow-miss-1";
    e.source = "legacy";
    e.type = "battery.low";
    e.severity = "warn";
    e.t_sec = 2700;
    e.dedup_key = "battery:home:low";
    e.payload = Json{{"battery", 15}, {"charging", false}, {"commuting", true}};
    e.shadow_probe = true;
    ev.push_back(std::move(e));
  }
  return ev;
}

inline ScenarioResult run_scenario(GateConfig const& cfg, UnderstandFn const& understand) {
  ScenarioResult out;
  Report& r = out.r;
  TaskManager tm;
  std::vector<Event> const events = make_events();
  r.total = static_cast<long>(events.size());

  std::vector<EventDisposition> disps(events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    disps[i].id = events[i].id;
    disps[i].source = events[i].source;
    disps[i].type = events[i].type;
    disps[i].severity = events[i].severity;
    disps[i].decision = "pending";
  }

  std::map<std::string, long> last_seen;
  std::map<std::string, std::vector<long>> rate_hits;
  long drop_count = 0;
  auto add_drop = [&](std::size_t idx, Event const& e, std::string gate, std::string rule,
                      std::string reason) {
    r.drops.push_back(Drop{e.id, gate, rule, 1, reason});
    disps[idx].layer = "dropped";
    disps[idx].decision = "dropped";
    disps[idx].reason = reason;
    ++drop_count;
  };
  auto shadow_check = [&](Event const& e) {
    bool const sampled = e.shadow_probe || (drop_count % 4 == 0);
    if (!sampled) return;
    ++r.shadow_sampled;
    if (mock_classify(e).relevant) ++r.shadow_miss;
  };

  struct Candidate {
    std::size_t idx;
    Event e;
  };
  std::vector<Candidate> candidates;

  for (std::size_t idx = 0; idx < events.size(); ++idx) {
    Event const& e = events[idx];
    if (e.id.empty() || e.source.empty() || e.type.empty() || e.severity.empty()) {
      ++r.dropped_schema;
      add_drop(idx, e, "L0-a", "schema/v1", "missing required field");
      shadow_check(e);
      continue;
    }
    if (is_whitelisted(e.type, cfg)) {
      ++r.whitelist_bypass;
      r.digest.push_back(DigestItem{e.id, e.type, "whitelist"});
      if (e.severity == "crit") {
        HandleResult const h = sad_handle(e, tm, understand);
        ++r.global_events;
        ++r.sad_analyses;
        r.llm_calls += h.attempts;
        r.sad_traces.push_back(SadTrace{e.id, h.layer, h.decision, h.task_id, "llm",
                                        h.attempts, h.spec, h.steps});
        disps[idx].layer = h.layer;
        disps[idx].decision = h.decision;
        disps[idx].reason = h.reason.empty() ? "whitelist_crit" : h.reason;
        disps[idx].task_id = h.task_id;
      } else {
        disps[idx].layer = "local";
        disps[idx].decision = "digest";
        disps[idx].reason = "whitelist";
      }
      continue;
    }
    if (cfg.known_sources.count(e.source) == 0) {
      ++r.dropped_unknown_source;
      add_drop(idx, e, "L0-a", "source/v1", "unknown source: " + e.source);
      shadow_check(e);
      continue;
    }
    if (cfg.noise_types.count(e.type) != 0) {
      ++r.dropped_noise;
      add_drop(idx, e, "L0-a", "noise/v1", "noise type: " + e.type);
      shadow_check(e);
      continue;
    }
    if (cfg.active_waits.count(e.type) != 0) {
      ++r.exact_wait_bypass;
      candidates.push_back(Candidate{idx, e});
      continue;
    }
    auto const it = last_seen.find(e.dedup_key);
    if (it != last_seen.end() && e.t_sec - it->second < cfg.window_sec) {
      ++r.dropped_dedup;
      add_drop(idx, e, "L0-a", "dedup/v1", "dedup window: " + e.dedup_key);
      shadow_check(e);
      continue;
    }
    last_seen[e.dedup_key] = e.t_sec;
    auto& hits = rate_hits[e.source + "|" + e.type];
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [&](long t) { return e.t_sec - t >= cfg.window_sec; }),
               hits.end());
    if (static_cast<long>(hits.size()) >= cfg.rate_limit) {
      ++r.dropped_rate;
      add_drop(idx, e, "L0-a", "rate/v1", "rate limit: " + e.source + "|" + e.type);
      shadow_check(e);
      continue;
    }
    hits.push_back(e.t_sec);
    candidates.push_back(Candidate{idx, e});
  }

  r.candidates = static_cast<long>(candidates.size());

  for (Candidate const& c0 : candidates) {
    Event const& e = c0.e;
    bool const bypass = cfg.active_waits.count(e.type) != 0;
    if (!bypass) {
      Classified const c = mock_classify(e);
      if (!c.relevant) {
        ++r.l0b_rejected;
        r.digest.push_back(DigestItem{e.id, e.type, "l0b_not_relevant: " + c.reason});
        disps[c0.idx].layer = "local";
        disps[c0.idx].decision = "digest";
        disps[c0.idx].reason = "l0b_not_relevant: " + c.reason;
        continue;
      }
    }
    ++r.l0b_passed;

    std::string const layer = classify_layer(e);
    if (layer == "global") {
      if (r.sad_analyses >= cfg.sad_analysis_budget) {
        ++r.global_events;
        r.digest.push_back(DigestItem{e.id, e.type, "sad_budget_exhausted"});
        disps[c0.idx].layer = "global";
        disps[c0.idx].decision = "escalated";
        disps[c0.idx].reason = "sad_budget_exhausted";
        continue;
      }
      HandleResult const h = sad_handle(e, tm, understand);
      ++r.global_events;
      ++r.sad_analyses;
      r.llm_calls += h.attempts;
      r.sad_traces.push_back(SadTrace{e.id, h.layer, h.decision, h.task_id, "llm",
                                      h.attempts, h.spec, h.steps});
      disps[c0.idx].layer = h.layer;
      disps[c0.idx].decision = h.decision;
      disps[c0.idx].reason = h.reason.empty() ? "sad_task" : h.reason;
      disps[c0.idx].task_id = h.task_id;
    } else {
      HandleResult const h = sad_handle(e, tm, understand);
      ++r.local_events;
      r.sad_traces.push_back(SadTrace{e.id, h.layer, h.decision, h.task_id, "",
                                      0, h.spec, h.steps});
      disps[c0.idx].layer = h.layer;
      disps[c0.idx].decision = h.decision;
      disps[c0.idx].reason = h.reason.empty() ? "direct_task" : h.reason;
      disps[c0.idx].task_id = h.task_id;
    }
  }

  r.tasks = tm.tasks;
  r.tasks_created = static_cast<long>(tm.tasks.size());
  for (Task const& t : tm.tasks) r.tasks_merged += t.merged - 1;
  r.miss_rate =
      r.shadow_sampled == 0 ? 0.0 : static_cast<double>(r.shadow_miss) / r.shadow_sampled;

  auto check = [&](bool ok, std::string what) {
    out.checks.push_back(Check{ok, std::move(what)});
    out.ok = out.ok && ok;
  };
  long whitelist_digest = 0;
  for (DigestItem const& d : r.digest)
    if (d.reason == "whitelist") ++whitelist_digest;

  check(r.global_events >= 5, "全局事件进入 SAD（" + std::to_string(r.global_events) + "）");
  check(r.local_events >= 4, "本地事件直达 Task Manager（" + std::to_string(r.local_events) + "）");
  check(static_cast<long>(r.sad_traces.size()) == r.global_events + r.local_events,
        "每个候选事件都有处理轨迹");
  bool global_ok = true;
  for (SadTrace const& t : r.sad_traces) {
    if (t.layer != "global") continue;
    if (t.task_id.empty() || t.spec.empty()) global_ok = false;
    bool has_understand = false, has_submit = false;
    for (SadStep const& s : t.steps) {
      if (s.node == "understand") has_understand = true;
      if (s.node == "submit") has_submit = true;
    }
    if (!has_understand || !has_submit) global_ok = false;
  }
  check(global_ok, "全局事件全部完成 理解→任务规格→提交");
  bool local_no_llm = true;
  for (SadTrace const& t : r.sad_traces) {
    if (t.layer != "local") continue;
    for (SadStep const& s : t.steps)
      if (s.node == "understand") local_no_llm = false;
  }
  check(local_no_llm, "本地事件不经过 SAD 理解层");
  bool tasks_ok = r.tasks_created > 0;
  for (Task const& t : r.tasks) {
    if (t.state == "completed" && t.outputs.empty()) tasks_ok = false;
  }
  check(tasks_ok, "Task Manager 创建并执行任务（" + std::to_string(r.tasks_created) + "）");
  check(whitelist_digest == r.whitelist_bypass,
        "白名单事件 100% 进摘要层（" + std::to_string(whitelist_digest) + "）");
  bool all_drops_explained = true;
  for (Drop const& d : r.drops)
    if (d.reason.empty() || d.rule_id.empty()) all_drops_explained = false;
  check(all_drops_explained, "每次丢弃都有 rule_id + reason");
  check(r.shadow_miss >= 1, "影子审计发现注入漏报（" + std::to_string(r.shadow_miss) + "）");
  check(r.miss_rate <= 0.005,
        "miss_rate ≤ 0.5%（实际 " + std::to_string(r.miss_rate * 100) + "%）");
  check(r.dropped_noise >= 900, "噪声被 L0-a 过滤（" + std::to_string(r.dropped_noise) + "）");
  check(r.dropped_dedup >= 19, "重复事件被合并（" + std::to_string(r.dropped_dedup) + "）");

  r.events = std::move(disps);
  return out;
}

inline ScenarioResult run_scenario(GateConfig const& cfg = {}) {
  return run_scenario(cfg, scripted_understand());
}

// ---- 单事件模拟（UI）----
struct SimResult {
  bool ok = false;
  std::string layer, decision, reason, task_id;
  Event event;
  Json spec = Json::object();
  std::vector<SadStep> steps;
  std::vector<DagStep> outputs;
  int attempts = 0;
};

inline SimResult simulate_one(Event e, UnderstandFn const& understand,
                              GateConfig const& cfg = {}) {
  SimResult s;
  s.event = std::move(e);
  auto done = [&](std::string layer, std::string decision, std::string reason) {
    s.ok = true;
    s.layer = std::move(layer);
    s.decision = std::move(decision);
    s.reason = std::move(reason);
    return s;
  };
  Event const& ev = s.event;
  if (ev.id.empty() || ev.source.empty() || ev.type.empty() || ev.severity.empty())
    return done("dropped", "dropped", "schema missing required field");
  if (is_whitelisted(ev.type, cfg)) {
    if (ev.severity != "crit") return done("local", "digest", "whitelist");
  } else {
    if (cfg.known_sources.count(ev.source) == 0)
      return done("dropped", "dropped", "unknown source: " + ev.source);
    if (cfg.noise_types.count(ev.type) != 0)
      return done("dropped", "dropped", "noise type: " + ev.type);
    bool const bypass = cfg.active_waits.count(ev.type) != 0;
    if (!bypass) {
      Classified const c = mock_classify(ev);
      if (!c.relevant) return done("local", "digest", "l0b_not_relevant: " + c.reason);
    }
  }

  TaskManager tm;
  HandleResult const h = sad_handle(ev, tm, understand);
  s.ok = true;
  s.layer = h.layer;
  s.decision = h.decision;
  s.reason = h.reason;
  s.task_id = h.task_id;
  s.spec = h.spec;
  s.steps = h.steps;
  s.attempts = h.attempts;
  if (Task const* t = tm.find(h.task_id)) s.outputs = t->outputs;
  return s;
}

inline SimResult simulate_one(Event e, GateConfig const& cfg = {}) {
  return simulate_one(std::move(e), scripted_understand(), cfg);
}

inline Json dags_json() {
  Json arr = Json::array();
  for (auto const& [name, def] : dag_registry()) {
    Json steps = Json::array();
    for (DagStep const& st : def.steps)
      steps.push_back(Json{{"tool", st.tool}, {"args", st.args}, {"output", st.output}});
    arr.push_back(Json{{"name", def.name}, {"domain", def.domain}, {"steps", steps}});
  }
  return arr;
}

inline Json steps_json(std::vector<DagStep> const& steps) {
  Json out = Json::array();
  for (DagStep const& st : steps)
    out.push_back(Json{{"tool", st.tool}, {"args", st.args}, {"output", st.output}});
  return out;
}

inline Json tasks_json(std::vector<Task> const& tasks) {
  Json arr = Json::array();
  for (Task const& t : tasks)
    arr.push_back(Json{{"id", t.id},
                       {"parent_event", t.parent_event},
                       {"template", t.template_id},
                       {"state", t.state},
                       {"merged", t.merged},
                       {"spec", t.spec},
                       {"outputs", steps_json(t.outputs)}});
  return arr;
}

inline Json traces_json(std::vector<SadTrace> const& traces) {
  Json arr = Json::array();
  for (SadTrace const& t : traces) {
    Json steps = Json::array();
    for (SadStep const& s : t.steps)
      steps.push_back(Json{{"node", s.node}, {"state", s.state}, {"detail", s.detail}});
    arr.push_back(Json{{"event", t.event_id},
                       {"layer", t.layer},
                       {"decision", t.decision},
                       {"task_id", t.task_id},
                       {"model", t.model},
                       {"attempts", t.attempts},
                       {"spec", t.spec},
                       {"steps", steps}});
  }
  return arr;
}

inline Json sim_result_json(SimResult const& s) {
  return Json{{"ok", s.ok},
              {"layer", s.layer},
              {"decision", s.decision},
              {"reason", s.reason},
              {"task_id", s.task_id},
              {"event",
               Json{{"id", s.event.id},
                    {"source", s.event.source},
                    {"type", s.event.type},
                    {"severity", s.event.severity},
                    {"payload", s.event.payload}}},
              {"spec", s.spec},
              {"trace_steps", [&] {
                 Json a = Json::array();
                 for (SadStep const& st : s.steps)
                   a.push_back(Json{{"node", st.node}, {"state", st.state},
                                    {"detail", st.detail}});
                 return a;
               }()},
              {"outputs", steps_json(s.outputs)},
              {"sad_dag", sad_dag_json()},
              {"dags", dags_json()}};
}

inline Json report_json(ScenarioResult const& s) {
  Report const& r = s.r;
  Json checks = Json::array();
  for (Check const& c : s.checks) checks.push_back(Json{{"ok", c.ok}, {"what", c.what}});
  Json drops = Json::array();
  for (Drop const& d : r.drops)
    drops.push_back(Json{{"event", d.event_id},
                         {"gate", d.gate},
                         {"rule_id", d.rule_id},
                         {"version", d.version},
                         {"reason", d.reason}});
  Json digest = Json::array();
  for (DigestItem const& d : r.digest)
    digest.push_back(Json{{"event", d.event_id}, {"type", d.type}, {"reason", d.reason}});
  Json events = Json::array();
  for (EventDisposition const& e : r.events)
    events.push_back(Json{{"id", e.id},
                          {"source", e.source},
                          {"type", e.type},
                          {"severity", e.severity},
                          {"layer", e.layer},
                          {"decision", e.decision},
                          {"reason", e.reason},
                          {"task_id", e.task_id}});
  auto const now_s = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return Json{{"schema", "sad-mock-report/v2"},
              {"ok", s.ok},
              {"generated_at", now_s},
              {"metrics",
               Json{{"total", r.total},
                    {"dropped_schema", r.dropped_schema},
                    {"dropped_unknown_source", r.dropped_unknown_source},
                    {"dropped_noise", r.dropped_noise},
                    {"dropped_dedup", r.dropped_dedup},
                    {"dropped_rate", r.dropped_rate},
                    {"candidates", r.candidates},
                    {"l0b_passed", r.l0b_passed},
                    {"l0b_rejected", r.l0b_rejected},
                    {"whitelist_bypass", r.whitelist_bypass},
                    {"exact_wait_bypass", r.exact_wait_bypass},
                    {"global_events", r.global_events},
                    {"local_events", r.local_events},
                    {"sad_analyses", r.sad_analyses},
                    {"tasks_created", r.tasks_created},
                    {"tasks_merged", r.tasks_merged},
                    {"llm_calls", r.llm_calls},
                    {"shadow_sampled", r.shadow_sampled},
                    {"shadow_miss", r.shadow_miss},
                    {"miss_rate", r.miss_rate}}},
              {"checks", checks},
              {"sad_dag", sad_dag_json()},
              {"dags", dags_json()},
              {"events", events},
              {"tasks", tasks_json(r.tasks)},
              {"sad_traces", traces_json(r.sad_traces)},
              {"drops", drops},
              {"digest", digest}};
}

}  // namespace cppchain::sad
