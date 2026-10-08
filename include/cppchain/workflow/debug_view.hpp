// cppchain —— workflow/debug_view.hpp
// wfr 调试可视化：把 WorkflowIR + 会话日志导出成一个**自包含单 HTML**
//（内嵌 IR 图 JSON + 日志 JSON + 纯 JS/SVG，零网络依赖，浏览器直接打开）。
//
// 数据管线：
//   debug_view_graph(ir)    — IR → {nodes, edges}（节点带完整原始定义 def；边 = NodeType::edges()，
//                             raw target，前端把 _turn_end/_fail 当终端、label 为灰虚线）
//   debug_view_logs(recs,ir) — 日志 → 按 run 分组 + C++ 用 merge_slots 重放 loop_counts，
//                              每条记录内嵌 `loop`（该时刻快照）以及 **`loop_id`/`iter`**
//                              （该记录属于哪个 loop 的第几轮 —— 前端据此把日志树按
//                              ↺ loop:1 / ↺ loop:2 /（循环外）分组）
//   build_debug_payload     — {graph, ir, runs[{run, records, edges}], trimmed_node_logs}；
//                             records 过大时裁 source=node
//   render_debug_html       — 单 HTML（纯 JS/SVG：三栏可拖布局、树形日志 轮次→loop 迭代→节点、
//                             选中节点 JSON 描述、选中日志下半区美化 JSON、点 node↔log 联动、
//                             循环 1,2,3.. 分组）
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

// 日志 → 按 seq 分 run；每条记录内嵌：
//   edges   本轮**实际走过**的边及其次数（从引擎 enter 序列推；终端边靠节点发布的
//           target 或"唯一 terminal 出边"补）—— 前端据此高亮走过的边并标 ×N
//   loop    该时刻 loop_counts 快照（重放 update.loop_counts，merge_slots 覆盖合并）
//   loop_id 该记录所属的 loop 节点 id（不在任何循环里则无此键）
//   iter    该记录属于该 loop 的第几轮（1 起；同上，不在循环里则无此键）
// iter 的取法：body 节点取"本条之后"的计数；**loop 节点头特殊**——它的 enter 在自增之前、
// exit 才是那次自增，所以取"此后第一次自增后的值"，让 enter/exit 这对记录都落在第 N 轮。
[[nodiscard]] Json debug_view_logs(std::vector<Json> const& records, WorkflowIR const& ir);

// 组装 {graph, ir{id}, runs:[{run, records}]}。records 过 20k 时裁掉 source=node 诊断
//（保结构事件）并标 trimmed_node_logs=true。
[[nodiscard]] Json build_debug_payload(WorkflowIR const& ir, std::vector<Json> const& records);

// 渲染自包含单 HTML（内嵌 payload）。返回完整 HTML 字符串。
[[nodiscard]] std::string render_debug_html(Json const& payload);

}  // namespace cppchain::workflow