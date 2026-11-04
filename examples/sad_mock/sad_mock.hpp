// examples/sad_mock/sad_mock.hpp
// SystemAgent Daemon（SAD）S10a 离线 mock —— 事件 → 漏斗 → 规则 → 预编排 DAG。
//
// 纪律：
//   · 纯离线、确定性、无模型调用；L0-b 用确定性 "mock classifier" 代替小模型。
//   · DAG 全部预编排（静态注册表），运行时只做路由/参数绑定 + 执行。
//   · 每道门都有可解释的丢弃记录 {rule_id, version, reason}；白名单/budget/shadow 审计是一等机制。
//   · 只做机制验证：动作本身是 mock step，不产生真实副作用。
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
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
  long t_sec = 0;        // mock 时钟（生产用真实 ts）
  std::string dedup_key;
  std::vector<std::string> correlation_keys;
  std::string scope;
  std::string actionability;  // auto|suggest|notify
  Json payload = Json::object();
  bool shadow_probe = false;  // 测试钩子：强制进入影子审计
};

struct GateConfig {
  std::set<std::string> known_sources{"svc", "disk", "deploy", "security", "human", "noise"};
  std::set<std::string> noise_types{"noise.metric"};
  std::set<std::string> whitelist_prefixes{"security.", "human.", "payment.", "health."};
  std::set<std::string> active_waits{"deploy.finished"};  // 活跃任务等待的精确事件
  long window_sec = 60;
  long rate_limit = 5;  // 同 source|type 每窗口上限
  long daily_wake_budget = 4;
  double shadow_sample_rate = 0.25;  // 演示放大（生产 0.5% 由预算反推）
  bool safety_priority_override = true;
};

struct Drop {
  std::string event_id;
  std::string gate;     // L0-a / L0-b
  std::string rule_id;
  int version = 1;
  std::string reason;
};

struct Wake {
  std::string event_id;
  std::string dag;
  std::string reason;
  bool safety = false;
};

struct DagRun {
  std::string dag;
  std::string event_id;
  std::vector<std::string> steps;
  bool ok = true;
};

