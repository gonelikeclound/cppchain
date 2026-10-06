// cppchain —— src/workflow/node_types.cpp
// 9 种内置节点类型的定义（解析 / 序列化 / 展示）。
// 这些代码原先散在 ir.cpp 的 parse_node / to_json 与 examples 的展示 switch 里；
// 集中到一处后，加一种节点只需在这里加一个 NodeType。
#include "cppchain/workflow/node_type.hpp"

#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "cppchain/message/serializer.hpp"
#include "cppchain/tool/tool_node.hpp"
#include "cppchain/workflow/expr.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/session_log.hpp"

namespace cppchain::workflow {

using cppchain::graph::Command;
using cppchain::graph::NodeContext;

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

void v_load_history(NodeIR const& n, NodeType::ValidateContext const&,
                    NodeType::ErrorSink const& err) {
  if (n.path.empty()) err("VN11", "load_history 节点 '" + n.id + "' 缺少 path（工作路径）");
  if (n.next.empty()) err("VN11", "load_history 节点 '" + n.id + "' 缺少 next");
}

// ---- 编译期助手（自 compiler.cpp 搬来）----

// tool 节点：把 parameters 里的每个值解析为运行期 JSON 参数（引用/模板替换）
Json resolve_args(Json const& params, Json const& state) {
  Json out = Json::object();
  for (auto it = params.begin(); it != params.end(); ++it)
    out[it.key()] = resolve_parameter(it.value(), state);
  return out;
}

// 状态里的 messages 是**规范 JSON**，这里与 Message 互转
std::vector<message::Message> messages_from_state(Json const& state) {
  std::vector<message::Message> out;
  auto const it = state.find(kMessagesKey);
  if (it == state.end() || !it->is_array()) return out;
  out.reserve(it->size());
  for (Json const& raw : *it)
    try {
      out.push_back(raw.get<message::Message>());
    } catch (std::exception const&) {
      // 坏消息跳过；形状由上游 validator 保证
    }
  return out;
}

// 最后一条 AI 消息的归一化 outcome（封闭词表）。无 AI 消息 → 空串（不匹配任何 case）→ 走 default
std::string last_ai_outcome(Json const& state) {
  auto const msgs = messages_from_state(state);
  for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
    if (it->type != message::Role::Ai) continue;
    if (!it->tool_calls.empty()) return kOutcomeToolCalls;
    if (!it->invalid_tool_calls.empty()) return kOutcomeInvalidToolCalls;
    return kOutcomeText;
  }
  return {};
}

