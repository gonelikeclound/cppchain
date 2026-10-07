// cppchain —— workflow/debug_view.hpp
// wfr 调试可视化：把 WorkflowIR + 会话日志导出成一个**自包含单 HTML**
//（内嵌 IR 图 JSON + 日志 JSON + 纯 JS/SVG，零网络依赖，浏览器直接打开）。
//
// 数据管线：
//   debug_view_graph(ir)    — IR → {nodes, edges}（节点带完整原始定义 def；边 = NodeType::edges()，
//                             raw target，前端把 _turn_end/_fail 当终端、label 为灰虚线）
//   debug_view_logs(records) — 日志 → 按 run 分组 + C++ 用 merge_slots 重放 loop_counts，
//                              每条记录内嵌 `loop` 字段（无需前端归约）
//   build_debug_payload     — {graph, ir, runs, trimmed_node_logs}；records 过大时裁 source=node
//   render_debug_html       — 单 HTML（纯 JS/SVG：三栏可拖布局、树形日志 run→节点→记录、
//                             选中节点 JSON 描述、点 node↔log 联动、循环 1,2,3.. 分组）
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace cppchain::workflow {

using Json = nlohmann::json;

struct WorkflowIR;

// IR → {nodes:[{id,kind,text,def}], edges:[{from,to,label,forward}]}；def = 节点原始 JSON。
// 边 = NodeType::edges()；target 保留原始串（前端把 _turn_end/_fail 当终端、label 当灰虚线）。
[[nodiscard]] Json debug_view_graph(WorkflowIR const& ir);

// 日志 → 按 seq 分 run；每条记录内嵌 `loop`（该时刻 loop_counts 快照，来自 goto 的
// update.loop_counts 重放，merge_slots 覆盖合并）。无 run 的记录归到 "?" 组。
[[nodiscard]] Json debug_view_logs(std::vector<Json> const& records);

// 组装 {graph, ir{id}, runs:[{run, records}]}。records 过 20k 时裁掉 source=node 诊断
//（保结构事件）并标 trimmed_node_logs=true。
[[nodiscard]] Json build_debug_payload(WorkflowIR const& ir, std::vector<Json> const& records);

// 渲染自包含单 HTML（内嵌 payload）。返回完整 HTML 字符串。
[[nodiscard]] std::string render_debug_html(Json const& payload);

}  // namespace cppchain::workflow