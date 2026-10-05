// cppchain —— src/workflow/ir.cpp
#include "cppchain/workflow/ir.hpp"

#include "cppchain/workflow/node_type.hpp"

#include <string>
#include <utility>

namespace cppchain::workflow {

namespace {

// 从 trigger 里按"简写形优先、通用形兜底"取出时序描述：
//   简写形 {source, timing:{kind, schedule}}（本项目产出）
//   通用形 {source, kind, args:{schedule, timezone}}（规范 §2.1 的完整结构）
void fill_timing(TriggerIR& out, Json const& trig) {
  Json const* timing = trig.contains("timing") && trig["timing"].is_object()
                           ? &trig["timing"]
                           : nullptr;
  if (timing != nullptr) {
    if (timing->contains("kind") && (*timing)["kind"].is_string())
      out.kind = (*timing)["kind"].get<std::string>();
    if (timing->contains("schedule") && (*timing)["schedule"].is_string())
      out.schedule = (*timing)["schedule"].get<std::string>();
    if (timing->contains("timezone") && (*timing)["timezone"].is_string())
      out.timezone = (*timing)["timezone"].get<std::string>();
    return;
  }
  // 通用形：kind 在 trigger 顶层，schedule/timezone 在 args 里
  if (trig.contains("args") && trig["args"].is_object()) {
    Json const& args = trig["args"];
    if (args.contains("schedule") && args["schedule"].is_string())
      out.schedule = args["schedule"].get<std::string>();
    if (args.contains("timezone") && args["timezone"].is_string())
      out.timezone = args["timezone"].get<std::string>();
  }
}

}  // namespace

std::expected<NodeKind, std::string> WorkflowIR::classify(Json const& node) {
  // 判别完全由**注册表**驱动：每个 NodeType 自己说"这个 JSON 是不是我"。
  // 命中 0 个 → 未知；命中 >1 个 → 歧义（fail-closed）。加一种节点不必再改这里。
  std::vector<NodeType const*> hits;
  for (NodeType const& t : builtin_node_types())
    if (t.matches(node)) hits.push_back(&t);

  if (hits.size() == 1) return hits[0]->kind;
  if (hits.size() > 1)
    return std::unexpected("ambiguous node: multiple decisive keys (kind/goto/expr/tool)");

  // 命中 0：写了 kind 但没人认领 → 报"未知 kind 值"（比笼统的 no decisive key 有用）
  if (node.contains("kind") && node["kind"].is_string())
    return std::unexpected("unknown node kind: '" + node["kind"].get<std::string>() +
                           "'（见 node_types.cpp 的注册表）");
  return std::unexpected(
      "unknown node: no decisive key (kind=label|goto|expr|tool|model|route|run_tools|"
      "load_history, 或 goto/expr/tool 键)");
}

std::expected<WorkflowIR, std::string> WorkflowIR::parse(Json const& j) {
  auto err = [](std::string m) { return std::unexpected(std::move(m)); };
  if (!j.is_object()) return err("root is not an object");
  if (!j.contains("id") || !j["id"].is_string()) return err("missing id (string)");
  if (!j.contains("trigger") || !j["trigger"].is_object())
    return err("missing trigger (object)");
  if (!j.contains("nodes") || !j["nodes"].is_array())
    return err("missing nodes (array)");

  WorkflowIR out;
  out.id = j["id"].get<std::string>();
  Json const& trig = j["trigger"];
  out.trigger.raw = trig;
  if (trig.contains("source") && trig["source"].is_string())
    out.trigger.source = trig["source"].get<std::string>();
  if (trig.contains("enabled") && trig["enabled"].is_boolean())
    out.trigger.enabled = trig["enabled"].get<bool>();
  fill_timing(out.trigger, trig);

  Json const& nodes = j["nodes"];
  for (Json const& node : nodes) {
    if (!node.is_object()) return err("nodes[?] is not an object");
    if (!node.contains("id") || !node["id"].is_string())
      return err("node has no id (string)");
    std::string id = node["id"].get<std::string>();
    auto kind = classify(node);
    if (!kind) return err(std::string(kind.error()) + " (node '" + id + "')");
    NodeType const* type = node_type_of(*kind);
    if (type == nullptr)  // 注册表缺项 = 内部错误
      return err("internal: no NodeType registered for this kind (node '" + id + "')");
    NodeIR n;
    n.raw = node;
    n.id = std::move(id);
    n.kind = *kind;
    type->parse(n, node);  // 类型专属字段
    out.nodes.push_back(std::move(n));
  }

  return out;
}

Json WorkflowIR::to_json() const {
  Json t = Json::object();
  t["source"] = trigger.source;
  t["timing"] = Json{{"kind", trigger.kind}};
  if (!trigger.schedule.empty()) t["timing"]["schedule"] = trigger.schedule;
  if (trigger.timezone) t["timing"]["timezone"] = *trigger.timezone;
  if (!trigger.enabled) t["enabled"] = false;

  Json nodes = Json::array();
  for (NodeIR const& n : this->nodes) {
    Json node{{"id", n.id}};
    NodeType const* type = node_type_of(n.kind);
    if (type != nullptr) type->write(node, n);
    nodes.push_back(std::move(node));
  }

  return Json{{"id", id}, {"trigger", std::move(t)}, {"nodes", std::move(nodes)}};
}

std::optional<std::size_t> WorkflowIR::index_of(std::string const& id) const {
  for (std::size_t i = 0; i < nodes.size(); ++i)
    if (nodes[i].id == id) return i;
  return std::nullopt;
}

}  // namespace cppchain::workflow