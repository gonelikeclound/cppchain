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

  // ⑤ imports：动态库导入声明（形状先收一遍，真正 dlopen/校验在 load_workflow_imports）
  if (j.contains("imports")) {
    if (!j["imports"].is_array()) return err("imports 必须是数组");
    for (Json const& imp : j["imports"]) {
      if (!imp.is_object())
        return err("imports 里的条目不是对象（应为 {lib, nodes[]}）");
      ImportIR in;
      if (imp.contains("lib") && imp["lib"].is_string()) in.lib = imp["lib"].get<std::string>();
      if (imp.contains("nodes") && imp["nodes"].is_array())
        for (Json const& nn : imp["nodes"])
          if (nn.is_string()) in.nodes.push_back(nn.get<std::string>());
      if (in.lib.empty())
        return err("imports 里的条目缺 lib（库是信任单元，库名必须显式）");
      // nodes 可省略（A1 枚举时整库加载，见 plugin_node.hpp）；列了就做形状检查。
      for (std::string const& nm : in.nodes)
        if (nm.empty()) return err("imports['" + in.lib + "'] 的节点名不能为空");
      out.imports.push_back(std::move(in));
    }
  }

  Json const& nodes = j["nodes"];
  for (Json const& node : nodes) {
    if (!node.is_object()) return err("nodes[?] is not an object");
    if (!node.contains("id") || !node["id"].is_string())
      return err("node has no id (string)");
    std::string id = node["id"].get<std::string>();
    // 判别由注册表驱动（内置按 matches、导入按显式 kind 名）—— 与 classify 的区别
    // 是它能认出动态节点；classify 保留给"内置 only"的语义（生成器/测试仍可用）。
    NodeType const* type = node_type_for_node(node);
    if (type == nullptr) {
      auto kind = classify(node);  // 产出可回喂的错误信息（unknown kind / no key / ambiguous）
      if (!kind) return err(std::string(kind.error()) + " (node '" + id + "')");
      return err("internal: 已分类但找不到 NodeType（node '" + id + "'）");
    }
    NodeIR n;
    n.raw = node;
    n.id = std::move(id);
    n.kind = type->kind;
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
    // 按名分发：动态节点（NodeKind::Dynamic）也能序列化回原形状
    NodeType const* type = node_type_of_name(type_name_of(n));
    if (type != nullptr) type->write(node, n);
    nodes.push_back(std::move(node));
  }

  Json out{{"id", id}, {"trigger", std::move(t)}, {"nodes", std::move(nodes)}};
  if (!imports.empty()) {
    Json arr = Json::array();
    for (ImportIR const& im : imports) {
      Json entry{{"lib", im.lib}};
      if (!im.nodes.empty()) entry["nodes"] = im.nodes;  // 省略（A1 整库）语义往返要保住
      arr.push_back(std::move(entry));
    }
    out["imports"] = std::move(arr);
  }
  return out;
}

std::optional<std::size_t> WorkflowIR::index_of(std::string const& id) const {
  for (std::size_t i = 0; i < nodes.size(); ++i)
    if (nodes[i].id == id) return i;
  return std::nullopt;
}

}  // namespace cppchain::workflow