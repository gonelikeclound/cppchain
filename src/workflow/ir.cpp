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
    case NodeKind::Model:
      if (node.contains("model") && node["model"].is_string())
        out.model = node["model"].get<std::string>();
      if (node.contains("tools") && node["tools"].is_string())
        out.toolset = node["tools"].get<std::string>();  // 工具集**组件名**（§5.4 Q7）
      if (node.contains("system_prompt") && node["system_prompt"].is_string())
        out.system_prompt = node["system_prompt"].get<std::string>();
      if (node.contains("next") && node["next"].is_string())
        out.next = node["next"].get<std::string>();
      break;
    case NodeKind::Route:
      if (node.contains("on") && node["on"].is_string())
        out.on = node["on"].get<std::string>();
      if (node.contains("cases") && node["cases"].is_object())
        for (auto it = node["cases"].begin(); it != node["cases"].end(); ++it)
          if (it.value().is_string()) out.cases[it.key()] = it.value().get<std::string>();
      if (node.contains("default") && node["default"].is_string())
        out.default_ = node["default"].get<std::string>();
      break;
    case NodeKind::RunTools:
      if (node.contains("next") && node["next"].is_string())
        out.next = node["next"].get<std::string>();
      break;
    case NodeKind::LoadHistory:
      if (node.contains("path") && node["path"].is_string())
        out.path = node["path"].get<std::string>();
      if (node.contains("session") && node["session"].is_string())
        out.session = node["session"].get<std::string>();
      if (node.contains("next") && node["next"].is_string())
        out.next = node["next"].get<std::string>();
      break;
    case NodeKind::SaveHistory:
      if (node.contains("path") && node["path"].is_string())
        out.path = node["path"].get<std::string>();
      if (node.contains("session") && node["session"].is_string())
        out.session = node["session"].get<std::string>();
      if (node.contains("next") && node["next"].is_string())
        out.next = node["next"].get<std::string>();
      break;
  }
  return out;
}

}  // namespace

std::expected<NodeKind, std::string> WorkflowIR::classify(Json const& node) {
  // kind 是**通用**判别键：label/goto/expr/tool/model/route/run_tools 都可用它显式声明。
  // 未知值直接拒绝（不留"猜"的余地）。历史写法（靠 goto/expr/tool 键判别）继续支持。
  std::optional<NodeKind> declared;
  std::string kind_str;
  if (node.contains("kind") && node["kind"].is_string()) {
    kind_str = node["kind"].get<std::string>();
    if (kind_str == "label") declared = NodeKind::Label;
    else if (kind_str == "goto") declared = NodeKind::Goto;
    else if (kind_str == "expr") declared = NodeKind::Expr;
    else if (kind_str == "tool") declared = NodeKind::Tool;
    else if (kind_str == "model") declared = NodeKind::Model;
    else if (kind_str == "route") declared = NodeKind::Route;
    else if (kind_str == "run_tools") declared = NodeKind::RunTools;
    else if (kind_str == "load_history") declared = NodeKind::LoadHistory;
    else if (kind_str == "save_history") declared = NodeKind::SaveHistory;
    else
      return std::unexpected("unknown node kind: '" + kind_str +
                             "'（可用：label/goto/expr/tool/model/route/run_tools/"
                             "load_history/save_history）");
  }

  bool const has_goto = node.contains("goto");
  bool const has_expr = node.contains("expr");
  bool const has_tool = node.contains("tool");

  if (declared) {
    // 与声明**一致**的键不算歧义（如 {"kind":"tool","tool":...}）；**冲突**才算 —— fail-closed。
    if ((has_goto && *declared != NodeKind::Goto) ||
        (has_expr && *declared != NodeKind::Expr) ||
        (has_tool && *declared != NodeKind::Tool))
      return std::unexpected("ambiguous node: decisive key conflicts with kind='" + kind_str + "'");
    return *declared;
  }

  if (static_cast<int>(has_goto) + static_cast<int>(has_expr) + static_cast<int>(has_tool) > 1)
    return std::unexpected("ambiguous node: multiple decisive keys (goto/expr/tool)");
  if (has_goto) return NodeKind::Goto;
  if (has_expr) return NodeKind::Expr;
  if (has_tool) return NodeKind::Tool;
  return std::unexpected(
      "unknown node: no decisive key (kind=label|goto|expr|tool|model|route|run_tools,"
      " 或 goto/expr/tool 键)");
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
      case NodeKind::Model:
        node["kind"] = "model";
        node["model"] = n.model;
        if (!n.toolset.empty()) node["tools"] = n.toolset;
        if (!n.system_prompt.empty()) node["system_prompt"] = n.system_prompt;
        node["next"] = n.next;
        break;
      case NodeKind::Route: {
        node["kind"] = "route";
        node["on"] = n.on;
        Json cs = Json::object();
        for (auto const& [k, v] : n.cases) cs[k] = v;
        node["cases"] = std::move(cs);
        node["default"] = n.default_;
        break;
      }
      case NodeKind::RunTools:
        node["kind"] = "run_tools";
        node["next"] = n.next;
        break;
      case NodeKind::LoadHistory:
      case NodeKind::SaveHistory:
        node["kind"] = (n.kind == NodeKind::LoadHistory) ? "load_history" : "save_history";
        node["path"] = n.path;
        if (!n.session.empty()) node["session"] = n.session;
        node["next"] = n.next;
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