struct DigestItem {
  std::string event_id;
  std::string type;
  std::string reason;
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
  long inbox = 0;
  long no_rule = 0;
  long wakeups = 0;
  long safety_wakeups = 0;
  long budget_exhausted = 0;
  long coalesced_wakes = 0;
  long shadow_sampled = 0;
  long shadow_miss = 0;
  double miss_rate = 0.0;
  std::vector<Drop> drops;
  std::vector<Wake> wakes;
  std::vector<DagRun> runs;
  std::vector<DigestItem> digest;
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

// ---- 预编排 DAG 注册表（运行时只按名字路由，不生成图）----
inline std::map<std::string, std::vector<std::string>> const& dag_registry() {
  static std::map<std::string, std::vector<std::string>> const kDags = {
      {"svc_restart", {"check_health", "restart_service", "verify_health"}},
      {"disk_cleanup", {"inspect_usage", "cleanup_temp", "verify_usage"}},
      {"deploy_rollback", {"identify_release", "rollback", "verify_deploy"}},
      {"deploy_verify", {"check_release", "verify_traffic"}},
      {"security_incident", {"isolate_host", "collect_evidence", "notify_security"}},
      {"capacity_scale", {"check_pool", "scale_workers", "verify_capacity"}},
  };
  return kDags;
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

// L0-b 的确定性替身：生产环境换成小模型/分类器（禁用主模型）。
inline Classified mock_classify(Event const& e) {
  if (e.type == "svc.health")
    return {e.payload.value("status", "") == "down", "svc.status=down", 0.92};
  if (e.type == "disk.usage")
    return {e.payload.value("usage_pct", 0) >= 90, "disk.usage>=90", 0.90};
  if (e.type == "deploy.failed") return {true, "deploy.failed", 0.95};
  if (e.type == "deploy.finished") return {true, "deploy.finished(wait)", 0.95};
  if (e.type == "security.alert") return {true, "security.alert", 0.99};
  if (e.type == "human.message") return {true, "human.message", 0.80};
  if (e.type == "capacity.alert") return {true, "capacity.alert", 0.85};
  return {false, "no semantic match", 0.10};
}

struct RuleHit {
  std::string id;
  std::string dag;        // 空 = 只进 inbox，不唤醒
  bool auto_action = true;
};

// L1a：规则命中是纯函数；模型不决定“要不要建任务”。
inline std::optional<RuleHit> match_rule(Event const& e) {
  if (e.type == "svc.health" && e.payload.value("status", "") == "down")
    return RuleHit{"svc-down", "svc_restart", true};
  if (e.type == "disk.usage" && e.payload.value("usage_pct", 0) >= 90)
    return RuleHit{"disk-pressure", "disk_cleanup", true};
  if (e.type == "deploy.failed") return RuleHit{"deploy-failed", "deploy_rollback", true};
  if (e.type == "deploy.finished") return RuleHit{"deploy-wait", "deploy_verify", true};
  if (e.type == "security.alert" && e.severity == "crit")
    return RuleHit{"security-crit", "security_incident", true};
  if (e.type == "capacity.alert") return RuleHit{"capacity-pressure", "capacity_scale", true};
  if (e.type == "human.message") return RuleHit{"human-message", "", false};
  return std::nullopt;
}

inline DagRun run_dag(std::string const& dag, std::string const& event_id) {
  DagRun run;
  run.dag = dag;
  run.event_id = event_id;
  auto const it = dag_registry().find(dag);
  if (it != dag_registry().end()) run.steps = it->second;
  run.ok = !run.steps.empty();
  return run;
}

// ---- 事件源（mock）：1000 条事件，3 个真事件 + 广播噪声 + 专项钩子 ----
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

  // 900 条传感器/指标噪声（L0-a 类型过滤）
  for (int i = 0; i < 900; ++i)
    add("noise-" + std::to_string(i), "noise", "noise.metric", "info", i * 30,
        "noise:" + std::to_string(i), Json{{"value", i}});

  // 50 条健康检查正常（速率限制专项：同 source|type 5/60s）
  for (int i = 0; i < 50; ++i)
    add("health-ok-" + std::to_string(i), "svc", "svc.health", "info", i,
        "health-ok:" + std::to_string(i), Json{{"status", "ok"}});

  // 20 条同 dedup_key 的低水位磁盘事件（去重/合并专项，阈值以下 → L0-b 拒绝）
  for (int i = 0; i < 20; ++i)
    add("disk-dup-" + std::to_string(i), "disk", "disk.usage", "info", 100 + i,
        "disk:/var:low", Json{{"mount", "/var"}, {"usage_pct", 50}});

  // 5 条 schema 非法（缺 id/type）
  for (int i = 0; i < 5; ++i) {
    Event e;
    e.source = "svc";
    e.severity = "warn";
    e.t_sec = 300 + i;
    e.id = (i == 0 ? "" : "bad-" + std::to_string(i));
    e.type = (i == 1 ? "" : "svc.health");
    ev.push_back(std::move(e));
  }

  // 3 个真事件 + 预算/旁路/白名单钩子
  add("svc-down-1", "svc", "svc.health", "crit", 2000, "svc:api:down",
      Json{{"service", "api"}, {"status", "down"}});
  add("disk-full-1", "disk", "disk.usage", "warn", 2100, "disk:/var:full",
      Json{{"mount", "/var"}, {"usage_pct", 92}});
  add("deploy-failed-1", "deploy", "deploy.failed", "crit", 2200, "deploy:rel-42:failed",
      Json{{"release", "rel-42"}});
  add("deploy-finished-1", "deploy", "deploy.finished", "info", 2300, "deploy:rel-42:done",
      Json{{"release", "rel-42"}});
  add("disk-full-2", "disk", "disk.usage", "warn", 2400, "disk:/data:full",
      Json{{"mount", "/data"}, {"usage_pct", 95}});
  add("capacity-1", "svc", "capacity.alert", "warn", 2450, "capacity:workers",
      Json{{"pool", "workers"}, {"utilization_pct", 96}});
  add("security-1", "security", "security.alert", "crit", 2500, "security:intrusion",
      Json{{"kind", "intrusion"}});
  add("human-1", "human", "human.message", "warn", 2600, "human:msg-1",
      Json{{"from", "user"}, {"text", "在吗？"}});

  // 注入的“应关注却被 L0-a 丢弃”事件：source=legacy（未知来源）+ 强制影子审计
  {
    Event e;
    e.id = "shadow-miss-1";
    e.source = "legacy";
    e.type = "disk.usage";
    e.severity = "warn";
    e.t_sec = 2700;
    e.dedup_key = "disk:/home:full";
    e.payload = Json{{"mount", "/home"}, {"usage_pct", 91}};
    e.shadow_probe = true;
    ev.push_back(std::move(e));
  }
  return ev;
}

inline ScenarioResult run_scenario(GateConfig const& cfg = {}) {
  ScenarioResult out;
  Report& r = out.r;
  std::vector<Event> const events = make_events();
  r.total = static_cast<long>(events.size());

  std::map<std::string, long> last_seen;                 // dedup
  std::map<std::string, std::vector<long>> rate_hits;    // source|type → timestamps
  std::vector<Event> candidates;
  long drop_count = 0;

  auto add_drop = [&](Event const& e, std::string gate, std::string rule, std::string reason) {
    r.drops.push_back(Drop{e.id, std::move(gate), std::move(rule), 1, std::move(reason)});
    ++drop_count;
  };
  // 影子审计：按比例抽样 + 强制 shadow_probe；用 L0-b 复核是否“本应关注”。
  auto shadow_check = [&](Event const& e) {
    bool const sampled = e.shadow_probe || (drop_count % 4 == 0);
    if (!sampled) return;
    ++r.shadow_sampled;
    if (mock_classify(e).relevant) ++r.shadow_miss;
  };

  for (Event const& e : events) {
    // schema
    if (e.id.empty() || e.source.empty() || e.type.empty() || e.severity.empty()) {
      ++r.dropped_schema;
      add_drop(e, "L0-a", "schema/v1", "missing required field");
      shadow_check(e);
      continue;
    }

    // 不可丢白名单：独立于订阅，先进摘要层；crit 直接安全唤醒（预算 override）。
    if (is_whitelisted(e.type, cfg)) {
      ++r.whitelist_bypass;
      r.digest.push_back(DigestItem{e.id, e.type, "whitelist"});
      if (e.severity == "crit") {
        ++r.wakeups;
        ++r.safety_wakeups;
        r.wakes.push_back(Wake{e.id, "security_incident", "whitelist_crit", true});
        r.runs.push_back(run_dag("security_incident", e.id));
      }
      continue;
    }

    // 未知来源
    if (cfg.known_sources.count(e.source) == 0) {
      ++r.dropped_unknown_source;
      add_drop(e, "L0-a", "source/v1", "unknown source: " + e.source);
      shadow_check(e);
      continue;
    }

    // 源侧噪声（L0-a 类型过滤）
    if (cfg.noise_types.count(e.type) != 0) {
      ++r.dropped_noise;
      add_drop(e, "L0-a", "noise/v1", "noise type: " + e.type);
      shadow_check(e);
      continue;
    }

    // 精确匹配 bypass：活跃任务等待的事件直接进 L1a（跳过 L0-b）
    if (cfg.active_waits.count(e.type) != 0) {
      ++r.exact_wait_bypass;
      candidates.push_back(e);
      continue;
    }

    // 去重/合并
    auto const it = last_seen.find(e.dedup_key);
    if (it != last_seen.end() && e.t_sec - it->second < cfg.window_sec) {
      ++r.dropped_dedup;
      add_drop(e, "L0-a", "dedup/v1", "dedup window: " + e.dedup_key);
      shadow_check(e);
      continue;
    }
    last_seen[e.dedup_key] = e.t_sec;

    // 限流（令牌桶简化：窗口内计数）
    auto& hits = rate_hits[e.source + "|" + e.type];
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [&](long t) { return e.t_sec - t >= cfg.window_sec; }),
               hits.end());
    if (static_cast<long>(hits.size()) >= cfg.rate_limit) {
      ++r.dropped_rate;
      add_drop(e, "L0-a", "rate/v1", "rate limit: " + e.source + "|" + e.type);
      shadow_check(e);
      continue;
    }
    hits.push_back(e.t_sec);
    candidates.push_back(e);
  }

  r.candidates = static_cast<long>(candidates.size());

  struct WakeCandidate {
    Event e;
    RuleHit rule;
  };
  std::vector<WakeCandidate> wake_candidates;

  // L0-b + L1a
  for (Event const& e : candidates) {
    bool bypass = cfg.active_waits.count(e.type) != 0;
    if (!bypass) {
      Classified const c = mock_classify(e);
      if (!c.relevant) {
        ++r.l0b_rejected;
        r.digest.push_back(DigestItem{e.id, e.type, "l0b_not_relevant: " + c.reason});
        continue;
      }
    }
    ++r.l0b_passed;

    auto const rule = match_rule(e);
    if (!rule) {
      ++r.no_rule;
      ++r.inbox;
      r.digest.push_back(DigestItem{e.id, e.type, "no_rule"});
      continue;
    }
    if (!rule->auto_action) {
      ++r.inbox;
      r.digest.push_back(DigestItem{e.id, e.type, "rule_notify_only: " + rule->id});
      continue;
    }
    wake_candidates.push_back(WakeCandidate{e, *rule});
  }

  // L1b：按 DAG 合并唤醒 + 打扰预算
  std::set<std::string> woken_dags;
  for (WakeCandidate const& wc : wake_candidates) {
    if (woken_dags.count(wc.rule.dag) != 0) {
      ++r.coalesced_wakes;
      ++r.inbox;
      r.digest.push_back(DigestItem{wc.e.id, wc.e.type, "coalesced: " + wc.rule.dag});
      continue;
    }
    // 活跃任务的 waits 事件属于任务续跑，不是新的主动打扰：优先于打扰预算。
    bool const task_continuation = wc.e.type == "deploy.finished";
    if (!task_continuation && r.wakeups >= cfg.daily_wake_budget) {
      ++r.budget_exhausted;
      ++r.inbox;
      r.digest.push_back(DigestItem{wc.e.id, wc.e.type, "budget_exhausted"});
      continue;
    }
    woken_dags.insert(wc.rule.dag);
    ++r.wakeups;
    r.wakes.push_back(Wake{wc.e.id, wc.rule.dag, wc.rule.id, false});
    r.runs.push_back(run_dag(wc.rule.dag, wc.e.id));
  }

  r.miss_rate =
      r.shadow_sampled == 0 ? 0.0 : static_cast<double>(r.shadow_miss) / r.shadow_sampled;

  auto check = [&](bool ok, std::string what) {
    out.checks.push_back(Check{ok, std::move(what)});
    out.ok = out.ok && ok;
  };

  long whitelist_digest = 0;
  for (DigestItem const& d : r.digest)
    if (d.reason == "whitelist") ++whitelist_digest;

  check(r.wakeups <= 5, "wakeups ≤ 5（实际 " + std::to_string(r.wakeups) + "）");
  check(whitelist_digest == r.whitelist_bypass,
        "白名单事件 100% 进摘要层（" + std::to_string(whitelist_digest) + "/" +
            std::to_string(r.whitelist_bypass) + "）");
  bool all_drops_explained = true;
  for (Drop const& d : r.drops)
    if (d.reason.empty() || d.rule_id.empty()) all_drops_explained = false;
  check(all_drops_explained, "每次丢弃都有 rule_id + reason");
  check(r.shadow_miss >= 1, "影子审计发现注入的漏报（shadow_miss=" +
                                std::to_string(r.shadow_miss) + "）");
  check(r.miss_rate <= 0.005,
        "miss_rate ≤ 0.5%（实际 " + std::to_string(r.miss_rate * 100) + "%）");
  check(r.exact_wait_bypass >= 1,
        "精确匹配 bypass 生效（" + std::to_string(r.exact_wait_bypass) + " 条）");
  check(r.budget_exhausted >= 1,
        "打扰预算会触发静默摘要（" + std::to_string(r.budget_exhausted) + " 条）");
  check(r.dropped_noise >= 900, "噪声被 L0-a 过滤（" + std::to_string(r.dropped_noise) + "）");
  check(r.dropped_dedup >= 19, "重复事件被合并（" + std::to_string(r.dropped_dedup) + "）");
  check(r.safety_wakeups >= 1, "安全类事件可越过打扰预算（" + std::to_string(r.safety_wakeups) + "）");

  std::set<std::string> ran;
  for (DagRun const& run : r.runs) ran.insert(run.dag);
  check(ran.count("svc_restart") != 0 && ran.count("disk_cleanup") != 0 &&
            ran.count("deploy_rollback") != 0 && ran.count("deploy_verify") != 0 &&
            ran.count("security_incident") != 0,
        "预编排 DAG 路由到 5 个预期 DAG（实际 " + std::to_string(ran.size()) + " 个）");

  return out;
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
  Json wakes = Json::array();
  for (Wake const& w : r.wakes)
    wakes.push_back(Json{{"event", w.event_id}, {"dag", w.dag}, {"reason", w.reason},
                         {"safety", w.safety}});
  Json runs = Json::array();
  for (DagRun const& run : r.runs)
    runs.push_back(Json{{"dag", run.dag}, {"event", run.event_id}, {"steps", run.steps},
                        {"ok", run.ok}});
  Json digest = Json::array();
  for (DigestItem const& d : r.digest)
    digest.push_back(Json{{"event", d.event_id}, {"type", d.type}, {"reason", d.reason}});

  return Json{{"schema", "sad-mock-report/v1"},
              {"ok", s.ok},
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
                    {"inbox", r.inbox},
                    {"no_rule", r.no_rule},
                    {"wakeups", r.wakeups},
                    {"safety_wakeups", r.safety_wakeups},
                    {"budget_exhausted", r.budget_exhausted},
                    {"coalesced_wakes", r.coalesced_wakes},
                    {"shadow_sampled", r.shadow_sampled},
                    {"shadow_miss", r.shadow_miss},
                    {"miss_rate", r.miss_rate}}},
              {"checks", checks},
              {"drops", drops},
              {"wakes", wakes},
              {"runs", runs},
              {"digest", digest}};
}

}  // namespace cppchain::sad
