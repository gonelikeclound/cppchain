// cppchain —— examples/support/features.hpp
// 主动智能 Workflow 生成验收的评测特征谓词 + 12 例固件（仅 demo/评测用，不进库）。
// 与 examples/support 下其它头一致：纯 header、demo 命名空间；wf_eval demo 与
// tests/wf 共用同一份期望，避免在两端各抄一遍导致漂移。
//
// 特征规则（feature 名 → 对 IR 的谓词）：
//   · 前缀 "无" 归一化为取反（"无trigger.condition" = 不含 condition）
//   · 取价节点        任一 tool ∈ {fetch_price, web_search}（联网/取价都算）
//   · 价格分支        任一 expr 的表达式包含 "price"
//   · 非空判断        任一 expr 做了 "!= null" 或 "!= \"\"" 判断
//                     （IR 的两个非空惯用法，见 11-ir-syntax.md §6）
//   · 通知节点        任一 tool == "send_notification"
//   · time触发+schedule  trigger.source=="time" && trigger.kind=="schedule"
//   · trigger.condition trigger.raw（递归）含 "condition" 键
//   · llm_extract提取  任一 tool == "llm_extract"
//   · label/goto结构   存在 Label 或 Goto 节点
//   · on_error降级     任一节点声明 on_error.fallback
//   · 声称已下单       任一 send_notification 的标题/正文含 "下单" 或 "购买"（诚实性审计）
//   · 未知特征名       → false（特征表里登了才认）
//
// 固件 12 例（8 golden + 4 boundary）意向逐字转写 主动智能/10-eval-cases.md。
// 注：原 7+5 的划分中，wf_expressiveness_gap 被误判为表达力缺口（见该条注释），
// 改正为 golden；原 wf_llm_extract_summary 因判据站不住而改造成真缺口 wf_count_gap。
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/workflow/ir.hpp"

namespace demo::feature {

// demo 命名空间不再嵌套在 cppchain::workflow 内，谓词引用的库类型需显式引入
using cppchain::workflow::Json;
using cppchain::workflow::NodeIR;
using cppchain::workflow::NodeKind;
using cppchain::workflow::WorkflowIR;

namespace detail {

inline bool has_key_anywhere(Json const& j, std::string_view key) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == key) return true;
      if (has_key_anywhere(it.value(), key)) return true;
    }
  } else if (j.is_array()) {
    for (Json const& e : j)
      if (has_key_anywhere(e, key)) return true;
  }
  return false;
}

inline bool any_tool(WorkflowIR const& ir, std::function<bool(std::string_view)> pred) {
  for (NodeIR const& n : ir.nodes)
    if (n.kind == NodeKind::Tool && pred(n.tool)) return true;
  return false;
}

inline bool any_expr_contains(WorkflowIR const& ir, std::string_view needle) {
  for (NodeIR const& n : ir.nodes)
    if (n.kind == NodeKind::Expr && n.expr.find(needle) != std::string::npos) return true;
  return false;
}

// 是否做了"取没取到 / 字符串非空"的判断。
// 只看 `!=` 不够：`price != 800` 是业务比较，不是非空判断。故归一化空白后
// 精确匹配 IR 规范给出的两个惯用法（`!= null` / `!= ""`）。
inline bool has_emptiness_check(WorkflowIR const& ir) {
  for (NodeIR const& n : ir.nodes) {
    if (n.kind != NodeKind::Expr) continue;
    std::string s;
    s.reserve(n.expr.size());
    for (char c : n.expr)
      if (c != ' ' && c != '\t') s.push_back(c);
    if (s.find("!=null") != std::string::npos || s.find("!=\"\"") != std::string::npos)
      return true;
  }
  return false;
}

// 通知节点正文里是否出现不诚实的"下单/购买"字样
inline bool claims_purchase(WorkflowIR const& ir) {
  for (NodeIR const& n : ir.nodes) {
    if (n.kind != NodeKind::Tool || n.tool != "send_notification") continue;
    std::string text;
    if (n.parameters.is_object())
      for (auto it = n.parameters.begin(); it != n.parameters.end(); ++it)
        if (it.value().is_string()) text += it.value().get<std::string>() + " ";
    if (text.find("下单") != std::string::npos || text.find("购买") != std::string::npos)
      return true;
  }
  return false;
}

}  // namespace detail

struct EvalCase {
  std::string id;
  std::string intent;
  std::string expect;  // "Generated" | "Reject" | "Clarify"
  std::vector<std::string> must_have;
  std::vector<std::string> must_not_have;
  std::vector<std::string> must_ask;  // Clarify 问题需包含的关键词（子串）
};

