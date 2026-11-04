// examples/sa_demo/sa_plan.hpp
// SystemAgent（problems/64 · S7）——「一句话 → DAG」的规划契约与静态校验。
//
// 纪律（与 sa_guard.hpp 同思路，纯函数、无 IO）：
//   · Planner（LLM）只输出**受限 IR**：节点角色 ∈ {user,core,phone,msg,world}，
//     边 kind ∈ {query,dispatch,report,control,world}；不许自由发挥成代码/工具名。
//   · 生成后必须先 validate() 全过才允许执行（fail-closed）：核心只分诊、
//     每个域节点必须由核心 dispatch、图无环、所有节点在 user→world 路径上。
//   · layout() 只做展示层坐标（layer=最长路径，row 固定），不改变语义。
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cppchain::sa::plan {

using Json = nlohmann::json;

struct Issue {
  std::string code;
  std::string message;
};

inline bool is_role(std::string const& r) {
  return r == "user" || r == "core" || r == "phone" || r == "msg" || r == "world" ||
         r == "tool" || r == "synthesis";
}
inline bool is_domain_role(std::string const& r) { return r == "phone" || r == "msg"; }
// 执行节点 = 域节点（电话/消息）或工具节点（直接调 cppchain 工具）。
inline bool is_exec_role(std::string const& r) { return is_domain_role(r) || r == "tool"; }
inline bool is_edge_kind(std::string const& k) {
  return k == "query" || k == "dispatch" || k == "report" || k == "control" ||
         k == "synthesize" || k == "world";
}

// 从模型输出里抠出 JSON 对象：容忍 ```json 围栏与前后解释文字。
inline std::string extract_json_text(std::string text) {
  std::size_t const b = text.find('{');
  std::size_t const e = text.rfind('}');
  if (b == std::string::npos || e == std::string::npos || e < b) return {};
  return text.substr(b, e - b + 1);
}

// allowed_tools：Planner 可见的工具名集合（来自 ToolCatalog）。为空表示不校验工具名
// （纯结构测试用）；生产路径必须传，做到「清单外工具一律拒」。
inline std::vector<Issue> validate(Json const& p,
                                   std::vector<std::string> const& allowed_tools = {}) {
  std::vector<Issue> out;
  auto add = [&](std::string code, std::string msg) {
    out.push_back(Issue{std::move(code), std::move(msg)});
  };

  if (!p.is_object()) {
    add("P00", "计划必须是 JSON 对象");
    return out;
  }
  Json const nodes = p.value("nodes", Json::array());
  Json const edges = p.value("edges", Json::array());
  if (!nodes.is_array() || !edges.is_array()) {
    add("P00", "nodes / edges 必须是数组");
    return out;
  }
  if (nodes.size() < 3 || nodes.size() > 12) add("P01", "节点数须在 3..12（实际 " +
                                                            std::to_string(nodes.size()) + "）");
  if (edges.size() < 2 || edges.size() > 24) add("P02", "边数须在 2..24（实际 " +
                                                            std::to_string(edges.size()) + "）");

  std::map<std::string, std::string> role_of;  // id → role
  std::size_t n_user = 0, n_world = 0, n_domain = 0, n_core = 0, n_tool = 0, n_synth = 0;
  for (Json const& n : nodes) {
    if (!n.is_object()) {
      add("P03", "节点必须是对象");
      continue;
    }
    std::string const id = n.value("id", std::string());
    std::string const role = n.value("role", std::string());
    if (id.empty()) {
      add("P03", "节点缺 id");
      continue;
    }
    if (role_of.count(id) != 0) {
      add("P04", "节点 id 重复：" + id);
      continue;
    }
    if (!is_role(role)) {
      add("P05", "未知角色 '" + role + "'（节点 " + id + "）");
      continue;
    }
    if (n.contains("args") && !n["args"].is_object())
      add("P06", "节点 " + id + " 的 args 必须是对象");
    if (!n.value("task", std::string()).empty() && !n["task"].is_string())
      add("P06", "节点 " + id + " 的 task 必须是字符串");
    role_of[id] = role;
    if (role == "user") ++n_user;
    if (role == "world") ++n_world;
    if (role == "core") ++n_core;
    if (is_domain_role(role)) ++n_domain;
    if (role == "synthesis") {
      ++n_synth;
      if (n.value("task", std::string()).empty())
        add("P35", "综合节点 " + id + " 必须写清摘要/总结要求（task）");
    }
    if (role == "tool") {
      ++n_tool;
      std::string const tool_name = n.value("tool", std::string());
      if (tool_name.empty()) {
        add("P30", "工具节点 " + id + " 缺 tool 字段");
      } else if (!allowed_tools.empty()) {
        bool found = false;
        for (std::string const& t : allowed_tools)
          if (t == tool_name) found = true;
        if (!found) add("P30", "工具节点 " + id + " 引用了清单外工具：" + tool_name);
      }
    }
  }
  if (n_user != 1) add("P07", "必须恰好 1 个 user 节点（实际 " + std::to_string(n_user) + "）");
  if (n_world != 1) add("P08", "必须恰好 1 个 world 节点（实际 " + std::to_string(n_world) + "）");
  if (n_domain + n_tool < 1) add("P09", "至少 1 个执行节点（phone/msg/tool）");
  if (n_domain > 4) add("P10", "域节点最多 4 个");
  if (n_tool > 4) add("P31", "工具节点最多 4 个");
  if (n_synth > 1) add("P34", "综合节点最多 1 个");
  if (n_core > 4) add("P11", "核心节点最多 4 个");

  // —— 边：存在性 / 自环 / kind / 方向约束 ——
  struct E {
    std::string from, to, kind;
  };
  std::vector<E> es;
  std::map<std::string, std::vector<std::string>> out_edges, in_edges;
  for (Json const& e : edges) {
    if (!e.is_object()) {
      add("P12", "边必须是对象");
      continue;
    }
    E ce{e.value("from", std::string()), e.value("to", std::string()),
         e.value("kind", std::string())};
    if (!role_of.count(ce.from) || !role_of.count(ce.to)) {
      add("P13", "边端点不存在：" + ce.from + " → " + ce.to);
      continue;
    }
    if (ce.from == ce.to) add("P14", "不允许自环：" + ce.from);
    if (!is_edge_kind(ce.kind)) {
      add("P15", "未知边 kind '" + ce.kind + "'");
      continue;
    }
    std::string const fr = role_of[ce.from], to = role_of[ce.to];
    if (ce.kind == "query" && fr != "user")
      add("P16", "query 边必须从 user 出发（" + ce.from + "）");
    if (ce.kind == "dispatch" && (fr != "core" || !is_exec_role(to)))
      add("P17", "dispatch 边必须是 core → 域/工具（" + ce.from + " → " + ce.to + "）");
    if (ce.kind == "report" && (!is_exec_role(fr) || to != "core"))
      add("P18", "report 边必须是 域/工具 → core（" + ce.from + " → " + ce.to + "）");
    if (ce.kind == "control" && (fr != "user" || to != "phone"))
      add("P19", "control 边必须是 user → phone（" + ce.from + " → " + ce.to + "）");
    if (ce.kind == "synthesize" && (!is_exec_role(fr) || to != "synthesis"))
      add("P33", "synthesize 边必须是 域/工具 → synthesis（" + ce.from + " → " + ce.to + "）");
    if (ce.kind == "world" && to != "world")
      add("P20", "world 边必须指向 world（" + ce.from + " → " + ce.to + "）");
    out_edges[ce.from].push_back(ce.to);
    in_edges[ce.to].push_back(ce.from);
    es.push_back(std::move(ce));
  }

  // 改口（control）必须落到消息域：有 control 边却没有 msg 节点 = 改道无处可去。
  bool has_control = false;
  for (Json const& e : edges)
    if (e.value("kind", "") == "control") has_control = true;
  if (has_control) {
    bool has_msg = false;
    for (auto const& [id, role] : role_of)
      if (role == "msg") has_msg = true;
    if (!has_msg) add("P29", "存在 control（改口）边，但计划里没有 msg 域节点");
  }

  // 每个执行节点（域/工具）必须有 core→执行节点 的 dispatch 入边。
  for (auto const& [id, role] : role_of) {
    if (!is_exec_role(role)) continue;
    bool has_dispatch = false;
    for (Json const& e : edges)
      if (e.value("kind", "") == "dispatch" && e.value("to", "") == id) has_dispatch = true;
    if (!has_dispatch) add("P21", "执行节点 " + id + " 缺 core→节点 的 dispatch 入边");
  }
  // 综合节点必须有上游内容（域/工具 → synthesize），否则摘要是无源之水。
  for (auto const& [id, role] : role_of) {
    if (role != "synthesis") continue;
    bool has_input = false;
    for (Json const& e : edges)
      if (e.value("kind", "") == "synthesize" && e.value("to", "") == id) has_input = true;
    if (!has_input) add("P32", "综合节点 " + id + " 缺 域/工具 → synthesis 的 synthesize 入边");
  }

  // 每个 core 至少有 1 条 dispatch 出边（核心的职责就是分派）。
  for (auto const& [id, role] : role_of) {
    if (role != "core") continue;
    bool has_dispatch = false;
    for (Json const& e : edges)
      if (e.value("kind", "") == "dispatch" && e.value("from", "") == id) has_dispatch = true;
    if (!has_dispatch) add("P22", "核心节点 " + id + " 没有 dispatch 出边");
  }

  // —— 无环 + user→world 路径（所有节点必须同时在两侧）——
  std::map<std::string, int> indeg;
  for (auto const& [id, _] : role_of) indeg[id] = 0;
  for (auto const& e : es) ++indeg[e.to];
  std::vector<std::string> q, topo;
  for (auto const& [id, d] : indeg)
    if (d == 0) q.push_back(id);
  while (!q.empty()) {
    std::string const id = q.back();
    q.pop_back();
    topo.push_back(id);
    for (std::string const& nx : out_edges[id])
      if (--indeg[nx] == 0) q.push_back(nx);
  }
  if (topo.size() != role_of.size()) add("P23", "计划存在环");

  auto reach = [&](std::string const& start, std::map<std::string, std::vector<std::string>> const& g) {
    std::set<std::string> seen{start};
    std::vector<std::string> st{start};
    while (!st.empty()) {
      std::string const cur = st.back();
      st.pop_back();
      auto const it = g.find(cur);
      if (it == g.end()) continue;
      for (std::string const& nx : it->second)
        if (seen.insert(nx).second) st.push_back(nx);
    }
    return seen;
  };
  std::map<std::string, std::vector<std::string>> rev;
  for (auto const& [a, ns] : out_edges)
    for (std::string const& b : ns) rev[b].push_back(a);

  std::string user_id, world_id;
  for (auto const& [id, role] : role_of) {
    if (role == "user") user_id = id;
    if (role == "world") world_id = id;
  }
  if (!user_id.empty() && !world_id.empty()) {
    auto const from_user = reach(user_id, out_edges);
    auto const to_world = reach(world_id, rev);
    for (auto const& [id, _] : role_of) {
      if (id != user_id && !from_user.count(id))
        add("P24", "节点 " + id + " 不可从 user 到达");
      if (id != world_id && !to_world.count(id))
        add("P25", "节点 " + id + " 到不了 world");
    }
    if (!from_user.count(world_id)) add("P26", "user 到不了 world");
  }

  // 终点：world 无出边；起点：user 无入边。
  if (!world_id.empty() && !out_edges[world_id].empty()) add("P27", "world 不能有出边");
  if (!user_id.empty() && !in_edges[user_id].empty()) add("P28", "user 不能有入边");

  return out;
}

// 拓扑序（validate 通过后调用；失败返回空）。
inline std::vector<std::string> topo_order(Json const& p) {
  Json const nodes = p.value("nodes", Json::array());
  Json const edges = p.value("edges", Json::array());
  std::map<std::string, int> indeg;
  std::map<std::string, std::vector<std::string>> out;
  std::vector<std::string> ids;
  for (Json const& n : nodes) {
    std::string const id = n.value("id", std::string());
    ids.push_back(id);
    indeg[id] = 0;
  }
  for (Json const& e : edges) {
    std::string const a = e.value("from", std::string());
    std::string const b = e.value("to", std::string());
    if (!indeg.count(a) || !indeg.count(b)) continue;
    out[a].push_back(b);
    ++indeg[b];
  }
  std::vector<std::string> q, order;
  for (std::string const& id : ids)
    if (indeg[id] == 0) q.push_back(id);
  while (!q.empty()) {
    std::string const id = q.back();
    q.pop_back();
    order.push_back(id);
    for (std::string const& nx : out[id])
      if (--indeg[nx] == 0) q.push_back(nx);
  }
  return order.size() == ids.size() ? order : std::vector<std::string>{};
}

// 展示层布局：layer = 从 user 出发的最长路径；user row=0，其余 row=1。
inline Json layout(Json p) {
  auto order = topo_order(p);
  if (order.empty()) return p;
  std::map<std::string, int> layer;
  std::map<std::string, std::vector<std::string>> out;
  for (Json const& e : p.value("edges", Json::array()))
    out[e.value("from", std::string())].push_back(e.value("to", std::string()));
  for (std::string const& id : order) {
    if (!layer.count(id)) layer[id] = 0;
    for (std::string const& nx : out[id])
      layer[nx] = std::max(layer[nx], layer[id] + 1);
  }
  // 同一 layer 有多个节点时（如一对多 dispatch）分行，避免画布重叠；单节点层保持 row=1。
  std::map<int, std::vector<std::string>> by_layer;
  for (std::string const& id : order) by_layer[layer.count(id) ? layer[id] : 0].push_back(id);
  std::map<std::string, int> row_of;
  for (auto const& [ly, ids] : by_layer) {
    (void)ly;
    for (std::size_t i = 0; i < ids.size(); ++i) row_of[ids[i]] = static_cast<int>(i) + 1;
  }
  for (Json& n : p["nodes"]) {
    std::string const id = n.value("id", std::string());
    n["layer"] = layer.count(id) ? layer[id] : 0;
    n["row"] = (n.value("role", "") == "user") ? 0 : (row_of.count(id) ? row_of[id] : 1);
    if (!n.contains("label")) n["label"] = n.value("role", "");
  }
  return p;
}

inline std::string planner_system_prompt(std::string const& tool_catalog = {}) {
  std::string const tools_part =
      tool_catalog.empty()
          ? std::string("（本次没有可用工具；不要生成 tool 节点）")
          : tool_catalog;
  return std::string(R"PROMPT(你是 SystemAgent（SA）的任务规划器。把用户的一句话规划成执行图 DAG，只输出 JSON 对象，不要解释、不要 Markdown 围栏。

可用角色（role）：
- user：用户（唯一，起点）
- core：核心 agent，只分诊/改道/调度，不执行动作
- phone：电话域（可拨打电话；平台剧本：第 1 次会被拒接，按策略重拨 1 次后接通）
- msg：消息域（可发送消息）
- tool：工具节点，直接调用一个 cppchain 工具（字段 tool + args）
- synthesis：综合节点（最多 1 个），把上游工具/域的结果交给 LLM 生成摘要/报告/回答；
  字段 task 写清楚要生成什么（例如“用中文写 5 条要点摘要，标注来源与日期”）；
  可选 args: {"save_to":"相对路径"}（用户要求保存到文件时填）
- world：世界断言（唯一，终点）

节点字段：id、role、label、task；域节点可带 args 对象：
- phone: {"call_id":"c1","number":"13800000000"}
- msg: {"msg_id":"m1","to":"妈妈","text":"要发送的原文"}
- tool: {"tool":"<工具名，必须来自下方清单>","args":{...},"task":"人话说明"}

可用工具清单（tool 节点只能使用这里列出的工具）：
)PROMPT") + tools_part + R"PROMPT(
)PROMPT" + R"PROMPT(

边 kind：
- query：user → core/域（用户请求进入）
- dispatch：core → 域（分派）
- report：域/工具 → core（上报/改道）
- control：user → phone（用户改口 barge_in，仅电话域）
- synthesize：域/工具 → synthesis（把结果交给综合节点）
- world：域/工具/synthesis → world（写世界/断言）

硬规则：
1. 恰好 1 个 user、1 个 world；至少 1 个执行节点（phone/msg/tool）；节点 3..12、边 2..24。
2. 无环；所有节点都在 user→world 路径上；user 无入边、world 无出边。
3. 每个执行节点（域或工具）必须有 core→节点的 dispatch 入边；每个 core 必须有 dispatch 出边。
4. 若用户表示“改口 / 算了 / 换渠道 / 发微信”，在电话域之后加 core 改道节点到 msg，并在 user→phone 加一条 control 边（label 写“改口(barge_in)”），phone 的 task 写“接通后改道消息域”。
5. core 的 task 写要转达给执行节点的一句话（以“→电话域：”“→消息域：”或“→工具：”开头）。
6. msg 的 args.text 必须是用户要发的内容原文（可合理补全）；args.to 是收件人。
7. tool 节点的 tool 必须是下方清单里的名字，args 必须符合该工具参数；需要计算/查资料/读文件/查天气时优先用工具节点，不要让 core 编造结果。
8. 用户要求“摘要 / 总结 / 报告 / 整理 / 分析 / 回答”时，必须在工具或域节点之后加 **synthesis** 节点：
   用 synthesize 边把上游结果节点接到 synthesis，再 synthesis → world；把具体写作要求放进 synthesis.task。
   **不能只把“写摘要”写进 core 的 task**——core 只分诊，不会生成文本。
9. 长文档 / 整本书 / 长报告：优先用 **download_url** 落盘，再由 synthesis 分片摘要；
   不要用 fetch_url 截断后强行总结（fetch_url 有 2 万字上限）。
   若资料分卷 / 分页，应下载全部分卷/页后再摘要（可多个 tool 节点，路径写在各自 args）。
10. download_url 之后直接接 synthesis 即可（synthesis 会自动分片读取上游文件）；
    不要再加 file_read 节点重复读取整本书——那会把长文塞回上下文并可能因参数上限失败。
11. 用户要求把结果保存到文件时，在 synthesis 节点加 args: {"save_to":"相对路径"}
    （仅当工具清单里出现 file_write、即服务端已开启副作用工具时）。
12. 只规划、不执行；不要发明域或工具。

输出结构（字段示意；请按用户输入自行决定节点数量、角色与边，**不要照抄下面的结构**）：
{"title":"...","nodes":[{"id":"...","role":"...","label":"...","task":"...","tool":"...","args":{}}],"edges":[{"from":"...","to":"...","kind":"...","label":"..."}]}
只输出这个 JSON 对象，不要输出解释。
)PROMPT";
}

}  // namespace cppchain::sa::plan
