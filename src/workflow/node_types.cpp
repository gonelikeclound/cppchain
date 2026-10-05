// cppchain —— src/workflow/node_types.cpp
// 9 种内置节点类型的定义（解析 / 序列化 / 展示）。
// 这些代码原先散在 ir.cpp 的 parse_node / to_json 与 examples 的展示 switch 里；
// 集中到一处后，加一种节点只需在这里加一个 NodeType。
#include "cppchain/workflow/node_type.hpp"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "cppchain/workflow/expr.hpp"

namespace cppchain::workflow {

namespace {

// ---- JSON 取值小工具（各类型的 parse 共用）----

void get_str(NodeIR& n, Json const& node, char const* key, std::string& out) {
  (void)n;
  if (node.contains(key) && node[key].is_string()) out = node[key].get<std::string>();
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

void write_on_error(Json& node, NodeIR const& n) {
  if (!n.on_error) return;
  Json oe{{"retry", n.on_error->retry}};
  if (n.on_error->fallback) oe["fallback"] = *n.on_error->fallback;
  node["on_error"] = std::move(oe);
}

// 显式 kind 写法是否声明为本类型
bool kind_is(Json const& node, std::string_view name) {
  return node.contains("kind") && node["kind"].is_string() &&
         node["kind"].get<std::string>() == name;
}

// 历史写法：靠决定性键判别（不带 kind 时）
bool has(Json const& node, char const* key) { return node.contains(key); }

// output_schema 字段名只能是标识符字符（自 validator 搬来，原只被 VN8 使用）
bool is_ident_chars(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  return true;
}

// ---- 类型专属校验（VN5–VN11）----
// 通用图规则（目标存在/无环/可达/死 label）**不在这里**，由框架按 edges() 统一做（②）。

void v_label(NodeIR const& n, NodeType::ValidateContext const&, NodeType::ErrorSink const& err) {
  if (n.raw.is_object())
    for (auto it = n.raw.begin(); it != n.raw.end(); ++it)
      if (it.key() != "id" && it.key() != "kind")
        err("VN5", "label '" + n.id + "' 不得带字段 '" + it.key() + "'");
}

void v_tool(NodeIR const& n, NodeType::ValidateContext const& ctx,
            NodeType::ErrorSink const& err) {
  if (n.tool.empty()) err("VN6", "tool 节点 '" + n.id + "' 缺少 tool 名");
  if (!n.parameters.is_object())
    err("VN6", "tool 节点 '" + n.id + "' 的 parameters 必须是对象");
  if (n.next.empty()) err("VN6", "tool 节点 '" + n.id + "' 缺少 next（成功出口）");
  if (ctx.registry != nullptr) {
    tool::ToolSpec const* spec = ctx.registry->find(n.tool);
    if (spec == nullptr)
      err("VN6", "tool 节点 '" + n.id + "' 引用了目录里不存在的工具 '" + n.tool +
                     "'（可用工具见目录注册表）");
    else if (n.parameters.is_object()) {
      Json const& props = spec->parameters.value("properties", Json::object());
      for (auto it = n.parameters.begin(); it != n.parameters.end(); ++it)
        if (!props.contains(it.key()))
          err("VN7", "tool 节点 '" + n.id + "' 的参数 '" + it.key() + "' 不在工具 '" + n.tool +
                         "' 的 input schema 里");
      if (spec->parameters.contains("required") && spec->parameters["required"].is_array())
        for (Json const& r : spec->parameters["required"]) {
          std::string const key = r.get<std::string>();
          if (!n.parameters.contains(key))
            err("VN7", "tool 节点 '" + n.id + "' 缺少工具 '" + n.tool + "' 的必填参数 '" + key +
                           "'");
        }
    }
  }
  if (!n.output_schema.is_object())
    err("VN8", "tool 节点 '" + n.id + "' 的 output_schema 必须是对象");
  else
    for (auto it = n.output_schema.begin(); it != n.output_schema.end(); ++it) {
      bool const ok_type = it.value().is_string() &&
                           (it.value() == "number" || it.value() == "string" ||
                            it.value() == "boolean" || it.value() == "*");
      if (!is_ident_chars(it.key()))
        err("VN8", "tool 节点 '" + n.id + "' 的 output_schema 字段名 '" + it.key() + "' 非法");
      if (!ok_type)
        err("VN8", "tool 节点 '" + n.id + "' 的 output_schema 字段 '" + it.key() +
                       "' 的类型必须是 number/string/boolean/*");
    }
}

void v_expr(NodeIR const& n, NodeType::ValidateContext const&, NodeType::ErrorSink const& err) {
  auto parsed = Expr::parse(n.expr);
  if (!parsed) err("VN9", "expr 节点 '" + n.id + "' 的表达式不合法：" + parsed.error());
  if (n.then.empty()) err("VN9", "expr 节点 '" + n.id + "' 缺少 then");
  if (n.else_.empty()) err("VN9", "expr 节点 '" + n.id + "' 缺少 else");
}

void v_goto(NodeIR const& n, NodeType::ValidateContext const&, NodeType::ErrorSink const& err) {
  if (n.max <= 0)
    err("VN9", "goto 节点 '" + n.id + "' 的 max 必须是正整数（循环有界），收到 " +
                   std::to_string(n.max));
}

void v_model(NodeIR const& n, NodeType::ValidateContext const& ctx,
             NodeType::ErrorSink const& err) {
  if (n.model.empty())
    err("VN10", "model 节点 '" + n.id + "' 缺少 model（组件名）");
  else if (ctx.components != nullptr && ctx.components->model(n.model) == nullptr)
    err("VN10", "model 节点 '" + n.id + "' 引用的组件 '" + n.model +
                    "' 不在注册表里（对标 VN6 对工具名的做法）");
  if (n.next.empty()) err("VN10", "model 节点 '" + n.id + "' 缺少 next");
  if (!n.toolset.empty()) {
    if (ctx.components == nullptr || ctx.components->toolset(n.toolset) == nullptr) {
      err("VN10", "model 节点 '" + n.id + "' 引用的工具集 '" + n.toolset + "' 不在注册表里");
    } else {
      for (std::string const& t : *ctx.components->toolset(n.toolset))
        if (ctx.registry != nullptr && ctx.registry->find(t) == nullptr)
          err("VN10", "工具集 '" + n.toolset + "' 里的工具 '" + t + "' 不在工具注册表里");
    }
  }
}

void v_route(NodeIR const& n, NodeType::ValidateContext const&, NodeType::ErrorSink const& err) {
  if (!is_known_discriminant(n.on))
    err("VN10", "route 节点 '" + n.id + "' 的 on='" + n.on +
                    "' 不在封闭词表里（可用：last_ai_outcome）");
  for (auto const& [k, v] : n.cases) {
    if (!is_known_outcome(k))
      err("VN10", "route 节点 '" + n.id + "' 的 cases 键 '" + k +
                      "' 不在 outcome 词表里（可用：tool_calls / invalid_tool_calls / text）");
    (void)v;
  }
  if (n.default_.empty()) {
    std::size_t covered = 0;
    for (char const* o : {kOutcomeToolCalls, kOutcomeInvalidToolCalls, kOutcomeText})
      if (n.cases.count(o) > 0) ++covered;
    if (covered < 3)
      err("VN10", "route 节点 '" + n.id +
                      "' 既没有覆盖全部 outcome 也没有 default（漏档会静默走错分支）");
  }
}

void v_run_tools(NodeIR const& n, NodeType::ValidateContext const&,
                 NodeType::ErrorSink const& err) {
  if (n.next.empty()) err("VN10", "run_tools 节点 '" + n.id + "' 缺少 next");
}

void v_history(NodeIR const& n, NodeType::ValidateContext const&, NodeType::ErrorSink const& err) {
  std::string const what = (n.kind == NodeKind::LoadHistory) ? "load_history" : "save_history";
  if (n.path.empty()) err("VN11", what + " 节点 '" + n.id + "' 缺少 path（工作路径）");
  if (n.next.empty()) err("VN11", what + " 节点 '" + n.id + "' 缺少 next");
}

}  // namespace

std::vector<NodeType> const& builtin_node_types() {
  static std::vector<NodeType> const types = {
      // ---------------- tool ----------------
      {NodeKind::Tool, "tool",
       [](Json const& n) { return has(n, "tool") || kind_is(n, "tool"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "tool", out.tool);
         if (node.contains("parameters") && node["parameters"].is_object())
           out.parameters = node["parameters"];
         if (node.contains("output_schema") && node["output_schema"].is_object())
           out.output_schema = node["output_schema"];
         get_str(out, node, "next", out.next);
         out.on_error = parse_on_error(node);
       },
       [](Json& node, NodeIR const& n) {
         node["tool"] = n.tool;
         if (!n.parameters.empty()) node["parameters"] = n.parameters;
         if (!n.output_schema.empty()) node["output_schema"] = n.output_schema;
         node["next"] = n.next;
         write_on_error(node, n);
       },
       [](NodeIR const& n) {
         std::string params;
         if (n.parameters.is_object())
           for (auto it = n.parameters.begin(); it != n.parameters.end(); ++it)
             params += (params.empty() ? "" : ", ") + it.key();
         std::string onerr;
         if (n.on_error) {
           if (n.on_error->retry > 0) onerr += " (retry " + std::to_string(n.on_error->retry) + ")";
           if (n.on_error->fallback) onerr += " fallback=" + *n.on_error->fallback;
         }
         return "tool " + n.tool + " (" + params + ") → " + n.next + onerr;
       }, v_tool},

      // ---------------- expr ----------------
      {NodeKind::Expr, "expr",
       [](Json const& n) { return has(n, "expr") || kind_is(n, "expr"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "expr", out.expr);
         get_str(out, node, "then", out.then);
         get_str(out, node, "else", out.else_);
       },
       [](Json& node, NodeIR const& n) {
         node["expr"] = n.expr;
         node["then"] = n.then;
         node["else"] = n.else_;
       },
       [](NodeIR const& n) {
         return "expr \"" + n.expr + "\" → then=" + n.then + " else=" + n.else_;
       }, v_expr},

      // ---------------- label ----------------
      {NodeKind::Label, "label",
       [](Json const& n) { return kind_is(n, "label"); },
       [](NodeIR&, Json const&) {},  // 只有 id/kind
       [](Json& node, NodeIR const&) { node["kind"] = "label"; },
       [](NodeIR const&) { return std::string("label"); }, v_label},

      // ---------------- goto ----------------
      {NodeKind::Goto, "goto",
       [](Json const& n) { return has(n, "goto") || kind_is(n, "goto"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "goto", out.goto_target);
         if (node.contains("max") && node["max"].is_number_integer())
           out.max = node["max"].get<int>();
       },
       [](Json& node, NodeIR const& n) {
         node["goto"] = n.goto_target;
         node["max"] = n.max;
       },
       [](NodeIR const& n) {
         return "goto " + n.goto_target + " (max " + std::to_string(n.max) + ")";
       }, v_goto},

      // ---------------- model（M12）----------------
      {NodeKind::Model, "model",
       [](Json const& n) { return kind_is(n, "model"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "model", out.model);
         get_str(out, node, "tools", out.toolset);  // 工具集**组件名**
         get_str(out, node, "system_prompt", out.system_prompt);
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "model";
         node["model"] = n.model;
         if (!n.toolset.empty()) node["tools"] = n.toolset;
         if (!n.system_prompt.empty()) node["system_prompt"] = n.system_prompt;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         return "model " + n.model + " (tools=" + (n.toolset.empty() ? "-" : n.toolset) + ") → " +
                n.next;
       }, v_model},

      // ---------------- route（M12）----------------
      {NodeKind::Route, "route",
       [](Json const& n) { return kind_is(n, "route"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "on", out.on);
         if (node.contains("cases") && node["cases"].is_object())
           for (auto it = node["cases"].begin(); it != node["cases"].end(); ++it)
             if (it.value().is_string()) out.cases[it.key()] = it.value().get<std::string>();
         get_str(out, node, "default", out.default_);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "route";
         node["on"] = n.on;
         Json cs = Json::object();
         for (auto const& [k, v] : n.cases) cs[k] = v;
         node["cases"] = std::move(cs);
         node["default"] = n.default_;
       },
       [](NodeIR const& n) {
         std::string cs;
         for (auto const& [k, v] : n.cases) cs += (cs.empty() ? "" : " ") + k + "→" + v;
         return "route on=" + n.on + " {" + cs + "} default=" + n.default_;
       }, v_route},

      // ---------------- run_tools（M12）----------------
      {NodeKind::RunTools, "run_tools",
       [](Json const& n) { return kind_is(n, "run_tools"); },
       [](NodeIR& out, Json const& node) { get_str(out, node, "next", out.next); },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "run_tools";
         node["next"] = n.next;
       },
       [](NodeIR const& n) { return "run_tools → " + n.next; }, v_run_tools},

      // ---------------- load_history / save_history（M12）----------------
      {NodeKind::LoadHistory, "load_history",
       [](Json const& n) { return kind_is(n, "load_history"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "path", out.path);
         get_str(out, node, "session", out.session);
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "load_history";
         node["path"] = n.path;
         if (!n.session.empty()) node["session"] = n.session;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         return "load_history path=" + n.path +
                " session=" + (n.session.empty() ? "-" : n.session) + " → " + n.next;
       }, v_history},

      {NodeKind::SaveHistory, "save_history",
       [](Json const& n) { return kind_is(n, "save_history"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "path", out.path);
         get_str(out, node, "session", out.session);
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "save_history";
         node["path"] = n.path;
         if (!n.session.empty()) node["session"] = n.session;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         return "save_history path=" + n.path +
                " session=" + (n.session.empty() ? "-" : n.session) + " → " + n.next;
       }, v_history},
  };
  return types;
}

NodeType const* node_type_of(NodeKind k) {
  for (NodeType const& t : builtin_node_types())
    if (t.kind == k) return &t;
  return nullptr;
}

}  // namespace cppchain::workflow
