// demo: sad_mock —— SystemAgent Daemon（SAD）S10 mock
//
// 三层：SAD 粗循环（事件 → 理解[LLM] → 任务规格 → 提交 → 监控）
//      → Task Manager（创建/去重/执行/状态）
//      → Task Agent（每个任务独立执行预编排 runbook）。
// 事件漏斗（L0-a/L0-b/白名单/bypass/影子审计）在 SAD 之前。
//
// 运行：./run-demo.sh sad_mock    （或 build/dev/examples/sad_mock）
// 产物：tmp/sad_mock_report.json（默认；--report=<path> 可覆盖）
#include "sad_mock.hpp"
#include "sad_understand.hpp"

#include <fstream>
#include <print>
#include <string>

using cppchain::sad::ScenarioResult;

int main(int argc, char** argv) {
  std::string report_path = "tmp/sad_mock_report.json";
  std::string simulate_path;
  bool print_dags = false;
  for (int i = 1; i < argc; ++i) {
    std::string const a = argv[i];
    if (a.starts_with("--report=")) {
      report_path = a.substr(std::string("--report=").size());
    } else if (a.starts_with("--simulate-event=")) {
      simulate_path = a.substr(std::string("--simulate-event=").size());
    } else if (a == "--dags") {
      print_dags = true;
    } else if (a == "--help" || a == "-h") {
      std::println("用法：sad_mock [--report=<json>] | --simulate-event=<json> | --dags");
      return 0;
    }
  }

  if (print_dags) {
    std::println("{}", cppchain::sad::dags_json().dump());
    return 0;
  }

  auto const understand = cppchain::sad::make_live_understand();

  if (!simulate_path.empty()) {
    std::ifstream in(simulate_path);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
      std::println("{}", nlohmann::json{{"ok", false}, {"error", "bad event json"}}.dump());
      return 1;
    }
    cppchain::sad::Event e;
    e.id = j.value("id", "sim-1");
    e.source = j.value("source", std::string());
    e.type = j.value("type", std::string());
    e.severity = j.value("severity", "info");
    e.t_sec = j.value("t_sec", 0L);
    e.dedup_key = j.value("dedup_key", e.id);
    e.scope = j.value("scope", std::string());
    e.actionability = j.value("actionability", "auto");
    e.payload = j.value("payload", nlohmann::json::object());
    e.shadow_probe = j.value("shadow_probe", false);
    auto const s = cppchain::sad::simulate_one(std::move(e), understand);
    std::println("{}", cppchain::sad::sim_result_json(s).dump());
    return s.ok ? 0 : 1;
  }

  std::println("SystemAgent Daemon（SAD）S10 mock\n");
  std::println("SAD 粗循环：事件 → 理解（真机 LLM）→ 任务规格 → Task Manager → Task Agent\n");

  ScenarioResult const s = cppchain::sad::run_scenario({}, understand);
  auto const& r = s.r;

  std::println("总事件 {} · 候选 {} · 全局 {} · 本地 {} · SAD 理解 {} · LLM 调用 {}",
               r.total, r.candidates, r.global_events, r.local_events, r.sad_analyses,
               r.llm_calls);
  std::println("任务：创建 {} · 合并 {} · 丢弃 schema {} · 噪声 {} · 去重 {} · 限流 {}",
               r.tasks_created, r.tasks_merged, r.dropped_schema, r.dropped_noise,
               r.dropped_dedup, r.dropped_rate);
  std::println("L0-b：通过 {} · 拒绝 {} · 白名单 {} · waits bypass {} · 影子 {} / miss {:.3f}%\n",
               r.l0b_passed, r.l0b_rejected, r.whitelist_bypass, r.exact_wait_bypass,
               r.shadow_sampled, r.miss_rate * 100);

  for (auto const& c : s.checks) std::println("  [{}] {}", c.ok ? "✓" : "✗", c.what);

  std::println("\nSAD 轨迹：");
  for (auto const& t : r.sad_traces) {
    std::print("  · {:<10} {:<24} → {}  [", t.layer, t.event_id,
               t.decision + (t.task_id.empty() ? "" : " " + t.task_id));
    for (std::size_t i = 0; i < t.steps.size(); ++i)
      std::print("{}{}", i == 0 ? "" : " → ", t.steps[i].node);
    std::println("]");
  }

  std::println("\nTask Agent 执行：");
  for (auto const& task : r.tasks) {
    std::print("  · {:<8} {:<18} ← {}  [", task.id, task.template_id, task.parent_event);
    for (std::size_t i = 0; i < task.outputs.size(); ++i)
      std::print("{}{}", i == 0 ? "" : " → ", task.outputs[i].tool);
    std::println("]");
  }

  std::ofstream out(report_path);
  if (!out) {
    std::println("\n[✗] 写报告失败：{}", report_path);
    return 1;
  }
  out << cppchain::sad::report_json(s).dump(2) << "\n";

  std::println("\n报告：{}", report_path);
  std::println("验收：{}", s.ok ? "全部通过" : "存在失败");
  return s.ok ? 0 : 1;
}
