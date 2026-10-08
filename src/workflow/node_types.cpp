// cppchain —— src/workflow/node_types.cpp
// 9 种内置节点类型的定义（解析 / 序列化 / 展示）。
// 这些代码原先散在 ir.cpp 的 parse_node / to_json 与 examples 的展示 switch 里；
// 集中到一处后，加一种节点只需在这里加一个 NodeType。
#include "cppchain/workflow/node_type.hpp"

#include <cctype>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "cppchain/agent/history.hpp"
#include "cppchain/message/content_block.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/message/utf8.hpp"
#include "cppchain/tool/tool_node.hpp"
#include "cppchain/workflow/expr.hpp"
#include "cppchain/workflow/ir_walk.hpp"
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

std::string as_string(Json const& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

// ---- wfr-㉙：运行期参数解析 ----
//
// 新增的参数都是"字面量或整串 @config 引用"的双态值，且**缺省与非法语义不同**：
//   缺省（未设 / 空 / @config 缺键解析成 null）→ 回落默认值（安全基线）；
//   非空但拼错 → 抛硬错（fail-closed：`--set history_mode=keep_evidnce` 不该被静默吞掉）。
// 注意 @config 缺键会经 resolve_ref 得到 Json(nullptr)，as_string 再 dump 成字符串 "null"
// —— 这正是"缺省"的识别依据（refs.hpp 的宽松语义）。

std::string resolve_text_parameter(std::string const& spec, Json const& state) {
  if (spec.empty()) return {};
  return as_string(resolve_parameter(Json(spec), state));
}

// 该参数是否写成了整串引用（是 → 编译期不判词表，留给运行期）。
bool is_whole_ref(std::string const& spec) { return parse_ref(spec).has_value(); }

model::ReasoningEcho parse_reasoning_echo(std::string const& text, std::string const& node_id) {
  if (text.empty() || text == "null") return model::ReasoningEcho::Drop;
  if (text == "drop") return model::ReasoningEcho::Drop;
  if (text == "tool_calls") return model::ReasoningEcho::ToolCalls;
  if (text == "always") return model::ReasoningEcho::Always;
  throw tool::ToolException("model 节点 '" + node_id + "' 的 reasoning_echo='" + text +
                            "' 不在词表里（drop / tool_calls / always）");
}

agent::HistoryMode parse_history_mode_arg(std::string const& text, std::string const& node_id) {
  if (text.empty() || text == "null") return agent::HistoryMode::Full;
  if (auto const mode = agent::parse_history_mode(text)) return *mode;
  throw tool::ToolException("load_history 节点 '" + node_id + "' 的 mode='" + text +
                            "' 不在词表里（full / drop_round / keep_evidence）");
}

std::size_t parse_history_budget(std::string const& text, std::string const& node_id) {
  if (text.empty() || text == "null") return 200;
  try {
    std::size_t pos = 0;
    long const v = std::stol(text, &pos);
    if (pos == text.size() && v > 0) return static_cast<std::size_t>(v);
  } catch (std::exception const&) {
    // 落到下面的统一报错
  }
  throw tool::ToolException("load_history 节点 '" + node_id + "' 的 budget='" + text +
                            "' 不是正整数");
}

// wfr-㉚：边的 forward 判定。`_continue` 是回边（指向 loop 节点头），其余都是前向。
// 只看向量本身 —— NodeType::edges() 只吃一个节点、拿不到作用域，也不需要。
NodeType::Edge edge_to(std::string target, std::string label) {
  bool const forward = !edge_is_backward(target);  // 必须在 move 之前算（moved-from 串是空的）
  return {std::move(target), std::move(label), forward};
}

// ---- 类型专属校验（VN5–VN11）----
// 通用图规则（目标存在/无环/可达）**不在这里**，由框架按 edges() 统一做（②）。

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

  // VN13（wfr-㉙）：params 原样并入请求体（build_request 的 extra 合并是**右胜**），
  // 所以必须挡住会覆盖请求体结构字段的键；reasoning_echo 为字面量时须在词表内。
  if (!n.model_params.is_object()) {
    err("VN13", "model 节点 '" + n.id + "' 的 params 必须是对象");
  } else {
    static constexpr std::string_view kReserved[] = {
        "model",       "messages",     "stream",     "stream_options", "tools",
        "tool_choice", "response_format", "temperature", "max_tokens",  "stop"};
    for (auto it = n.model_params.begin(); it != n.model_params.end(); ++it) {
      if (it.key().empty()) {
        err("VN13", "model 节点 '" + n.id + "' 的 params 含空键");
        continue;
      }
      for (std::string_view const r : kReserved)
        if (it.key() == r)
          err("VN13", "model 节点 '" + n.id + "' 的 params 含保留键 '" + it.key() +
                          "'（会覆盖请求体结构字段；请用节点自身字段设置）");
    }
  }
  if (!n.reasoning_echo.empty() && !is_whole_ref(n.reasoning_echo) &&
      n.reasoning_echo != "drop" && n.reasoning_echo != "tool_calls" &&
      n.reasoning_echo != "always")
    err("VN13", "model 节点 '" + n.id + "' 的 reasoning_echo='" + n.reasoning_echo +
                    "' 不在词表里（drop / tool_calls / always）");
}

