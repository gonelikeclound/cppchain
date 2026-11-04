// demo: sad_mock —— SystemAgent Daemon（SAD）S10a 离线 mock
//
// 1000 条 mock 事件 → L0-a（结构/噪声/去重/限流）→ L0-b（确定性语义替身）
// → L1a（规则）→ L1b（按 DAG 合并 + 打扰预算）→ 预编排 DAG 执行。
// 另含不可丢白名单、精确匹配 bypass、影子审计（注入漏报）、预算静默摘要。
//
// 运行：./run-demo.sh sad_mock    （或 build/dev/examples/sad_mock）
// 产物：tmp/sad_mock_report.json（默认；--report=<path> 可覆盖）
#include "sad_mock.hpp"

#include <fstream>
#include <print>
#include <string>

using cppchain::sad::ScenarioResult;

int main(int argc, char** argv) {
  std::string report_path = "tmp/sad_mock_report.json";
  for (int i = 1; i < argc; ++i) {
    std::string const a = argv[i];
    if (a.starts_with("--report=")) {
      report_path = a.substr(std::string("--report=").size());
    } else if (a == "--help" || a == "-h") {
      std::println("用法：sad_mock [--report=<json 路径>]");
      return 0;
    }
  }

  std::println("SystemAgent Daemon（SAD）S10a 离线 mock\n");
  std::println("事件 → L0-a 结构/噪声/去重/限流 → L0-b 语义替身 → L1a 规则 → L1b 预算 → 预编排 DAG\n");

  ScenarioResult const s = cppchain::sad::run_scenario();
  auto const& r = s.r;

  std::println("总事件 {} · 候选 {} · 唤醒 {}（安全 {}）· 预算静默 {} · 合并唤醒 {}",
               r.total, r.candidates, r.wakeups, r.safety_wakeups, r.budget_exhausted,
               r.coalesced_wakes);
  std::println("丢弃：schema {} · 未知来源 {} · 噪声 {} · 去重 {} · 限流 {}",
               r.dropped_schema, r.dropped_unknown_source, r.dropped_noise, r.dropped_dedup,
               r.dropped_rate);
  std::println("L0-b：通过 {} · 拒绝 {} · 白名单 {} · waits bypass {} · no-rule {}",
               r.l0b_passed, r.l0b_rejected, r.whitelist_bypass, r.exact_wait_bypass, r.no_rule);
  std::println("影子审计：抽样 {} · 漏报 {} · miss_rate {:.3f}%\n", r.shadow_sampled,
               r.shadow_miss, r.miss_rate * 100);

  for (auto const& c : s.checks) std::println("  [{}] {}", c.ok ? "✓" : "✗", c.what);

  std::println("\nDAG 执行：");
  for (auto const& run : r.runs) {
    std::print("  · {:<20} ← {}  [", run.dag, run.event_id);
    for (std::size_t i = 0; i < run.steps.size(); ++i)
      std::print("{}{}", i == 0 ? "" : " → ", run.steps[i]);
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
