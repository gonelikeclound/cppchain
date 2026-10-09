// cppchain —— workflow/debug_view.hpp
// wfr 调试可视化：把 WorkflowIR + 会话日志导出成一个**自包含单 HTML**
//（内嵌 IR 图 JSON + 日志 JSON + 纯 JS/SVG，零网络依赖，浏览器直接打开）。
//
// 数据管线：
//   debug_view_graph(ir)    — IR → {nodes, edges}（节点带完整原始定义 def；边 = NodeType::edges()，
//                             raw target，前端把 _turn_end/_fail 当终端、label 为灰虚线）
//   （日志的推导在**页面里的 JS**：分组 / loop 迭代号 / 边使用 —— 因为"把日志文件拖进
//     页面"这条路径要在浏览器里推导，一份实现同时服务静态导出与拖入，wfr-㉟）
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

// 组装 {graph, ir{id}, records, trimmed_node_logs}。records 过 20k 时裁掉 source=node
// 诊断（保结构事件）并标 trimmed_node_logs=true。**只放原始记录**，推导交给页面。
[[nodiscard]] Json build_debug_payload(WorkflowIR const& ir, std::vector<Json> const& records);

// 渲染自包含单 HTML（内嵌 payload）。返回完整 HTML 字符串。
[[nodiscard]] std::string render_debug_html(Json const& payload);

}  // namespace cppchain::workflow