void v_echo(NodeIR const& n, NodeType::ValidateContext const&, NodeType::ErrorSink const& err) {
  if (n.next.empty()) err("VN12", "echo 节点 '" + n.id + "' 缺少 next");
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

// ---- wfr-㉚：loop 容器 ----

void v_loop(NodeIR const& n, NodeType::ValidateContext const&, NodeType::ErrorSink const& err) {
  if (n.next.empty()) err("VN15", "loop 节点 '" + n.id + "' 缺少 next（循环出口）");
  if (n.max <= 0)
    err("VN15", "loop 节点 '" + n.id + "' 的 max 必须是正整数（body 的最大执行次数）");
  if (n.body.empty())
    err("VN15", "loop 节点 '" + n.id + "' 的 body 不能为空（空循环没有意义）");
  if (n.next == kContinueName || n.next == kBreakName)
    err("VN15", "loop 节点 '" + n.id + "' 的 next 不能是保留名 " + n.next +
                    "（出口要么是真节点、要么是 _turn_end / _fail）");
  if (!n.on_max.empty() && (n.on_max == kContinueName || n.on_max == kBreakName))
    err("VN15", "loop 节点 '" + n.id + "' 的 on_max 不能是保留名 " + n.on_max);

  // VG11：不支持 loop 套 loop（本层直接可见就能判，不必等全图遍历）
  for (NodeIR const& sub : n.body)
    if (sub.kind == NodeKind::Loop)
      err("VG11", "loop 节点 '" + n.id + "' 的 body 里又有 loop 节点 '" + sub.id +
                      "'（不支持嵌套循环）");

  // VG13：load_history 不得放进循环体 —— 每轮 replay 都会把整段历史再追加一遍，
  // messages 会随迭代平方级膨胀。沿用 VG9 的"硬拒绝，读了没人用就是错"风格。
  for (NodeIR const& sub : n.body)
    if (sub.kind == NodeKind::LoadHistory)
      err("VG13", "loop 节点 '" + n.id + "' 的 body 里有 load_history '" + sub.id +
                      "'（循环体里读历史会让 messages 随迭代反复膨胀）");
}

std::vector<NodeType::Edge> e_loop(NodeIR const& n) {
  std::vector<NodeType::Edge> out;
  if (!n.body.empty()) out.push_back(edge_to(n.body.front().id, "body"));
  if (!n.next.empty()) out.push_back(edge_to(n.next, "next"));
  // on_max 缺省时指向 _turn_end（也是可达性判定要看到的出口边）
  out.push_back(edge_to(n.on_max.empty() ? std::string(kTurnEndName) : n.on_max, "on_max"));
  return out;
}

// loop 节点头 = 「计数 + 分支」。与 c_goto 同一手法：只注册节点、不加静态边，
// 路由完全由 Command.goto_nodes 给出（graph 里它的优先级最高）。
void c_loop(NodeIR const& n, NodeType::CompileContext& ctx) {
  if (n.body.empty()) {
    ctx.error = "loop 节点 '" + n.id + "' 的 body 为空";
    return;
  }
  std::string const loop_id = n.id;
  std::string const body_entry = n.body.front().id;
  std::string const break_target =
      n.next.empty() ? std::string(graph::kEnd) : ctx.resolve(n.next);
  std::string const max_target =
      n.on_max.empty() ? std::string(graph::kEnd) : ctx.resolve(n.on_max);
  int const max = n.max;
  ctx.graph.add_node(n.id, [loop_id, body_entry, break_target, max_target, max](Json const& state,
                                                                               NodeContext&) {
    long cur = 0;
    if (state.contains(kLoopCountsKey))
      if (auto const& lc = state.at(kLoopCountsKey);
          lc.is_object() && lc.contains(loop_id) && lc[loop_id].is_number())
        cur = lc[loop_id].get<long>();
    Command c;
    // **先判后加**：cur 是"已完成的轮数"。超限走 on_max（缺省 _turn_end）——
    // 不报错，与旧 goto 超 max 跳 kEnd 的行为一致。
    if (cur >= max) {
      c.goto_nodes = {max_target};
      return c;
    }
    c.update = Json{{kLoopCountsKey, Json{{loop_id, cur + 1}}}};
    c.goto_nodes = {body_entry};
    return c;
  });
  ++ctx.registered;
}

void v_load_history(NodeIR const& n, NodeType::ValidateContext const&,
                    NodeType::ErrorSink const& err) {
  if (n.path.empty()) err("VN11", "load_history 节点 '" + n.id + "' 缺少 path（工作路径）");
  if (n.next.empty()) err("VN11", "load_history 节点 '" + n.id + "' 缺少 next");

  // VN14（wfr-㉙）：历史渲染策略。字面量须在词表内；写成整串 @config 引用则留给运行期判
  // （运行期缺省→full、非法→硬错，见 parse_history_mode_arg）。
  if (!n.history_mode.empty() && !is_whole_ref(n.history_mode) &&
      !agent::parse_history_mode(n.history_mode))
    err("VN14", "load_history 节点 '" + n.id + "' 的 mode='" + n.history_mode +
                    "' 不在词表里（full / drop_round / keep_evidence）");
  if (!n.history_budget.empty() && !is_whole_ref(n.history_budget)) {
    std::size_t pos = 0;
    long v = 0;
    try {
      v = std::stol(n.history_budget, &pos);
    } catch (std::exception const&) {
      pos = 0;
    }
    if (pos != n.history_budget.size() || v <= 0)
      err("VN14", "load_history 节点 '" + n.id + "' 的 budget='" + n.history_budget +
                      "' 不是正整数");
  }
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

// ---- 出边（通用图规则的唯一输入）----

std::vector<NodeType::Edge> e_tool(NodeIR const& n) {
  std::vector<NodeType::Edge> out{edge_to(n.next, "next")};
  // on_error.fallback 也是一条**前向边**：它必须是存在的节点，且算一条可达路径。
  // 把它渲染成边之后，VG7 的可达性判定就与节点类型完全无关了（原先要特判 tool）。
  if (n.on_error && n.on_error->fallback)
    out.push_back(edge_to(*n.on_error->fallback, "on_error.fallback"));
  return out;
}
std::vector<NodeType::Edge> e_expr(NodeIR const& n) {
  return {edge_to(n.then, "then"), edge_to(n.else_, "else")};
}
std::vector<NodeType::Edge> e_next(NodeIR const& n) { return {edge_to(n.next, "next")}; }
std::vector<NodeType::Edge> e_route(NodeIR const& n) {
  std::vector<NodeType::Edge> out;
  for (auto const& [k, v] : n.cases) out.push_back(edge_to(v, "cases." + k));
  if (!n.default_.empty()) out.push_back(edge_to(n.default_, "default"));
  return out;
}

// ---- 引用文本片段（VR1 语法检查 + VR 段引用收集共用）----

std::vector<std::string> t_none(NodeIR const&) { return {}; }

// 递归收集 JSON 里所有字符串叶（VR1 语法检查 + @config 契约检查的输入）。
void collect_strings(Json const& v, std::vector<std::string>& out) {
  if (v.is_string()) {
    out.push_back(v.get<std::string>());
  } else if (v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it) collect_strings(it.value(), out);
  } else if (v.is_array()) {
    for (Json const& e : v) collect_strings(e, out);
  }
}

// echo 的 text 模板作为引用文本（VR1 语法检查 + VR2 校验内嵌 $/@config 引用是否合法）
std::vector<std::string> t_echo(NodeIR const& n) {
  if (n.raw.is_object() && n.raw.contains("text") && n.raw["text"].is_string())
    return {n.raw["text"].get<std::string>()};
  return {};
}

std::vector<std::string> t_tool(NodeIR const& n) {
  std::vector<std::string> out;
  collect_strings(n.parameters, out);
  return out;
}

// wfr-㉙：model 的 params（厂商私有参数）与 reasoning_echo 也是引用文本。
// 少了这个钩子，新字段里的 @config.* 既不过 VR1 语法检查、也不参与 VR4 契约核对。
std::vector<std::string> t_model(NodeIR const& n) {
  std::vector<std::string> out;
  collect_strings(n.model_params, out);
  if (!n.reasoning_echo.empty()) out.push_back(n.reasoning_echo);
  return out;
}

std::vector<std::string> t_expr(NodeIR const& n) { return {n.expr}; }
std::vector<std::string> t_history(NodeIR const& n) {
  std::vector<std::string> out{n.path};
  if (!n.session.empty()) out.push_back(n.session);
  // wfr-㉙：mode/budget 同样是运行期参数（可写 @config.*）
  if (!n.history_mode.empty()) out.push_back(n.history_mode);
  if (!n.history_budget.empty()) out.push_back(n.history_budget);
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

void c_echo(NodeIR const& n, NodeType::CompileContext& ctx) {
  Json const text_tpl =
      (n.raw.is_object() && n.raw.contains("text")) ? n.raw["text"] : Json("");
  auto sink_p = ctx.sink;
  ctx.graph.add_node(n.id, [sink_p, text_tpl](Json const& state, NodeContext& nc) {
    Json const r = resolve_parameter(text_tpl, state);
    std::string const msg =
        r.is_string() ? r.get<std::string>() : message::utf8_safe_dump(r);  // 对象/数组安全渲染
    if (!msg.empty()) std::println("{}", msg);
    if (sink_p) sink_p->log_node(nc.node_name, "info", msg.empty() ? "(echo)" : msg);
    return Command{};
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
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
  Json const params = n.model_params;          // wfr-㉙：运行期解析后并入 CallOptions.extra
  std::string const echo_spec = n.reasoning_echo;
  auto sink_p = ctx.sink;  // 触发期注入的 NodeLog（docs/12 §6.4：source=node 的诊断日志）

  ctx.graph.add_node(n.id, [m, tools_json, sys, model_name, params, echo_spec, sink_p,
                            node_id = n.id](Json const& state, NodeContext& nc) {
    std::vector<message::Message> msgs = messages_from_state(state);
    if (!sys.empty()) msgs.insert(msgs.begin(), message::system_message(sys));
    model::CallOptions co;
    co.tools = tools_json;
    // 厂商私有参数：逐键解析引用；解析为 null（@config 缺键）的**顶层键直接丢弃** ——
    // 不把 "reasoning_effort": null 发给端点（那会被当成显式设值）。
    for (auto it = params.begin(); it != params.end(); ++it) {
      Json v = resolve_parameter(it.value(), state);
      if (v.is_null()) continue;
      co.extra[it.key()] = std::move(v);
    }
    std::string const echo_text = resolve_text_parameter(echo_spec, state);
    co.reasoning_echo = parse_reasoning_echo(echo_text, node_id);
    auto r = m->invoke(msgs, co);
    if (!r) throw tool::ToolException("model 节点调用失败：" + r.error().message);
    // **只发一次**：回复追加进 messages，循环由 route + 回边负责
    sink_p->log_node(nc.node_name, "info", "model '" + model_name + "' 完成一次调用",
                     Json{{"in_msg", msgs.size()},
                          {"tool_calls", !r->tool_calls.empty()},
                          {"extra_keys", co.extra.size()},
                          {"reasoning_echo", echo_text.empty() ? "drop" : echo_text}});
    // 发布可引用 result（echo 节点 $引用打印）：thinking / 正文 / 工具调用摘要
    std::string thinking;
    for (auto const& blk : r->content)
      if (auto const* rb = std::get_if<message::ReasoningBlock>(&blk)) thinking += rb->reasoning;
    Json tcalls = Json::array();
    for (auto const& t : r->tool_calls) {
      Json call;
      call["name"] = t.name;
      call["args"] = t.args;
      tcalls.push_back(std::move(call));
    }
    Json frame;
    frame["thinking"] = std::move(thinking);
    frame["text"] = r->text();
    frame["tool_calls"] = std::move(tcalls);
    frame["extra"] = co.extra;  // 生效的厂商参数（调试台/echo 可见）
    Json results = Json::object();
    results[node_id] = std::move(frame);
    Json update = Json::object();
    update[kMessagesKey] = Json::array({*r});
    update[kResultsKey] = std::move(results);
    return Command::with_update(std::move(update));
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

void c_route(NodeIR const& n, NodeType::CompileContext& ctx) {
  std::map<std::string, std::string> targets;
  for (auto const& [k, v] : n.cases) targets[k] = ctx.resolve(v);
  std::string const dflt = ctx.resolve(n.default_);
  std::string const on = n.on;
  std::string const node_id = n.id;
  // 诊断/结果里发布的是**作者写的原始目标名**（_turn_end / act / _continue …）——
  // 与 workflow JSON、图上的边标签一致，比解析后的图目标名（__end__）可读得多。
  std::map<std::string, std::string> raw_targets = n.cases;
  std::string const raw_dflt = n.default_;
  auto sink_p = ctx.sink;

  // 节点体：把"命中哪一档 → 去哪"算出来并**发布进 results**。两个好处：
  //   · 会话日志的 exit update 里就带上了这次路由的决定（调试台点 route 节点直接看见）
  //   · 下游/echo 可以用 $<route id>.result.target 引用它
  // 同时写一条 source=node 诊断，让日志里有一行**人可读**的"走了哪条分支"。
  ctx.graph.add_node(n.id, [node_id, on, raw_targets, raw_dflt, sink_p](Json const& state,
                                                                       NodeContext& nc) {
    std::string const outcome = last_ai_outcome(state);
    auto const it = raw_targets.find(outcome);
    bool const matched = (it != raw_targets.end());
    std::string const target = matched ? it->second : raw_dflt;
    sink_p->log_node(nc.node_name, "info",
                     "route: " + on + " = " + (outcome.empty() ? "（无 AI 消息）" : outcome) +
                         (matched ? "  → " : "（未命中，走 default）→ ") + target,
                     Json{{"on", on}, {"outcome", outcome}, {"target", target},
                          {"matched", matched}});
    Json frame;
    frame["on"] = on;
    frame["outcome"] = outcome;
    frame["target"] = target;
    frame["matched"] = matched;
    Json results = Json::object();
    results[node_id] = std::move(frame);
    return Command::with_update(Json{{kResultsKey, std::move(results)}});
  });
  // router：直接取节点体算好的决定（它在 update 合并**之后**才跑，所以取得到）。
  // 取不到时兜底重算一遍，行为与原先一致。
  ctx.graph.add_conditional_edges(n.id, [node_id, targets, dflt](Json const& state) {
    // 节点体发布的 target 是**原始名**，这里要的是图目标名 → 用解析后的表按"同一个 outcome"
    // 再查一次（而不是直接拿原始名去当节点 id）。
    auto const raw_it = state.find(kResultsKey);
    if (raw_it != state.end() && raw_it->is_object())
      if (auto const f = raw_it->find(node_id); f != raw_it->end() && f->is_object())
        if (auto const oc = f->find("outcome"); oc != f->end() && oc->is_string()) {
          auto const t = targets.find(oc->get<std::string>());
          return std::vector<std::string>{t != targets.end() ? t->second : dflt};
        }
    std::string const outcome = last_ai_outcome(state);
    auto const it = targets.find(outcome);
    return std::vector<std::string>{it != targets.end() ? it->second : dflt};
  });
  ++ctx.registered;
}

void c_run_tools(NodeIR const& n, NodeType::CompileContext& ctx) {
  tool::ToolNode tn(ctx.registry);  // 复用 M3 的实现（并行、顺序对齐、错误回喂）
  ctx.graph.add_node(n.id, [tn, node_id = n.id](Json const& state, NodeContext&) {
    std::vector<message::ToolCall> calls;
    auto const msgs = messages_from_state(state);
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
      if (it->type == message::Role::Ai) {
        calls = it->tool_calls;
        break;
      }
    if (calls.empty())
      throw tool::ToolException("run_tools 节点：最后一条 AI 消息没有 tool_calls");
    std::vector<message::Message> tool_msgs = tn.run(calls);
    Json arr = Json::array();
    for (message::Message& m : tool_msgs) {
      // 工具输出（含 CJK/截断字节）可能非法 UTF-8 → 净化后再进 state，避免 dump 崩溃
      for (auto& blk : m.content)
        if (auto* tb = std::get_if<message::TextBlock>(&blk)) message::sanitize_utf8(tb->text);
      arr.push_back(m);
    }
    // 发布可引用 result（echo 节点 $引用打印）：每个工具调用的 名字(参数) → 结果截断
    std::unordered_map<std::string, message::ToolCall const*> by_id;
    for (auto const& tc : calls) by_id[tc.id] = &tc;
    Json summary = Json::array();
    for (message::Message const& m : tool_msgs) {
      std::string name = "tool", args;
      if (m.tool_call_id)
        if (auto const it = by_id.find(*m.tool_call_id); it != by_id.end()) {
          name = it->second->name;
          args = message::utf8_safe_dump(it->second->args);  // 模型给的参数也可能带坏字节
        }
      std::string text = m.text();
      if (text.size() > 140) text = text.substr(0, 140) + "…";
      std::string line = name + (args.empty() ? "" : "(" + args + ")") + " → " + text;
      message::sanitize_utf8(line);  // 逐行兜底：任何一段带坏字节都整行净化
      summary.push_back(std::move(line));
    }
    Json frame;
    frame["summary"] = std::move(summary);
    Json results = Json::object();
    results[node_id] = std::move(frame);
    Json update = Json::object();
    update[kMessagesKey] = std::move(arr);
    update[kResultsKey] = std::move(results);
    return Command::with_update(std::move(update));
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

void c_load_history(NodeIR const& n, NodeType::CompileContext& ctx) {
  Json const path_expr = Json(n.path);
  Json const sess_expr = Json(n.session);
  std::string const mode_spec = n.history_mode;
  std::string const budget_spec = n.history_budget;
  auto sink_p = ctx.sink;
  // ④：history = 日志的投影 —— 读本会话日志 → replay → 取 messages（docs/12 §7）。
  // 与 D 阶段的"读 history 文件"差别：文件是 append-only 日志，宿主不再单独 save；
  // 本轮输入也已由 fire_once 写成 input 事件排在日志里，replay 天然含它。
  // wfr-㉙：拿到 messages 后按 mode 渲染（纯变换，无 IO）。压缩结果**不落盘** ——
  // 本节点的 update 是"回声"，编译器已把它从日志里摘掉（compiler.cpp 的两处 kind 判断）。
  ctx.graph.add_node(n.id, [path_expr, sess_expr, mode_spec, budget_spec, sink_p, node_id = n.id](
                              Json const& state, NodeContext& nc) {
    std::string const ws = as_string(resolve_parameter(path_expr, state));
    std::string const sess = as_string(resolve_parameter(sess_expr, state));
    std::string const log_path = session_path(ws, sess);
    std::vector<message::Message> hist = replay_session_messages(log_path);

    std::string const mode_text = resolve_text_parameter(mode_spec, state);
    agent::HistoryMode const mode = parse_history_mode_arg(mode_text, node_id);
    std::size_t const budget = parse_history_budget(resolve_text_parameter(budget_spec, state),
                                                   node_id);
    agent::HistoryStats const stats = agent::compress_history(hist, mode, budget, log_path);

    sink_p->log_node(nc.node_name, "info",
                     "load_history: replay " + std::to_string(stats.before_msgs) + " → " +
                         std::to_string(stats.after_msgs) + " messages（" +
                         std::string(agent::history_mode_name(mode)) + "）",
                     agent::history_stats_json(stats));
    Json arr = Json::array();
    for (message::Message const& m : hist) arr.push_back(m);
    // 走 messages 通道 → add_messages 与本次输入合并（同 id 覆盖、无 id 追加）
    Json results = Json::object();
    results[node_id] = agent::history_stats_json(stats);
    return Command::with_update(Json{{kMessagesKey, std::move(arr)},
                                     {kResultsKey, std::move(results)}});
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

      // ---------------- loop（wfr-㉚，替代 label + goto）----------------
      // 循环容器：body 是子树；`_continue` 回节点头（回边）、`_break` 去 next。
      // 判定只认显式 kind（不拿 body 当决定性键 —— 动态节点也可能自带 body 字段）。
      {NodeKind::Loop, "loop",
       [](Json const& n) { return kind_is(n, "loop"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "next", out.next);
         get_str(out, node, "on_max", out.on_max);
         if (node.contains("max") && node["max"].is_number_integer())
           out.max = node["max"].get<int>();
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "loop";
         node["max"] = n.max;
         node["next"] = n.next;
         if (!n.on_max.empty()) node["on_max"] = n.on_max;
         // body 由 ir.cpp 的 write_node_list 递归写（结构性字段）
       },
       [](NodeIR const& n) {
         return "loop ×" + std::to_string(n.max) + " (" + std::to_string(n.body.size()) +
                " nodes) break→" + n.next + " max→" +
                (n.on_max.empty() ? std::string(kTurnEndName) : n.on_max);
       }, v_loop, c_loop, e_loop, t_none},

      // ---------------- model（M12）----------------
      {NodeKind::Model, "model",
       [](Json const& n) { return kind_is(n, "model"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "model", out.model);
         get_str(out, node, "tools", out.toolset);  // 工具集**组件名**
         get_str(out, node, "system_prompt", out.system_prompt);
         get_str(out, node, "next", out.next);
         get_str(out, node, "reasoning_echo", out.reasoning_echo);
         // 原样收下（含非对象）：让 VN13 报错，而不是这里静默丢掉。
         if (node.contains("params") && !node["params"].is_null()) out.model_params = node["params"];
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "model";
         node["model"] = n.model;
         if (!n.toolset.empty()) node["tools"] = n.toolset;
         if (!n.system_prompt.empty()) node["system_prompt"] = n.system_prompt;
         if (!n.model_params.empty()) node["params"] = n.model_params;
         if (!n.reasoning_echo.empty()) node["reasoning_echo"] = n.reasoning_echo;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         std::string desc = "model " + n.model + " (tools=" + (n.toolset.empty() ? "-" : n.toolset);
         if (!n.model_params.empty()) desc += " params=" + std::to_string(n.model_params.size());
         desc += ")";
         if (!n.reasoning_echo.empty()) desc += " echo=" + n.reasoning_echo;
         return desc + " → " + n.next;
       }, v_model, c_model, e_next, t_model},

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
         get_str(out, node, "mode", out.history_mode);
         get_str(out, node, "budget", out.history_budget);
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "load_history";
         node["path"] = n.path;
         if (!n.session.empty()) node["session"] = n.session;
         if (!n.history_mode.empty()) node["mode"] = n.history_mode;
         if (!n.history_budget.empty()) node["budget"] = n.history_budget;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         std::string desc = "load_history path=" + n.path +
                            " session=" + (n.session.empty() ? "-" : n.session);
         if (!n.history_mode.empty()) desc += " mode=" + n.history_mode;
         return desc + " → " + n.next;
       }, v_load_history, c_load_history, e_next, t_history},
      // ---------------- echo（wfr-㉗）----------------
      // 回显节点：text 模板（$节点.result.字段 / @config.*）解析后打印到终端并落一条
      // 节点诊断；不改 state，纯粹"回显一步"。
      {NodeKind::Echo, "echo",
       [](Json const& n) { return kind_is(n, "echo"); },
       [](NodeIR& out, Json const& node) { get_str(out, node, "next", out.next); },
       [](Json& node, NodeIR const& n) { node["kind"] = "echo"; node["next"] = n.next; },
       [](NodeIR const& n) {
         std::string text =
             n.raw.is_object() && n.raw.contains("text") && n.raw["text"].is_string()
                 ? n.raw["text"].get<std::string>()
                 : "";
         if (text.size() > 34) text = text.substr(0, 34) + "…";
         return (text.empty() ? "echo" : text) + " → " + n.next;
       }, v_echo, c_echo, e_next, t_echo},
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
