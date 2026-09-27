// cppchain —— src/workflow/ir.cpp
#include "cppchain/workflow/ir.hpp"

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

std::optional<OnErrorIR> parse_on_error(Json const& node) {
  if (!node.contains("on_error") || !node["on_error"].is_object()) return std::nullopt;
  Json const& oe = node["on_error"];
  OnErrorIR out;
  if (oe.contains("retry") && oe["retry"].is_number_integer())
    out.retry = oe["retry"].get<int>();
  if (oe.contains("fallback") && oe["fallback"].is_string())
    out.fallback = oe["fallback"].get<std::string>();
  return out;
}

NodeIR parse_node(Json const& node, NodeKind kind, std::string const& id) {
  NodeIR out;
  out.raw = node;
  out.id = id;
  out.kind = kind;
  switch (kind) {
    case NodeKind::Tool:
      if (node.contains("tool") && node["tool"].is_string())
        out.tool = node["tool"].get<std::string>();
      if (node.contains("parameters") && node["parameters"].is_object())
        out.parameters = node["parameters"];
      if (node.contains("output_schema") && node["output_schema"].is_object())
        out.output_schema = node["output_schema"];
      if (node.contains("next") && node["next"].is_string())
        out.next = node["next"].get<std::string>();
      out.on_error = parse_on_error(node);
      break;
    case NodeKind::Expr:
      if (node.contains("expr") && node["expr"].is_string())
        out.expr = node["expr"].get<std::string>();
      if (node.contains("then") && node["then"].is_string())
        out.then = node["then"].get<std::string>();
      if (node.contains("else") && node["else"].is_string())
        out.else_ = node["else"].get<std::string>();
      break;
    case NodeKind::Label:
      break;  // 只有 kind:id
    case NodeKind::Goto:
      if (node.contains("goto") && node["goto"].is_string())
        out.goto_target = node["goto"].get<std::string>();
      if (node.contains("max") && node["max"].is_number_integer())
        out.max = node["max"].get<int>();
      break;
  }
  return out;
}

}  // namespace

std::expected<NodeKind, std::string> WorkflowIR::classify(Json const& node) {
  bool const explicit_label = node.contains("kind") && node["kind"] == "label";
  bool const has_goto = node.contains("goto");
  bool const has_expr = node.contains("expr");
  bool const has_tool = node.contains("tool");

  // 歧义即错（fail-closed）：label 是显式标记，与任何执行类键并存都算歧义；
  // 两个执行类键并存也歧义。
  if (explicit_label && (has_goto || has_expr || has_tool))
    return std::unexpected("ambiguous node: kind:label co-occurs with a decisive key");
  if (static_cast<int>(has_goto) + static_cast<int>(has_expr) + static_cast<int>(has_tool) > 1)
    return std::unexpected("ambiguous node: multiple decisive keys (goto/expr/tool)");

  if (explicit_label) return NodeKind::Label;
  if (has_goto) return NodeKind::Goto;
  if (has_expr) return NodeKind::Expr;
  if (has_tool) return NodeKind::Tool;
  return std::unexpected("unknown node: no decisive key (kind=label | goto | expr | tool)");
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
    out.nodes.push_back(parse_node(node, *kind, std::move(id)));
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
    switch (n.kind) {
      case NodeKind::Tool:
        node["tool"] = n.tool;
        if (!n.parameters.empty()) node["parameters"] = n.parameters;
        if (!n.output_schema.empty()) node["output_schema"] = n.output_schema;
        node["next"] = n.next;
        if (n.on_error) {
          Json oe{{"retry", n.on_error->retry}};
          if (n.on_error->fallback) oe["fallback"] = *n.on_error->fallback;
          node["on_error"] = std::move(oe);
        }
        break;
      case NodeKind::Expr:
        node["expr"] = n.expr;
        node["then"] = n.then;
        node["else"] = n.else_;
        break;
      case NodeKind::Label:
        node["kind"] = "label";
        break;
      case NodeKind::Goto:
        node["goto"] = n.goto_target;
        node["max"] = n.max;
        break;
    }
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