std::string as_string(Json const& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

// ---- 出边（通用图规则的唯一输入）----

std::vector<NodeType::Edge> e_tool(NodeIR const& n) {
  std::vector<NodeType::Edge> out{{n.next, "next", true}};
  // on_error.fallback 也是一条**前向边**：它必须是存在的节点，且算一条可达路径。
  // 把它渲染成边之后，VG7 的可达性判定就与节点类型完全无关了（原先要特判 tool）。
  if (n.on_error && n.on_error->fallback)
    out.push_back({*n.on_error->fallback, "on_error.fallback", true});
  return out;
}
std::vector<NodeType::Edge> e_expr(NodeIR const& n) {
  return {{n.then, "then", true}, {n.else_, "else", true}};
}
std::vector<NodeType::Edge> e_label(NodeIR const&) { return {}; }
std::vector<NodeType::Edge> e_goto(NodeIR const& n) {
  return {{n.goto_target, "goto", /*forward=*/false}};  // ← 回边，是循环的来源
}
std::vector<NodeType::Edge> e_next(NodeIR const& n) { return {{n.next, "next", true}}; }
std::vector<NodeType::Edge> e_route(NodeIR const& n) {
  std::vector<NodeType::Edge> out;
  for (auto const& [k, v] : n.cases) out.push_back({v, "cases." + k, true});
  if (!n.default_.empty()) out.push_back({n.default_, "default", true});
  return out;
}

// ---- 引用文本片段（VR1 语法检查 + VR 段引用收集共用）----

std::vector<std::string> t_none(NodeIR const&) { return {}; }

std::vector<std::string> t_tool(NodeIR const& n) {
  std::vector<std::string> out;
  std::function<void(Json const&)> walk = [&](Json const& v) {
    if (v.is_string()) {
      out.push_back(v.get<std::string>());
    } else if (v.is_object()) {
      for (auto it = v.begin(); it != v.end(); ++it) walk(it.value());
    } else if (v.is_array()) {
      for (Json const& e : v) walk(e);
    }
  };
  walk(n.parameters);
  return out;
}
std::vector<std::string> t_expr(NodeIR const& n) { return {n.expr}; }
std::vector<std::string> t_history(NodeIR const& n) {
  std::vector<std::string> out{n.path};
  if (!n.session.empty()) out.push_back(n.session);
  return out;
}

// ---- 编译：把节点变成图节点 ----

void c_tool(NodeIR const& n, NodeType::CompileContext& ctx) {
  std::string const node_id = n.id;
  std::string const tool = n.tool;
  Json const parameters = n.parameters;
  int const retry = n.on_error ? n.on_error->retry : 0;
  // fallback 与 next/then/else 走**同一条**解析（label → 续接、_turn_end → 终止）。
  // 原先把原始 id 直接塞进 Command.goto，而 label 不注册为图节点 → 一跑就 unknown node。
  std::optional<std::string> const fallback =
      (n.on_error && n.on_error->fallback)
          ? std::optional<std::string>(ctx.resolve(*n.on_error->fallback))
          : std::nullopt;
  tool::ToolRegistry const* reg = ctx.registry;

  ctx.graph.add_node(
      n.id, [node_id, tool, parameters, retry, fallback, reg](Json const& state, NodeContext&) {
        tool::ToolSpec const* spec = reg->find(tool);
        if (spec == nullptr)
          throw tool::ToolException("tool '" + tool + "' not in registry (node '" + node_id + "')");

        Json const args = resolve_args(parameters, state);
        tool::ToolContext tctx;
        tctx.tool_call_id = node_id;  // 幂等/审计兜底：节点 id 即本次调用的标识

        std::string last_err;
        for (int a = 0; a <= retry; ++a) {
          // handler **抛异常**与返回 unexpected 同等对待：都算本次尝试失败，交给 retry/fallback。
          // 比照 ToolNode::execute_one 的纪律 —— 否则 on_error 在最常见的失败形态（工具抛错）
          // 下完全不生效：IR 明写了 fallback 也照样崩。
          std::expected<nlohmann::json, tool::ToolError> r =
              std::unexpected(tool::ToolError{"(handler 未产生结果)"});
          try {
            r = spec->handler(args, tctx);
          } catch (std::exception const& e) {
            r = std::unexpected(tool::ToolError{std::string("handler threw: ") + e.what()});
          } catch (...) {
            r = std::unexpected(tool::ToolError{"handler threw: (非 std 异常)"});
          }
          if (r) {
            Json update = Json{{kResultsKey, Json{{node_id, *r}}}, {kLastExecKey, node_id}};
            return Command::with_update(std::move(update));
          }
          last_err = r.error().message;
        }
        if (fallback) return Command{{}, {*fallback}};
        throw tool::ToolException("tool '" + tool + "' failed after " + std::to_string(retry + 1) +
                                  " attempts (node '" + node_id + "'): " + last_err);
      });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

void c_expr(NodeIR const& n, NodeType::CompileContext& ctx) {
  std::string const then_t = ctx.resolve(n.then);
  std::string const else_t = ctx.resolve(n.else_);
  auto parsed = Expr::parse(n.expr);
  if (!parsed) {
    ctx.error = "expr 节点 '" + n.id + "' 语法错误：" + parsed.error();
    return;
  }
  auto expr_ptr = std::make_shared<Expr>(std::move(*parsed));
  ctx.graph.add_node(n.id, [](Json const&, NodeContext&) { return Command{}; });
  ctx.graph.add_conditional_edges(n.id, [expr_ptr, then_t, else_t](Json const& state) {
    return std::vector<std::string>{expr_ptr->eval(state) ? then_t : else_t};
  });
  ++ctx.registered;
}

void c_label(NodeIR const&, NodeType::CompileContext&) {}  // 不注册（指针已替换）

void c_goto(NodeIR const& n, NodeType::CompileContext& ctx) {
  std::string const label = n.goto_target;
  std::string const target = label.empty() ? graph::kEnd : ctx.resolve(label);
  int const max = n.max;
  ctx.graph.add_node(n.id, [n_ = n.id, target, max](Json const& state, NodeContext&) {
    long cur = 0;
    if (state.contains(kLoopCountsKey))
      if (auto const& lc = state.at(kLoopCountsKey); lc.contains(n_) && lc[n_].is_number())
        cur = lc[n_].get<long>();
    long const next = cur + 1;
    Json update = Json{{kLoopCountsKey, Json{{n_, next}}}};
    Command c;
    c.update = std::move(update);
    c.goto_nodes = {next >= max ? std::string(graph::kEnd) : target};
    return c;
  });
  ++ctx.registered;
}

void c_model(NodeIR const& n, NodeType::CompileContext& ctx) {
  model::ChatModel* m =
      (ctx.components != nullptr) ? ctx.components->model(n.model) : nullptr;
  if (m == nullptr) {
    ctx.error = "model 节点 '" + n.id + "' 引用的组件 '" + n.model + "' 不可用";
    return;
  }
  Json tools_json = Json::array();
  if (!n.toolset.empty() && ctx.components != nullptr)
    if (auto const* ts = ctx.components->toolset(n.toolset); ts != nullptr)
      for (std::string const& tname : *ts)
        if (auto const* sp = ctx.registry->find(tname); sp != nullptr)
          tools_json.push_back(sp->to_openai());
  std::string const sys = n.system_prompt;
  std::string const model_name = n.model;
  auto sink_p = ctx.sink;  // 触发期注入的 NodeLog（docs/12 §6.4：source=node 的诊断日志）

  ctx.graph.add_node(n.id, [m, tools_json, sys, model_name, sink_p](Json const& state,
                                                                    NodeContext& nc) {
    std::vector<message::Message> msgs = messages_from_state(state);
    if (!sys.empty()) msgs.insert(msgs.begin(), message::system_message(sys));
    model::CallOptions co;
    co.tools = tools_json;
    auto r = m->invoke(msgs, co);
    if (!r) throw tool::ToolException("model 节点调用失败：" + r.error().message);
    // **只发一次**：回复追加进 messages，循环由 route + 回边负责
    sink_p->log_node(nc.node_name, "info", "model '" + model_name + "' 完成一次调用",
                     Json{{"in_msg", msgs.size()}, {"tool_calls", !r->tool_calls.empty()}});
    return Command::with_update(Json{{kMessagesKey, Json::array({*r})}});
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

void c_route(NodeIR const& n, NodeType::CompileContext& ctx) {
  std::map<std::string, std::string> targets;
  for (auto const& [k, v] : n.cases) targets[k] = ctx.resolve(v);
  std::string const dflt = ctx.resolve(n.default_);
  ctx.graph.add_node(n.id, [](Json const&, NodeContext&) { return Command{}; });
  ctx.graph.add_conditional_edges(n.id, [targets, dflt](Json const& state) {
    std::string const outcome = last_ai_outcome(state);
    auto const it = targets.find(outcome);
    return std::vector<std::string>{it != targets.end() ? it->second : dflt};
  });
  ++ctx.registered;
}

void c_run_tools(NodeIR const& n, NodeType::CompileContext& ctx) {
  tool::ToolNode tn(ctx.registry);  // 复用 M3 的实现（并行、顺序对齐、错误回喂）
  ctx.graph.add_node(n.id, [tn](Json const& state, NodeContext&) {
    std::vector<message::ToolCall> calls;
    auto const msgs = messages_from_state(state);
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
      if (it->type == message::Role::Ai) {
        calls = it->tool_calls;
        break;
      }
    if (calls.empty())
      throw tool::ToolException("run_tools 节点：最后一条 AI 消息没有 tool_calls");
    Json arr = Json::array();
    for (message::Message& m : tn.run(calls)) arr.push_back(m);
    return Command::with_update(Json{{kMessagesKey, std::move(arr)}});
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

void c_load_history(NodeIR const& n, NodeType::CompileContext& ctx) {
  Json const path_expr = Json(n.path);
  Json const sess_expr = Json(n.session);
  auto sink_p = ctx.sink;
  // ④：history = 日志的投影 —— 读本会话日志 → replay → 取 messages（docs/12 §7）。
  // 与 D 阶段的"读 history 文件"差别：文件是 append-only 日志，宿主不再单独 save；
  // 本轮输入也已由 fire_once 写成 input 事件排在日志里，replay 天然含它。
  ctx.graph.add_node(n.id, [path_expr, sess_expr, sink_p](Json const& state, NodeContext& nc) {
    std::string const ws = as_string(resolve_parameter(path_expr, state));
    std::string const sess = as_string(resolve_parameter(sess_expr, state));
    auto const hist = replay_session_messages(session_path(ws, sess));
    sink_p->log_node(nc.node_name, "info",
                     "load_history: replay " + std::to_string(hist.size()) + " messages",
                     Json{{"workspace", ws}, {"session", sess}});
    Json arr = Json::array();
    for (message::Message const& m : hist) arr.push_back(m);
    // 走 messages 通道 → add_messages 与本次输入合并（同 id 覆盖、无 id 追加）
    return Command::with_update(Json{{kMessagesKey, std::move(arr)}});
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
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
       }, v_tool, c_tool, e_tool, t_tool},

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
       }, v_expr, c_expr, e_expr, t_expr},

      // ---------------- label ----------------
      {NodeKind::Label, "label",
       [](Json const& n) { return kind_is(n, "label"); },
       [](NodeIR&, Json const&) {},  // 只有 id/kind
       [](Json& node, NodeIR const&) { node["kind"] = "label"; },
       [](NodeIR const&) { return std::string("label"); }, v_label, c_label, e_label, t_none},

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
       }, v_goto, c_goto, e_goto, t_none},

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
       }, v_model, c_model, e_next, t_none},

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
       }, v_route, c_route, e_route, t_none},

      // ---------------- run_tools（M12）----------------
      {NodeKind::RunTools, "run_tools",
       [](Json const& n) { return kind_is(n, "run_tools"); },
       [](NodeIR& out, Json const& node) { get_str(out, node, "next", out.next); },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "run_tools";
         node["next"] = n.next;
       },
       [](NodeIR const& n) { return "run_tools → " + n.next; }, v_run_tools, c_run_tools, e_next, t_none},

      // ---------------- load_history（M12 / ④）----------------
      // history = 日志的投影（docs/12 §7）：不再有 save_history 节点。
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
       }, v_load_history, c_load_history, e_next, t_history},
  };
  return types;
}