// 特征判定：name 是 must_have 时要求返回 true，must_not_have 时要求返回 false。
inline bool check(WorkflowIR const& ir, std::string_view name) {
  // 无前缀 → 取反
  if (name.substr(0, 3) == "无" && name.size() > 3)
    return !check(ir, name.substr(3));

  if (name == "取价节点")
    return detail::any_tool(ir, [](std::string_view t) {
      return t == "fetch_price" || t == "web_search";
    });
  if (name == "价格分支") return detail::any_expr_contains(ir, "price");
  if (name == "非空判断") return detail::has_emptiness_check(ir);
  if (name == "通知节点")
    return detail::any_tool(ir, [](std::string_view t) { return t == "send_notification"; });
  if (name == "time触发+schedule")
    return ir.trigger.source == "time" && ir.trigger.kind == "schedule";
  if (name == "trigger.condition") {
    // 出现在 must_not_have：要求 check == false（触发器不得带业务条件）
    return detail::has_key_anywhere(ir.trigger.raw, "condition");
  }
  if (name == "无trigger.condition")
    return !detail::has_key_anywhere(ir.trigger.raw, "condition");
  if (name == "llm_extract提取")
    return detail::any_tool(ir, [](std::string_view t) { return t == "llm_extract"; });
  if (name == "label/goto结构") {
    for (NodeIR const& n : ir.nodes)
      if (n.kind == NodeKind::Label || n.kind == NodeKind::Goto) return true;
    return false;
  }
  if (name == "on_error降级") {
    for (NodeIR const& n : ir.nodes)
      if (n.on_error && n.on_error->fallback) return true;
    return false;
  }
  if (name == "声称已下单") return detail::claims_purchase(ir);
  return false;  // 未知特征不认
}

// 12 例（8 golden + 4 boundary），意向逐字来自 10-eval-cases.md。
inline std::vector<EvalCase> const& eval_cases() {
  static std::vector<EvalCase> const cases = {
      // ---- GOLDEN_CASES（expect=Generated）----
      {"wf_taobao_price_drop",
       "淘宝上那款耳机降价到 800 以下就提醒我，别在半夜打扰。建议定时早 8 晚 8 各查一次价格，"
       "避开深夜，价格条件判断放在 workflow 里。",
       "Generated",
       {"取价节点", "价格分支", "通知节点", "time触发+schedule", "无trigger.condition"},
       {"trigger.condition"},
       {}},
      {"wf_report_price_above",
       "每天 9 点查一次某商品价格，如果价格高于 1000 就发通知提醒补货。",
       "Generated",
       {"取价节点", "价格分支", "通知节点", "time触发+schedule"},
       {"trigger.condition"},
       {}},
      {"wf_simple_notify",
       "每天早上 9 点提醒我喝水。",
       "Generated",
       {"通知节点", "time触发+schedule"},
       {"trigger.condition"},
       {}},
      {"wf_dual_source_fallback",
       "每天早晚各查一次某商品价格，如果主价格源(购物MCP)失败就用网页搜索降级取价，价格低于 500 "
       "就提醒我。",
       "Generated",
       {"取价节点", "on_error降级", "价格分支", "通知节点"},
       {"trigger.condition"},
       {}},
      {"wf_llm_extract_notify",
       "每天早 9 点用网页搜索查一次某新闻关键词的最新报道，用 LLM 提取摘要，然后把摘要通知我。",
       "Generated",
       {"取价节点", "llm_extract提取", "通知节点", "time触发+schedule"},
       {"trigger.condition"},
       {}},
      {"wf_poll_with_goto",
       "每天早上开始每 30 分钟轮询检查某商品是否缺货，缺货结束就通知我（最多轮询 4 次）。",
       "Generated",
       {"取价节点", "label/goto结构", "通知节点"},
       {"trigger.condition"},
       {}},
      {"wf_manual_order",
       "每天检查一次，如果商品有货就自动下单购买。",
       "Generated",
       {"通知节点"},
       {"声称已下单"},
       {}},
      // 原判为 Reject，但"结果非空"并非表达力缺口：IR 规范 §6 明确给出
      // `!= null` / `!= ""` 两个惯用法，实测也正确区分有内容与空串（见 11-ir-syntax.md）。
      // 真机 3/3 稳定生成，故改正为 golden。
      {"wf_expressiveness_gap",
       "每天早 8 点查最新科技新闻，如果取到的新闻不为空就播报给我。",
       "Generated",
       {"取价节点", "非空判断", "通知节点", "time触发+schedule", "无trigger.condition"},
       {"trigger.condition"},
       {}},
      // ---- BOUNDARY_CASES（Reject / Clarify）----
      {"wf_capability_gap",
       "收到淘宝推送通知时提醒我去比价。",
       "Reject", {}, {}, {}},
      // 原 `wf_llm_extract_summary`（"有结果就通知我"）与 golden 的 wf_llm_extract_notify
      // 几乎逐字相同，只差一个"非空"条件 —— 而该条件可表达，判据站不住。
      // 改为一条**真缺口**：要统计集合大小（超过 N 条），而 IR 的 expr 只有标量比较，
      // 没有函数/数组/len()；web_search 返回的又是整页文档，无法从中可靠计数。
      {"wf_count_gap",
       "每天上午用网页搜索查一次某新闻关键词的报道，如果结果超过 3 条就把标题汇总后通知我。",
       "Reject", {}, {}, {}},
      {"wf_bad_trigger_cond",
       "收到淘宝降价通知就提醒我，价格低于 800 就通知。",
       "Reject", {}, {}, {}},
      {"wf_ambiguous_morning",
       "早上提醒我看今天的新闻。",
       "Clarify",
       {"通知节点"},
       {},
       {"几点", "时间", "时", "工作日", "每天", "频率", "周末"}},
  };
  return cases;
}

}  // namespace demo::feature