NodeType const* node_type_of(NodeKind k) {
  for (NodeType const& t : builtin_node_types())
    if (t.kind == k) return &t;
  return nullptr;
}

// ---- ⑤ 运行期导入注册表（docs/12 §5）----
// 内置保持集中注册（可读性资产）；导入类型进程期追加在这里。名字为唯一键（内置优先）。
namespace {

std::unordered_map<std::string, std::unique_ptr<NodeType>>& imported_node_types() {
  static std::unordered_map<std::string, std::unique_ptr<NodeType>> m;
  return m;
}

}  // namespace

NodeType const* node_type_of_name(std::string_view name) {
  for (NodeType const& t : builtin_node_types())
    if (t.name == name) return &t;
  auto& m = imported_node_types();
  auto const it = m.find(std::string(name));
  return it != m.end() ? it->second.get() : nullptr;
}

bool register_dynamic_node_type(std::string name, std::unique_ptr<NodeType> t) {
  for (NodeType const& b : builtin_node_types())
    if (b.name == name) return false;
  auto& m = imported_node_types();
  if (m.count(name)) return false;
  // 名字的稳定存储：泄漏一个 std::string（进程期，与内置表的生命周期同规）；
  // 这样 NodeType::name 的 string_view 永远有效（名字多为短串，避免 SSO 失效）。
  auto* stable = new std::string(std::move(name));
  t->name = *stable;
  m.emplace(*stable, std::move(t));
  return true;
}

std::string type_name_of(NodeIR const& n) {
  if (NodeType const* t = node_type_of(n.kind); t != nullptr) return std::string(t->name);
  if (n.raw.is_object())
    if (auto const it = n.raw.find("kind"); it != n.raw.end() && it->is_string())
      return it->get<std::string>();
  return {};
}

NodeType const* node_type_for_node(Json const& node) {
  // 1) 内置 matches（显式 kind / 历史写法）；命中>1 是"歧义即错"，不该落到动态名兜底
  std::vector<NodeType const*> hits;
  for (NodeType const& t : builtin_node_types())
    if (t.matches(node)) hits.push_back(&t);
  if (hits.size() == 1) return hits[0];
  if (!hits.empty()) return nullptr;  // 歧义

  // 2) 内置没命中 → 显式 kind 名匹配导入的动态节点（kind 就是它的名字，§5.3）
  if (node.contains("kind") && node["kind"].is_string())
    if (NodeType const* t = node_type_of_name(node["kind"].get<std::string>());
        t != nullptr && t->kind == NodeKind::Dynamic)
      return t;
  return nullptr;
}

}  // namespace cppchain::workflow
