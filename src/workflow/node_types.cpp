// cppchain —— src/workflow/node_types.cpp
// 9 种内置节点类型的定义（解析 / 序列化 / 展示）。
// 这些代码原先散在 ir.cpp 的 parse_node / to_json 与 examples 的展示 switch 里；
// 集中到一处后，加一种节点只需在这里加一个 NodeType。
#include "cppchain/workflow/node_type.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <unistd.h>

#include "cppchain/agent/agent.hpp"
#include "cppchain/agent/history.hpp"
#include "cppchain/agent/tokens.hpp"
#include "cppchain/message/content_block.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/message/utf8.hpp"
#include "cppchain/tool/probe.hpp"
#include "cppchain/tool/command_classify.hpp"  // wfr-㊸ ⑤：allow 前缀按类别封顶
#include "cppchain/tool/exec_guard.hpp"
#include "cppchain/tool/tool_node.hpp"
#include "cppchain/tool/tool_plugin.hpp"  // wfr-㊹：动态工具描述符/loader（tool_registry 用）
#include "cppchain/workflow/expr.hpp"
#include "cppchain/workflow/ir_walk.hpp"
#include "cppchain/workflow/compiler.hpp"   // B2：next_artifact_version（产物版本单调源）
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "cppchain/workflow/validator.hpp"

namespace cppchain::workflow {

using cppchain::graph::Command;
using cppchain::graph::NodeContext;
using cppchain::graph::interrupt;

namespace {

// B2（problems/39）：写产物工具成功登记版本的 helper（定义在后，c_tool/c_run_tools 先用）。
void b2_register_artifact(Json& update, Json const& call_args, bool ok);
std::int64_t b2_artifact_version(Json const& state, std::string const& relpath);


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

// ---- wfr-㊵：历史处理节点的参数（"字面量或整串 @config 引用"的双态）----
//
// 与 wfr-㉙ 的 mode/budget 同一套差异化语义：**缺省回落默认值、非空但拼错抛硬错**
//（fail-closed：`--set sum_style=evidnce` 这种拼错不该被静默吞成默认档）。

std::string resolve_enum_param(std::string const& spec, Json const& state,
                               std::string const& node_id, std::string_view what,
                               std::initializer_list<std::string_view> allowed,
                               std::string const& fallback) {
  std::string const t = resolve_text_parameter(spec, state);
  if (t.empty() || t == "null") return fallback;
  std::string avail;
  for (std::string_view const a : allowed) {
    if (t == a) return t;
    if (!avail.empty()) avail += " / ";
    avail += std::string(a);
  }
  throw tool::ToolException(node_id + " 的 " + std::string(what) + "='" + t +
                            "' 不在词表里（" + avail + "）");
}

std::size_t resolve_size_param(std::string const& spec, Json const& state,
                               std::string const& node_id, std::string_view what,
                               std::size_t fallback) {
  std::string const t = resolve_text_parameter(spec, state);
  if (t.empty() || t == "null") return fallback;
  try {
    std::size_t pos = 0;
    long const v = std::stol(t, &pos);
    if (pos == t.size() && v > 0) return static_cast<std::size_t>(v);
  } catch (std::exception const&) {
  }
  throw tool::ToolException(node_id + " 的 " + std::string(what) + "='" + t + "' 不是正整数");
}

// 把待压缩的历史渲染成提示词里的文本（摘要节点用）
std::string render_history_for_summary(std::vector<message::Message> const& msgs) {
  std::string out;
  for (message::Message const& m : msgs) {
    char const* role = m.type == message::Role::Human    ? "用户"
                       : m.type == message::Role::Ai     ? "助手"
                       : m.type == message::Role::Tool   ? "工具"
                       : m.type == message::Role::System ? "系统"
                                                         : "其它";
    out += "[" + std::string(role) + "] " + m.text();
    if (m.type == message::Role::Tool && m.status != "success") out += "（失败：" + m.status + "）";
    out += "\n";
  }
  return out;
}

// 提示词风格三档 —— 这是"历史摘要"这个方向的核心自变量
std::string summary_instruction(std::string_view style) {
  std::string s =
      "把下面这段多轮对话历史压缩成要点，供后续轮次继续使用。只写历史里确凿出现过的事实，"
      "不要编造，也不要补充建议。";
  if (style == "detailed")
    s += "保留关键数值与结论（含工具给出的数值）及其出处。";
  else if (style == "evidence")
    s += "另外显式记录：调用过哪些工具、参数是什么、结果的关键结论 —— 后续回答需要用它们"
         "证明自己不是凭空说的。";
  else
    s += "按「用户目标 / 已确认事实 / 未决问题」三段写，尽量短。";
  return s;
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

// ---- wfr-㊷：approve_tools（副作用审批，见 02-arch-design/18） ----

void v_approve_tools(NodeIR const& n, NodeType::ValidateContext const& ctx,
                     NodeType::ErrorSink const& err) {
  if (n.next.empty()) err("VN17", "approve_tools 节点 '" + n.id + "' 缺少 next");

  // deny 名单：字面量必须在注册表里且**不能是只读工具**（deny 免审工具=死配置）；
  // 整串 @config 引用留给运行期（与 RefineHistory 对 categories 的手法一致）。
  std::vector<std::string> seen;
  for (std::string const& d : n.deny) {
    if (d.empty()) {
      err("VN17", "approve_tools 节点 '" + n.id + "' 的 deny 含空工具名");
      continue;
    }
    if (std::find(seen.begin(), seen.end(), d) != seen.end()) {
      err("VN17", "approve_tools 节点 '" + n.id + "' 的 deny 重复：'" + d + "'");
      continue;
    }
    seen.push_back(d);
    if (is_whole_ref(d)) continue;  // 运行期解析
    if (ctx.registry == nullptr) continue;  // 无注册表时不做存在性检查（如纯编译测试）
    tool::ToolSpec const* spec = ctx.registry->find(d);
    if (spec == nullptr)
      err("VN17", "approve_tools 节点 '" + n.id + "' 的 deny 引用目录里不存在的工具 '" + d +
                      "'");
    else if (spec->read_only)
      err("VN17", "approve_tools 节点 '" + n.id + "' 的 deny 含只读工具 '" + d +
                      "'（只读工具本来就免审，拒绝它是死配置）");
  }
}

// ref_texts：deny 元素也可能是 @config 引用，要让 VR1/VR4 的引用收集覆盖它们
std::vector<std::string> t_approve_tools(NodeIR const& n) {
  std::vector<std::string> out;
  for (std::string const& d : n.deny) out.push_back(d);
  return out;
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

void v_summarize(NodeIR const& n, NodeType::ValidateContext const& ctx,
                 NodeType::ErrorSink const& err) {
  if (n.model.empty())
    err("VN15", "summarize 节点 '" + n.id + "' 缺少 model（组件名）");
  else if (ctx.components != nullptr && ctx.components->model(n.model) == nullptr)
    err("VN15", "summarize 节点 '" + n.id + "' 引用的模型组件 '" + n.model + "' 不在注册表里");
  if (n.next.empty()) err("VN15", "summarize 节点 '" + n.id + "' 缺少 next");
  struct Lit {
    std::string const* v;
    char const* what;
    std::initializer_list<std::string_view> allowed;
  };
  for (Lit const l : {Lit{&n.when, "when", {"off", "messages", "tokens", "always"}},
                      Lit{&n.style, "style", {"concise", "detailed", "evidence"}},
                      Lit{&n.summary_role, "summary_role", {"system", "human", "ai"}},
                      Lit{&n.on_fail, "on_fail", {"skip", "placeholder"}}}) {
    if (l.v->empty() || is_whole_ref(*l.v)) continue;  // 引用留给运行期判
    bool ok = false;
    std::string avail;
    for (std::string_view const a : l.allowed) {
      if (*l.v == a) ok = true;
      if (!avail.empty()) avail += " / ";
      avail += std::string(a);
    }
    if (!ok)
      err("VN15", "summarize 节点 '" + n.id + "' 的 " + l.what + "='" + *l.v +
                      "' 不在词表里（" + avail + "）");
  }
  for (std::string const* f : {&n.max_msgs, &n.max_tokens, &n.keep_last}) {
    if (f->empty() || is_whole_ref(*f)) continue;
    std::size_t pos = 0;
    long v = 0;
    try {
      v = std::stol(*f, &pos);
    } catch (std::exception const&) {
      pos = 0;
    }
    if (pos != f->size() || v <= 0)
      err("VN15", "summarize 节点 '" + n.id + "' 的阈值/条数必须是正整数，收到 '" + *f + "'");
  }
}

void v_refine_history(NodeIR const& n, NodeType::ValidateContext const&,
                      NodeType::ErrorSink const& err) {
  if (n.next.empty()) err("VN16", "refine_history 节点 '" + n.id + "' 缺少 next");
  if (!n.preset.empty() && !agent::parse_history_preset(n.preset) && !is_whole_ref(n.preset))
    err("VN16", "refine_history 节点 '" + n.id + "' 的 preset='" + n.preset +
                    "' 不在词表里（minimal / lean / aggressive / clip）");
  if (!n.clip_scope.empty() && !is_whole_ref(n.clip_scope) && n.clip_scope != "tool_results" &&
      n.clip_scope != "ai_text" && n.clip_scope != "both")
    err("VN16", "refine_history 节点 '" + n.id + "' 的 clip_scope='" + n.clip_scope +
                    "' 不在词表里（tool_results / ai_text / both）");
  if (!n.clip_mode.empty() && !is_whole_ref(n.clip_mode) && n.clip_mode != "truncate" &&
      n.clip_mode != "artifact")
    err("VN16", "refine_history 节点 '" + n.id + "' 的 clip_mode='" + n.clip_mode +
                    "' 不在词表里（truncate / artifact）");
  for (std::string const& r : n.rules)
    if (!is_whole_ref(r) && !agent::is_known_history_rule(r))
      err("VN16", "refine_history 节点 '" + n.id + "' 的规则 '" + r + "' 不认识");
  // 类别：形状在 agent 层校，**词表在 tool 层**校（分层：agent 不 include tool 头）
  for (std::string const& r : n.rules)
    if (auto const c = agent::history_rule_category(r); c && !tool::is_known_category(*c))
      err("VN16", "refine_history 节点 '" + n.id + "' 的规则 '" + r +
                      "' 里的类别不在词表里（见 tool::kToolCategories）");
  for (std::string const* f : {&n.tool_budget, &n.ai_budget}) {
    if (f->empty() || is_whole_ref(*f)) continue;
    std::size_t pos = 0;
    long v = 0;
    try {
      v = std::stol(*f, &pos);
    } catch (std::exception const&) {
      pos = 0;
    }
    if (pos != f->size() || v <= 0)
      err("VN16", "refine_history 节点 '" + n.id + "' 的预算必须是正整数，收到 '" + *f + "'");
  }
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

// ---- wfr-㊹ 治理（problems/34）：活动工具集 ----
// tool_registry 节点在 workflow 起始处发布 `results.<id>.active_tools`，think/run 从此取。
// 扫 state["results"] 全帧收集该字段求并集（保序去重）；没有任何帧带它 → nullopt
//（走既有 toolset 组件 / 编译期快照路径 = 零回归分支）。
struct ActiveTools {
  std::vector<std::string> order;        // 保序去重活动名（model schema 顺序）
  std::unordered_set<std::string> set;   // 名称集合（run_tools 允许名单查重）
  [[nodiscard]] bool empty() const { return order.empty(); }
};
std::optional<ActiveTools> scan_active_tools(Json const& state) {
  auto const res = state.find(kResultsKey);
  if (res == state.end() || !res->is_object()) return std::nullopt;
  ActiveTools out;
  for (auto const& [k, v] : res->items()) {
    if (!v.is_object()) continue;
    auto const it = v.find("active_tools");
    if (it == v.end() || !it->is_array()) continue;
    for (auto const& name : *it)
      if (name.is_string()) {
        std::string const s = name.get<std::string>();
        if (out.set.insert(s).second) out.order.push_back(s);
      }
  }
  return out.set.empty() ? std::nullopt : std::optional<ActiveTools>(std::move(out));
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
// wfr-㊾ 批③：executor 与 e_next 同语义（静态边 next 回 parallel）；parallel 出边 =
// target（Send 目标，VG1 校验其存在）+ _break（全 done 跳循环出口，VG10 在 loop 内合法）。
std::vector<NodeType::Edge> e_executor(NodeIR const& n) { return {edge_to(n.next, "next")}; }
std::vector<NodeType::Edge> e_parallel(NodeIR const& n) {
  std::vector<NodeType::Edge> out;
  if (!n.target.empty()) out.push_back(edge_to(n.target, "target"));
  out.push_back(edge_to(kBreakName, "next"));
  return out;
}
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

// wfr-㊵：summarize 的运行期参数（may be @config.*）也要过 VR1 语法检查与 VR4 契约核对
std::vector<std::string> t_summarize(NodeIR const& n) {
  std::vector<std::string> out;
  for (std::string const* f : {&n.when, &n.max_msgs, &n.max_tokens, &n.keep_last, &n.style,
                               &n.summary_role, &n.on_fail})
    if (!f->empty()) out.push_back(*f);
  collect_strings(n.model_params, out);
  if (!n.reasoning_echo.empty()) out.push_back(n.reasoning_echo);
  return out;
}

std::vector<std::string> t_refine_history(NodeIR const& n) {
  std::vector<std::string> out;
  for (std::string const* f : {&n.preset, &n.clip_scope, &n.clip_mode, &n.tool_budget,
                               &n.ai_budget, &n.artifact_dir})
    if (!f->empty()) out.push_back(*f);
  for (std::string const& r : n.rules) out.push_back(r);
  for (std::string const& c : n.categories) out.push_back(c);
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
  tool::ExecutionGuard const* guard = ctx.guard;  // wfr-㊷：副作用防护（null = 不防护）

  ctx.graph.add_node(
      n.id, [node_id, tool, parameters, retry, fallback, reg, guard](Json const& state,
                                                                      NodeContext&) {
        tool::ToolSpec const* spec = reg->find(tool);
        if (spec == nullptr)
          throw tool::ToolException("tool '" + tool + "' not in registry (node '" + node_id + "')");

        Json const args = resolve_args(parameters, state);
        tool::ToolContext tctx;
        tctx.tool_call_id = node_id;  // 幂等/审计兜底：节点 id 即本次调用的标识

        // wfr-㊷：执行点防护（单工具节点无并发 → prepare 决策门；保持 handler 的 JSON 语义）。
        //   决策不通过（重放/拦截）：不执行 handler，把报告文本当结果（尽力 parse 回 JSON）；
        //   决策通过：原 handler + retry/fallback 逻辑不变。
        if (guard != nullptr) {
          // prepare 内部对 read_only 免查日志/审计 → 只读工具不受影响
          tool::ExecutionGuard::Prepared const prep =
              guard->prepare(message::ToolCall{node_id, tool, args}, spec, "host");
          if (!prep.execute) {
            Json result;
            try {  // 重放内容若是 handler 结果的 JSON dump → parse 回原形（expr/route 继续能用）
              result = Json::parse(prep.report.text());
            } catch (...) {
              result = prep.report.text();  // 非 JSON（拦截说明/字符串结果）→ 原样字符串
            }
            Json update = Json{{kResultsKey, Json{{node_id, result}}}, {kLastExecKey, node_id}};
            return Command::with_update(std::move(update));
          }
        }

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
            b2_register_artifact(update, args, /*ok=*/true);  // B2：写产物工具成功登记版本
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
  std::vector<std::string> toolset_names;  // wfr-㊹：共享 registry 存在时按此名单重建 schema
  if (!n.toolset.empty() && ctx.components != nullptr)
    if (auto const* ts = ctx.components->toolset(n.toolset); ts != nullptr) {
      for (std::string const& tname : *ts) {
        toolset_names.push_back(tname);
        if (auto const* sp = ctx.registry->find(tname); sp != nullptr)
          tools_json.push_back(sp->to_openai());
      }
    }
  std::string const sys_spec = n.system_prompt;  // 运行期解析（@config.* 在节点体内替换）
  std::string const model_name = n.model;
  Json const params = n.model_params;          // wfr-㉙：运行期解析后并入 CallOptions.extra
  std::string const echo_spec = n.reasoning_echo;
  bool const stream_on = n.stream;
  auto sink_p = ctx.sink;  // 触发期注入的 NodeLog（docs/12 §6.4：source=node 的诊断日志）

  ctx.graph.add_node(
      n.id, [m, tools_json, toolset_names, sys_spec, model_name, params, echo_spec, stream_on, sink_p,
             node_id = n.id, reg = ctx.registry](Json const& state, NodeContext& nc) {
    std::vector<message::Message> msgs = messages_from_state(state);
    // 运行期解析 system_prompt 里的 @config.* / @env.* 引用（否则模型看到的是字面量占位符）
    std::string const sys = resolve_text_parameter(sys_spec, state);
    if (!sys.empty()) msgs.insert(msgs.begin(), message::system_message(sys));
    // wfr-context（⑤）：节点注入的 system 上下文（env + skill 等），装配时统一拼在
    // 系统提示之后、对话历史之前。来源 state[system_context]（context 节点写入；不进
    // messages 通道/会话日志 → 上下文只对本 run 生效，replay 不回溯）。
    if (auto it = state.find(kSystemContextKey); it != state.end() && it->is_array()) {
      std::vector<message::Message> ctx_msgs;
      for (Json const& raw : *it) {
        try {
          ctx_msgs.push_back(raw.get<message::Message>());
        } catch (std::exception const&) {
          // 形状由 context 节点保证；坏消息跳过（防御）
        }
      }
      if (!ctx_msgs.empty()) {
        auto const at = msgs.empty() ? msgs.begin() : msgs.begin() + 1;
        msgs.insert(at, ctx_msgs.begin(), ctx_msgs.end());
      }
    }
    model::CallOptions co;
    // wfr-㊹ 治理（problems/34）：共享工具注册表存在时，按 toolset 名单**运行期重建** schema
    //（共享优先、内置兜底，mock 工具可覆盖同名内置）；否则用编译期快照（既有行为）。
    // wfr-㊹ select：tool_registry 发布的活动工具集**优先于** toolset 组件 ——
    // 逐名取定义（共享→内置兜底），取不到即 fail-closed（正常编排不可达：select 名
    // 编译期已验、so 名已加载进共享表）。
    tool::ToolRegistry const* shared =
        (nc.shared_tools != nullptr) ? static_cast<tool::ToolRegistry const*>(nc.shared_tools)
                                     : nullptr;
    if (auto act = scan_active_tools(state); act != std::nullopt && !act->empty()) {
      Json live = Json::array();
      for (std::string const& name : act->order) {
        tool::ToolSpec const* sp = (shared != nullptr) ? shared->find(name) : nullptr;
        if (sp == nullptr && reg != nullptr) sp = reg->find(name);
        if (sp == nullptr)
          throw tool::ToolException("model 节点 '" + node_id + "'：活动工具 '" + name +
                                    "' 在共享/内置注册表都取不到定义");
        live.push_back(sp->to_openai());
      }
      co.tools = std::move(live);
    } else if (shared != nullptr && !toolset_names.empty()) {
      Json live = Json::array();
      for (std::string const& tname : toolset_names) {
        if (tool::ToolSpec const* sp = shared->find(tname); sp != nullptr)
          live.push_back(sp->to_openai());
        else if (reg != nullptr) {
          if (tool::ToolSpec const* bp = reg->find(tname); bp != nullptr)
            live.push_back(bp->to_openai());
        }
      }
      co.tools = std::move(live);
    } else {
      co.tools = tools_json;
    }
    // 厂商私有参数：逐键解析引用；解析为 null（@config 缺键）的**顶层键直接丢弃** ——
    // 不把 "reasoning_effort": null 发给端点（那会被当成显式设值）。
    for (auto it = params.begin(); it != params.end(); ++it) {
      Json v = resolve_parameter(it.value(), state);
      if (v.is_null()) continue;
      co.extra[it.key()] = std::move(v);
    }
    std::string const echo_text = resolve_text_parameter(echo_spec, state);
    co.reasoning_echo = parse_reasoning_echo(echo_text, node_id);
    std::expected<model::Message, model::ModelError> r;
    if (stream_on) {
      // 流式：请求体带 stream:true（走真实 token 流）；逐 delta 经 nc.emit("token") 发布，
      // 累积后 finalize_chunk 成完整消息。空流给一个空 Ai 消息（route 依 last_ai_outcome 兜底）。
      message::MessageChunk acc;
      bool has_acc = false;
      for (auto const& c : m->stream(msgs, co)) {
        for (auto const& blk : c.content) {
          if (auto const* t = std::get_if<message::TextBlock>(&blk)) {
            nc.emit("token", Json{{"text", t->text}});
          } else if (auto const* rb = std::get_if<message::ReasoningBlock>(&blk)) {
            nc.emit("token", Json{{"reasoning", rb->reasoning}});
          }
        }
        if (!has_acc) { acc = c; has_acc = true; } else { acc = acc + c; }
      }
      r = std::expected<model::Message, model::ModelError>(
          has_acc ? model::Message{message::finalize_chunk(acc)} : model::Message{});
    } else {
      r = m->invoke(msgs, co);
    }
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
  tool::ToolNode tn(ctx.registry);  // 复用 M3 的实现（并行、顺序对齐、错误回喂）——内置 registry 兜底
  tool::ExecutionGuard const* guard = ctx.guard;  // wfr-㊷：副作用防护（null = 不防护）
  tool::ToolRegistry const* reg = ctx.registry;
  ctx.graph.add_node(
      n.id, [tn, guard, reg, node_id = n.id](Json const& state, NodeContext& nc) {
        // wfr-㊹ 治理（problems/34）：共享工具优先 + 内置兜底（nc.shared_tools 由 tool_registry 注入）
        tool::ToolRegistry const* shared =
            (nc.shared_tools != nullptr) ? static_cast<tool::ToolRegistry const*>(nc.shared_tools)
                                         : nullptr;
        tool::ToolNode tn_eff = (shared != nullptr) ? tool::ToolNode(shared, reg) : tn;
        // 找最后一条 AI 消息：它（若在位置 i）之后的 ToolMessage 视为"已回执的调用"
        std::vector<message::ToolCall> calls;
        auto const msgs = messages_from_state(state);
        std::size_t last_ai_at = msgs.size();  // 哨兵：默认"AI 在最前/无 AI"，其后 Tool 全算
        for (std::size_t ii = msgs.size(); ii-- > 0;)
          if (msgs[ii].type == message::Role::Ai) {
            last_ai_at = ii;
            calls = msgs[ii].tool_calls;
            break;
          }
        if (calls.empty())
          throw tool::ToolException("run_tools 节点：最后一条 AI 消息没有 tool_calls");

        // wfr-㊷：approve_tools 可能已给**最后一条 AI 之后**的某些调用补过 error ToolMessage
        // （审批拒绝）。这些调用必须跳过执行 —— 且必须在这里（guard.prepare 之前）过滤：
        // 否则 guard 会当"未见过 → 正常执行"，把被拒的调用真实执行（deny 被静默反转）。
        // 判据 = "最后 AI 之后已存在同 id ToolMessage"（approve 的 error 恰好落在那里）；
        // 不扫全 state —— 避免把 load_history 投影出的历史 ToolMessage 误算进来（那会让
        // 新 AI 变成"调了工具无对应 ToolMessage" → 真端点 400）。
        {
          std::unordered_set<std::string> done_ids;
          for (std::size_t j = last_ai_at + 1; j < msgs.size(); ++j)
            if (msgs[j].is_tool() && msgs[j].tool_call_id) done_ids.insert(*msgs[j].tool_call_id);
          if (!done_ids.empty())
            calls.erase(std::remove_if(calls.begin(), calls.end(),
                                       [&](message::ToolCall const& c) {
                                         return done_ids.count(c.id) > 0;
                                       }),
                        calls.end());
        }

        // wfr-㊸ ⑤：approve_tools 人审放行的调用（approved_ids）→ guard 不再重复拦词面门禁
        // （人审看的正是这个 args；未批准的写/危险仍由 guard fail-closed 兜注入）。
        // 从 state results 里找 approve 节点发布的 approved_ids（state 纯函数的产物）。
        std::unordered_set<std::string> approved_ids;
        {
          auto const res = state.find(kResultsKey);
          if (res != state.end() && res->is_object()) {
            for (auto const& [k, v] : res->items()) {
              if (!v.is_object()) continue;
              auto const it = v.find("approved_ids");
              if (it == v.end() || !it->is_array()) continue;
              for (auto const& id : *it)
                if (id.is_string()) approved_ids.insert(id.get<std::string>());
            }
          }
        }

        // wfr-㊹ select：活动工具集允许名单（state 里 tool_registry 发布的 active_tools）。
        // 非活动名调用 → 拒绝执行（error ToolMessage 回喂，对齐 approve_tools denied 语义）。
        // 拒绝必须发生在这里（guard.prepare 之前）——被拒调用不进 guard（与 done_ids 过滤同位置）。
        std::vector<message::ToolCall> const all_calls = calls;  // 过滤后的原序（by_id/summary 用）
        std::vector<message::ToolCall> to_exec;
        std::vector<char> denied;  // 与 all_calls 对齐：1 = 拒绝（不执行）
        if (auto act = scan_active_tools(state); act != std::nullopt && !act->empty()) {
          denied.assign(all_calls.size(), 0);
          for (std::size_t i = 0; i < all_calls.size(); ++i)
            if (act->set.count(all_calls[i].name) > 0) to_exec.push_back(all_calls[i]);
            else denied[i] = 1;
        } else {
          to_exec = all_calls;  // 无活动集 → 全部执行（旧路径零回归）
        }

        // ---- wfr-㊹ 环检测：工具调用跨轮指纹（方法 1）----
        // agent 反复调同一工具（同名同参）打转成环 → 用 state 里的跨轮指纹表识别"连续重复"：
        //   第 1、2 次正常执行；第 3 次起**拦截**（error 回喂「已连续执行过 n 次，请换策略」）。
        // 语义（用户在计划中拍板）：
        //   · 指纹 = ExecutionGuard::key_for(call, NameAndArgs)（tool_name + canonical args，复用不新造）
        //   · "连续" = 上一轮实际执行的指纹集合（_prev）含它；换工具/换参 → 计数复位
        //   · 只读工具完全豁免（重复只读无害，同 guard 的 read_only 早退纪律）
        //   · 拦截不进 to_exec、不进 guard（与 allowlist/done_ids 同一纪律：判定在 guard.prepare 之前）
        // 表结构（state 顶层 kToolLoopKey，merge_slots 子键右胜）：
        //   { "_prev": [<上一轮执行指纹>...], "<fp>": <连续次数>, ... }
        // 注意：指纹表完全 state 派生（读 → 算 → 写），不碰外部日志 → 快照恢复重算一致（同 approve 纯态纪律）。
        constexpr long kToolLoopThreshold = 3;
        std::vector<char> repeat(all_calls.size(), 0);          // 与 all_calls 对齐：1 = 环拦截
        std::vector<message::ToolCall> next_exec;
        Json fp_map = Json::object();                           // 写回的新表（含 _prev + 计数）
        std::unordered_set<std::string> prev_exec;              // 上一轮执行指纹
        {
          auto const tl = state.find(kToolLoopKey);
          if (tl != state.end() && tl->is_object()) {
            auto const pv = tl->find("_prev");
            if (pv != tl->end() && pv->is_array())
              for (auto const& f : *pv)
                if (f.is_string()) prev_exec.insert(f.get<std::string>());
          }
        }
        // 本组已执行指纹（本轮真正进 to_exec 的），写回 _prev
        std::vector<std::string> this_prev;
        for (std::size_t i = 0; i < all_calls.size(); ++i) {
          if (!denied.empty() && denied[i]) continue;           // 已被活动集拒绝：不再参与
          message::ToolCall const& c = all_calls[i];
          // 只读豁免：spec 拿不到（未知）→ 不豁免（fail-closed 方向）
          tool::ToolSpec const* sp = tn_eff.find(c.name);
          if (sp != nullptr && sp->read_only) { next_exec.push_back(c); continue; }
          std::string const fp = tool::ExecutionGuard::key_for(c, tool::ExecKeyMode::NameAndArgs);
          // 连续次数 = 上一轮执行过同指纹 ? 旧计数+1 : 1（复位）
          long cur = 1;
          if (prev_exec.count(fp) > 0) {
            auto const old = state.find(kToolLoopKey);
            if (old != state.end() && old->is_object()) {
              auto const it = old->find(fp);
              if (it != old->end() && it->is_number()) cur = it->get<long>() + 1;
              else cur = 2;  // 上一轮执行过但无计数记录（异常/旧表）→ 保守视为第 2 次
            } else {
              cur = 2;
            }
          }
          if (cur >= kToolLoopThreshold) {                      // 环 → 拦截（计数保留继续涨）
            repeat[i] = 1;
            fp_map[fp] = cur;
            this_prev.push_back(fp);  // 拦截也算"上轮来过"——下一轮 prev_exec 含它，继续累进
            continue;
          }
          next_exec.push_back(c);
          fp_map[fp] = cur;
          this_prev.push_back(fp);
        }
        to_exec = std::move(next_exec);
        fp_map["_prev"] = [&] {
          Json a = Json::array();
          for (std::string const& f : this_prev) a.push_back(f);
          return a;
        }();

        std::vector<message::Message> exec_msgs;  // 与 to_exec 对齐
        if (guard == nullptr) {
          // 原路：ToolNode 自己并行执行；有共享 registry 时用「共享优先+内置兜底」的 tn_eff
          exec_msgs = tn_eff.run(to_exec);
        } else {
          // wfr-㊷：先**串行 prepare** 全部（审批/查重/登记意图，单线程访问日志与审计），
          // 放行的再**并行 run**（handler 直跑，不碰日志 → 并行安全）。顺序按下标回填。
          std::size_t const width = std::max<std::size_t>(1, tn_eff.options().max_parallel);
          struct Plan {
            tool::ExecutionGuard::Prepared prep;
            tool::ToolSpec const* spec = nullptr;
          };
          std::vector<Plan> plans(to_exec.size());
          for (std::size_t i = 0; i < to_exec.size(); ++i) {  // 串行段
            // 治理：共享优先、内置兜底（动态工具可覆盖同名内置 —— mock 顶替真工具）
            plans[i].spec = tn_eff.find(to_exec[i].name);
            plans[i].prep =
                guard->prepare(to_exec[i], plans[i].spec, "model",
                               approved_ids.count(to_exec[i].id) > 0);  // ⑤：人审放行 ≠ 重复拦
          }
          exec_msgs.resize(to_exec.size());
          for (std::size_t base = 0; base < to_exec.size(); base += width) {  // 并行段
            std::size_t const end = std::min(base + width, to_exec.size());
            std::vector<std::future<message::Message>> batch;
            batch.reserve(end - base);
            for (std::size_t i = base; i < end; ++i) {
              batch.push_back(std::async(std::launch::async, [&, i] {
                if (!plans[i].prep.execute) return plans[i].prep.report;  // 重放/拦截 → 报告
                return guard->run(to_exec[i], plans[i].spec, to_exec[i].args);  // 真执行
              }));
            }
            for (std::size_t i = base; i < end; ++i) exec_msgs[i] = batch[i - base].get();
          }
        }
        // 按 all_calls 原序归并：活动集拒绝 / 环拦截 → error ToolMessage 回喂（summary/审计可见工具名）
        std::vector<message::Message> tool_msgs;
        tool_msgs.reserve(all_calls.size());
        std::size_t ei = 0;
        for (std::size_t i = 0; i < all_calls.size(); ++i) {
          if (!denied.empty() && denied[i])
            tool_msgs.push_back(message::tool_message(
                "工具 '" + all_calls[i].name + "' 不在本 workflow 的活动工具名单里，拒绝执行",
                all_calls[i].id, "error"));
          else if (!repeat.empty() && repeat[i])
            tool_msgs.push_back(message::tool_message(
                "工具 '" + all_calls[i].name +
                    "' 已连续执行过多次（疑似在兜圈子），请换策略或换个参数，避免空转",
                all_calls[i].id, "error"));
          else
            tool_msgs.push_back(std::move(exec_msgs[ei++]));
        }
        Json arr = Json::array();
    for (message::Message& m : tool_msgs) {
      // 工具输出（含 CJK/截断字节）可能非法 UTF-8 → 净化后再进 state，避免 dump 崩溃
      for (auto& blk : m.content)
        if (auto* tb = std::get_if<message::TextBlock>(&blk)) message::sanitize_utf8(tb->text);
      arr.push_back(m);
    }
    // 发布可引用 result（echo 节点 $引用打印）：每个工具调用的 名字(参数) → 结果截断
    // 用 all_calls 建索引：被活动集拒绝的调用也有名字/参数可显示（而非裸 "tool"）。
    std::unordered_map<std::string, message::ToolCall const*> by_id;
    for (auto const& tc : all_calls) by_id[tc.id] = &tc;
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
    // B2（problems/39）：写产物工具执行**成功**（ToolMessage status=="success"）→ 登记版本。
    // 版本号来自引擎单调源（模型/工具伪造不了）；verify 判据可据此校验产物新鲜度。
    for (std::size_t i = 0; i < all_calls.size() && i < tool_msgs.size(); ++i)
      if (tool_msgs[i].status == "success") b2_register_artifact(update, all_calls[i].args, true);
    // wfr-㊹ 环检测：跨轮指纹表更新（merge_slots 子键右胜；仅当本轮有非只读调用参与才写，
    // 无环/纯只读 workflow 不产该键 = 零回归）
    if (fp_map.size() > 1) update[kToolLoopKey] = std::move(fp_map);
    return Command::with_update(std::move(update));
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ---- wfr-㊷/㊸：approve_tools（副作用审批决策层 + wfr-㊸ @② 人工悬挂）----
// 读最后一条 AI 消息的 tool_calls → 逐 call 审批：
//   · 未知工具 / deny 名单   → 静态拒绝：error ToolMessage 回喂（run_tools 靠过滤跳过）
//   · read_only / 名单外      → 放行：id 写 results（**不执行**，执行在 run_tools）
//   · NeedHuman 工具（bash） → **整节点悬挂**：interrupt 抛 InterruptSignal（落快照 +
//     payload 供宿主渲染待批请求）；恢复时节点从头重跑、interrupt() 返回人工裁决：
//       action=="approve" → 放行；其余（缺失/坏值）→ 视为拒绝 + feedback 回喂（fail-closed）。
//     注释：悬挂前 rejects 的静态判定（deny/未知）在重跑时会重算 —— 该节点是**纯 state
//     函数**（不碰外部），重跑一致；deny 名单硬拦先于 NeedHuman 判定（硬名单 > 人审）。
// 判定纯函数：复用 agent::ToolApproval（编译期构造、MemoPolicy::Off），节点行为 = state 纯函数。
// wfr-㊸ ⑤：命令危险度排序（allow 前缀"类别封顶"用）—— 干净→危险 = RO < Write < Risky ≤ Unknown。
int command_risk_rank(tool::CommandClass c) {
  switch (c) {
    case tool::CommandClass::ReadOnly: return 0;
    case tool::CommandClass::Write: return 1;
    case tool::CommandClass::Risky: return 2;
    case tool::CommandClass::Unknown: return 3;
  }
  return 3;
}
// 类别名字 → 枚举（allowlist 记录持久化用的是 command_class_name；名字稳定可读）。
tool::CommandClass command_class_from_name(std::string const& s) {
  if (s == "ReadOnly") return tool::CommandClass::ReadOnly;
  if (s == "Write") return tool::CommandClass::Write;
  if (s == "Risky") return tool::CommandClass::Risky;
  return tool::CommandClass::Unknown;  // 未识别名 → Unknown（fail-closed 方向）
}

void c_approve_tools(NodeIR const& n, NodeType::CompileContext& ctx) {
  auto const reg = ctx.registry;
  agent::ToolApproval app{n.deny, agent::ToolApproval::Options{
                                       .memo = agent::ToolApproval::MemoPolicy::Off,
                                       .registry = reg}};  // 按值捕获：节点 = state 纯函数
  ctx.graph.add_node(
      n.id, [app, reg, node_id = n.id](Json const& state, NodeContext& nc) mutable -> Command {
        auto const msgs = messages_from_state(state);
        std::vector<message::ToolCall> calls;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
          if (it->type == message::Role::Ai) {
            calls = it->tool_calls;
            break;
          }
        if (calls.empty()) return Command{};  // 无工具调用（text 轮）→ 不产任何东西

        Json msgs_update = Json::array();   // denied / 人类拒绝的 error ToolMessage
        Json approved_ids = Json::array();
        Json denied = Json::array();
        std::vector<message::ToolCall> pending;  // wfr-㊸ @②：NeedHuman 工具 → 待人工

        // wfr-㊸ @④：会话级 allow 前缀（宿主从 <session>.allowlist.jsonl 灌进 config）。
        // 匹配 = 命令字符串 starts_with(前缀)，且记录的 sandbox provider == 当前（范围不变量）,
        // 且（⑤ 注入封顶）**新命令的词面类别不比已批准记录更危险** ——
        //   批过只读前缀 `echo hi`，`echo hi; rm -rf /` 是 Risky → 不自动放行（照旧人审），
        //   防止「注入文本 → 模型在已批前缀后串联危险命令」被自动放行。
        // 缺配置（没开 allow_prefixes / 没有记录）→ approved_prefixes=null → 一律不命中（照旧人审）。
        Json const ap_cfg = resolve_parameter(Json("@config.approved_prefixes"), state);
        Json const sbx_cfg = resolve_parameter(Json("@config.sandbox_provider"), state);
        std::string const sbx_now = sbx_cfg.is_string() ? sbx_cfg.get<std::string>() : "";
        auto auto_allow = [&](message::ToolCall const& c) -> bool {
          if (!ap_cfg.is_array() || sbx_now.empty()) return false;
          if (!c.args.is_object() || !c.args.contains("command") ||
              !c.args["command"].is_string())
            return false;
          std::string const cmd = c.args["command"].get<std::string>();
          if (cmd.empty()) return false;
          tool::CommandClass const cur = tool::classify_bash_command(cmd);
          int const cur_rank = command_risk_rank(cur);
          for (auto const& rec : ap_cfg) {
            if (!rec.is_object()) continue;
            if (rec.value("provider", "") != sbx_now) continue;  // 沙箱身份一致才作数
            std::string const pfx = rec.value("prefix", "");
            if (pfx.empty() || !cmd.starts_with(pfx)) continue;
            std::string const cls = rec.value("class", "");
            if (cls.empty()) continue;  // ⑤：旧记录无类别 → 不自动放行（fail-closed，需重新人审）
            if (cur_rank > command_risk_rank(command_class_from_name(cls))) continue;
            return true;  // 前缀命中 且 类别不封顶 → 自动放行（语义见 problems/22）
          }
          return false;
        };

        for (message::ToolCall const& c : calls) {
          // wfr-㊹：内置 registry 找不到 → 共享 registry 兜底（动态 mock 工具常驻 shared_tools）。
          // 与 c_run_tools 的解析优先级对齐；再找不到才是"未知工具" fail-closed deny。
          tool::ToolSpec const* spec = reg ? reg->find(c.name) : nullptr;
          if (spec == nullptr && nc.shared_tools != nullptr)
            spec = static_cast<tool::ToolRegistry const*>(nc.shared_tools)->find(c.name);
          if (spec == nullptr) {
            // 未知工具 → fail-closed deny（ToolApproval"名单外放行"的兜底）
            std::string const why = "未知工具 '" + c.name + "'（不在注册表里），拒绝执行";
            denied.push_back(Json{{"id", c.id}, {"tool", c.name}, {"why", why}});
            msgs_update.push_back(message::tool_message("审批拒绝：" + why, c.id, "error"));
            continue;
          }
          std::string why;
          if (!app.approve_tool_call(c, why)) {  // deny 名单硬拦（read_only 在 approve_tool_call 内免审）
            denied.push_back(Json{{"id", c.id}, {"tool", c.name}, {"why", why}});
            msgs_update.push_back(message::tool_message("审批拒绝：" + why, c.id, "error"));
            continue;
          }
          if (spec->approval == tool::ToolApprovalLevel::NeedHuman) {
            // wfr-㊸ @④：会话级 allow 前缀自动放行。范围不变量：同会话 + **同沙箱 provider**
            // 才作数（沙箱降级后旧批准不再自动放行）。命中 → 直接放行不悬挂；否则照旧悬挂。
            if (auto_allow(c)) approved_ids.push_back(c.id);
            else pending.push_back(c);  // 需人工裁决（不会被静态策略放行/拦截）
          } else
            approved_ids.push_back(c.id);
        }

        // wfr-㊸ @②：有 NeedHuman 调用 → 整节点悬挂。
        // 首次跑：interrupt 抛 InterruptSignal（引擎落快照、payload 携待批请求）；
        // 恢复跑：节点从头重算（纯函数，结果一致）→ interrupt() 返回人工裁决。
        if (!pending.empty()) {
          Json req = Json::array();
          for (message::ToolCall const& c : pending)
            req.push_back(Json{{"id", c.id}, {"tool", c.name}, {"args", c.args}});
          Json const decision =
              interrupt(Json{{"kind", "approve_tools_request"}, {"node", node_id},
                             {"calls", std::move(req)}},
                        nc);
          std::string const action = decision.is_object() ? decision.value("action", "") : "";
          std::string const feedback =
              decision.is_object() ? decision.value("feedback", "") : "";
          if (action == "approve") {
            for (message::ToolCall const& c : pending) approved_ids.push_back(c.id);
          } else {
            std::string const why = "人工审批拒绝" +
                                    (feedback.empty() ? std::string{} : "：" + feedback);
            for (message::ToolCall const& c : pending) {
              denied.push_back(Json{{"id", c.id}, {"tool", c.name}, {"why", why}});
              msgs_update.push_back(message::tool_message(why, c.id, "error"));
            }
          }
        }

        Json update = Json::object();
        if (!msgs_update.empty()) update[kMessagesKey] = std::move(msgs_update);
        Json results = Json::object();
        results[node_id] = Json{{"approved_ids", std::move(approved_ids)},
                                {"denied", std::move(denied)}};
        update[kResultsKey] = std::move(results);
        return Command::with_update(std::move(update));
      });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ---- wfr-㊵：summarize（折叠成 LLM 摘要）----

void c_summarize(NodeIR const& n, NodeType::CompileContext& ctx) {
  model::ChatModel* m = (ctx.components != nullptr) ? ctx.components->model(n.model) : nullptr;
  if (m == nullptr) {
    ctx.error = "summarize 节点 '" + n.id + "' 引用的组件 '" + n.model + "' 不可用";
    return;
  }
  struct Specs {
    std::string when, max_msgs, max_tokens, keep_last, style, role, on_fail;
  };
  std::string const node_id = n.id;
  std::string const sys = n.system_prompt;
  std::string const echo_spec = n.reasoning_echo;
  Json const params = n.model_params;
  Specs const sp{n.when, n.max_msgs, n.max_tokens, n.keep_last, n.style, n.summary_role, n.on_fail};
  auto sink_p = ctx.sink;

  ctx.graph.add_node(n.id, [m, node_id, sys, echo_spec, params, sp, sink_p](
                              Json const& state, NodeContext& nc) {
    std::vector<message::Message> const all = messages_from_state(state);
    // 与 state 层的 fold 归约**同一判定**：剥掉头部摘要得到 base（标记是单一来源）
    std::size_t head = 0;
    while (head < all.size() && message::is_summary_message(all[head])) ++head;
    std::vector<message::Message> base(all.begin() + static_cast<long>(head), all.end());
    std::string head_text;  // 旧摘要原文 —— 喂给模型做"接力"，避免摘要间丢信息
    for (std::size_t i = 0; i < head; ++i) head_text += all[i].text() + "\n";

    std::string const when = resolve_enum_param(sp.when, state, node_id, "when",
                                                {"off", "messages", "tokens", "always"}, "messages");
    std::string const style = resolve_enum_param(sp.style, state, node_id, "style",
                                                 {"concise", "detailed", "evidence"}, "concise");
    std::string const role = resolve_enum_param(sp.role, state, node_id, "summary_role",
                                                {"system", "human", "ai"}, "system");
    std::string const on_fail = resolve_enum_param(sp.on_fail, state, node_id, "on_fail",
                                                   {"skip", "placeholder"}, "skip");
    std::size_t const keep = resolve_size_param(sp.keep_last, state, node_id, "keep_last", 6);
    std::size_t const max_msgs = resolve_size_param(sp.max_msgs, state, node_id, "max_msgs", 20);
    std::size_t const max_tokens = resolve_size_param(sp.max_tokens, state, node_id, "max_tokens", 2000);

    std::size_t const before_tokens = agent::tokens::estimate_msgs(base);
    Json frame;
    frame["when"] = when;
    frame["style"] = style;
    frame["summary_role"] = role;
    frame["on_fail"] = on_fail;
    frame["keep_last"] = static_cast<long>(keep);
    frame["before_msgs"] = static_cast<long>(base.size());
    frame["before_tokens"] = static_cast<long>(before_tokens);
    frame["after_msgs"] = static_cast<long>(base.size());   // 未折叠时先按原样
    frame["after_tokens"] = static_cast<long>(before_tokens);
    frame["summarized_msgs"] = 0;
    frame["kept_msgs"] = static_cast<long>(base.size());
    frame["summary_tokens"] = 0;

    auto publish = [&](std::string action, std::string reason) {
      frame["action"] = std::move(action);
      frame["reason"] = std::move(reason);
      sink_p->log_node(nc.node_name, "info",
                       "summarize: " + frame["action"].get<std::string>() + "（" +
                           frame["reason"].get<std::string>() + "）",
                       frame);
      Json results = Json::object();
      results[node_id] = frame;
      return Command::with_update(Json{{kResultsKey, std::move(results)}});
    };

    if (when == "off") return publish("skipped", "when_off");
    if (when == "messages" && base.size() <= max_msgs) return publish("skipped", "below_threshold");
    if (when == "tokens" && before_tokens <= max_tokens) return publish("skipped", "below_threshold");
    if (base.size() <= keep) return publish("skipped", "nothing_to_fold");

    // 折叠边界：复用 TrimHistory 的 safe_boundary —— 保留区不以孤儿 ToolMessage 开头、
    // 也不会把"Tool 在保留区而它的 AI(tool_calls) 被折掉"（真端点会 400）
    std::size_t const desired = base.size() - keep;
    std::size_t const cut = agent::TrimHistory::safe_boundary(base, 0, desired);
    if (cut == 0) return publish("skipped", "nothing_to_fold");
    std::size_t const kept = base.size() - cut;
    std::vector<message::Message> fold(base.begin(), base.begin() + static_cast<long>(cut));

    // 调模型做摘要（失败降级：skip 不发 op，placeholder 用占位文本仍然折叠）
    std::vector<message::Message> prompt;
    std::string inst = sys;
    if (!inst.empty()) inst += "\n\n";
    inst += summary_instruction(style);
    prompt.push_back(message::system_message(inst));
    std::string body;
    if (!head_text.empty()) body += "（更早的摘要，供接力）\n" + head_text + "\n";
    body += "待压缩的历史：\n" + render_history_for_summary(fold);
    prompt.push_back(message::human_message(body));

    model::CallOptions co;
    for (auto it = params.begin(); it != params.end(); ++it) {
      Json v = resolve_parameter(it.value(), state);
      if (v.is_null()) continue;   // 缺键不发（同 model 节点的语义）
      co.extra[it.key()] = std::move(v);
    }
    co.reasoning_echo = parse_reasoning_echo(resolve_text_parameter(echo_spec, state), node_id);

    std::string summary;
    auto const r = m->invoke(prompt, co);
    if (!r) {
      if (on_fail == "skip") return publish("failed", "llm_error");
      summary = "（摘要生成失败：这段历史已折叠，原文仍在会话日志里）";
    } else {
      summary = r->text();
    }
    message::sanitize_utf8(summary);
    if (summary.empty()) {
      if (on_fail == "skip") return publish("failed", "empty_summary");
      summary = "（摘要为空：这段历史已折叠，原文仍在会话日志里）";
    }

    message::Message sm = role == "human"   ? message::human_message(summary)
                          : role == "ai"    ? message::ai_message(summary)
                                            : message::system_message(summary);
    sm.additional_kwargs[message::kSummaryMarker] = true;
    sm.additional_kwargs["summarized_msgs"] = static_cast<long>(fold.size());

    Json prepend = Json::array({Json(sm)});
    Json op = Json{{"op", "fold"},
                   {"keep_last", static_cast<long>(kept)},
                   {"base_size", static_cast<long>(base.size())},
                   {"prepend", std::move(prepend)}};

    frame["action"] = "folded";
    frame["reason"] = "ok";
    frame["after_msgs"] = static_cast<long>(1 + kept);
    frame["summarized_msgs"] = static_cast<long>(fold.size());
    frame["kept_msgs"] = static_cast<long>(kept);
    frame["summary_tokens"] = static_cast<long>(agent::tokens::estimate(summary));
    frame["after_tokens"] = 1 + kept;  // 占位，下面用真实估算覆盖
    {
      std::vector<message::Message> after;
      after.push_back(sm);
      for (std::size_t i = base.size() - kept; i < base.size(); ++i) after.push_back(base[i]);
      frame["after_tokens"] = static_cast<long>(agent::tokens::estimate_msgs(after));
    }
    sink_p->log_node(nc.node_name, "info",
                     "summarize: folded " + std::to_string(fold.size()) + " 条 → 1 条摘要（" +
                         style + "）",
                     frame);

    Json update = Json::object();
    update[kMessagesKey] = std::move(op);
    Json results = Json::object();
    results[node_id] = frame;
    update[kResultsKey] = std::move(results);
    return Command::with_update(std::move(update));
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ---- wfr-㊵：refine_history（按规则精简/裁剪）----

void c_refine_history(NodeIR const& n, NodeType::CompileContext& ctx) {
  // 编译期快照"工具名 → 类别"（纯数据交给 agent 层的规则引擎；agent 不 include tool 头）
  std::unordered_map<std::string, std::string> tool_category;
  if (ctx.registry != nullptr)
    for (tool::ToolSpec const& sp : ctx.registry->all()) tool_category[sp.name] = sp.category;

  struct Specs {
    std::string preset, clip_scope, clip_mode, tool_budget, ai_budget, artifact_dir;
    std::vector<std::string> rules, categories;
  };
  std::string const node_id = n.id;
  Specs const sp{n.preset,      n.clip_scope, n.clip_mode,     n.tool_budget,
                 n.ai_budget,   n.artifact_dir, n.rules,        n.categories};
  auto sink_p = ctx.sink;

  ctx.graph.add_node(n.id, [node_id, sp, tool_category, sink_p](Json const& state,
                                                               NodeContext& nc) {
    std::vector<message::Message> const msgs = messages_from_state(state);

    std::string const preset = resolve_enum_param(sp.preset, state, node_id, "preset",
                                                  {"minimal", "lean", "aggressive", "clip"}, "");
    std::string const scope = resolve_enum_param(sp.clip_scope, state, node_id, "clip_scope",
                                                 {"tool_results", "ai_text", "both"}, "both");
    std::string const mode = resolve_enum_param(sp.clip_mode, state, node_id, "clip_mode",
                                                {"truncate", "artifact"}, "truncate");

    // 规则集合 = preset ∪ 静态 rules ∪ 运行期 categories（展开成 drop_category:<c>）
    std::vector<std::string> extra = sp.rules;
    for (std::string const& cspec : sp.categories) {
      std::string const t = resolve_text_parameter(cspec, state);
      if (t.empty() || t == "null") continue;
      std::string cur;
      for (char const ch : t + ",") {
        if (ch == ',' || ch == ' ' || ch == '\t') {
          if (!cur.empty()) extra.push_back("drop_category:" + cur);
          cur.clear();
        } else {
          cur.push_back(ch);
        }
      }
    }
    std::vector<std::string> const rules = agent::resolve_history_rules(preset, extra, scope);

    agent::HistoryRuleParams p;
    p.tool_budget = resolve_size_param(sp.tool_budget, state, node_id, "tool_budget", 200);
    p.ai_budget = resolve_size_param(sp.ai_budget, state, node_id, "ai_budget", 400);
    p.artifact_mode = (mode == "artifact");
    p.tool_category = tool_category;
    std::string dir = resolve_text_parameter(sp.artifact_dir, state);
    if (dir == "null") dir.clear();
    // 展示根 = 工作区绝对路径（与 wfr 文件工具的沙箱根一致）：落盘路径在引用行里
    // 改成相对这个根的形式，agent 才能把路径直接喂给 file_read（沙箱只认根内相对路径）。
    Json const ws = resolve_parameter(Json("@config.workspace"), state);
    std::string const ws_abs =
        (ws.is_string() && !ws.get<std::string>().empty())
            ? std::filesystem::absolute(ws.get<std::string>()).generic_string()
            : std::string{};
    if (dir.empty()) {
      // 缺省落在工作区的 .wfr/artifacts 下（与其它运行态产物同处）
      if (!ws_abs.empty()) dir = ws_abs + "/.wfr/artifacts";
      else dir = ".wfr/artifacts";
    }
    p.artifact_dir = dir;
    p.artifact_display_root = ws_abs;

    agent::HistoryRulePlan const plan = agent::plan_history_rules(msgs, rules, p);

    Json stats = agent::history_rule_stats_json(plan.stats);
    stats["action"] = (plan.drop.empty() && plan.rewrite.empty()) ? "noop" : "transformed";
    stats["clip_mode"] = mode;
    stats["clip_scope"] = scope;
    std::string rules_json;
    for (std::string const& r : rules) rules_json += (rules_json.empty() ? "" : ",") + r;
    stats["rules"] = rules_json;

    sink_p->log_node(nc.node_name, "info",
                     "refine_history: " + std::to_string(plan.stats.before_msgs) + " → " +
                         std::to_string(plan.stats.after_msgs) + " 条（规则 " + rules_json + "）",
                     stats);

    Json results = Json::object();
    results[node_id] = stats;
    if (plan.drop.empty() && plan.rewrite.empty()) {
      return Command::with_update(Json{{kResultsKey, std::move(results)}});
    }

    Json drop = Json::array();
    for (std::size_t const i : plan.drop) drop.push_back(static_cast<long>(i));
    Json rewrite = Json::array();
    for (auto const& [i, m] : plan.rewrite)
      rewrite.push_back(Json{{"index", static_cast<long>(i)}, {"message", Json(m)}});

    Json op = Json{{"op", "transform"},
                   {"base_size", static_cast<long>(msgs.size())},
                   {"drop", std::move(drop)},
                   {"rewrite", std::move(rewrite)}};
    Json update = Json::object();
    update[kMessagesKey] = std::move(op);
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

// ================= wfr-㊹：任务级 P-E-V 环（plan / verify，arch 19）=================
namespace {

inline char const* kPlanStateKey = "plan";
inline char const* kPlanLogStateKey = "plan_log";

// state.plan 取出来（缺省空对象）
Json plan_from_state(Json const& state) {
  auto const it = state.find("plan");
  return it != state.end() && it->is_object() ? *it : Json::object();
}

// 校验步骤数组：非空对象数组、每步 {id,goal}、deps 只指向前面的 id（上升依赖 → 天然无环）。
bool validate_plan_steps(Json const& steps, std::string& why) {
  if (!steps.is_array() || steps.empty()) { why = "步骤必须是非空数组"; return false; }
  std::vector<std::string> ids;
  ids.reserve(steps.size());
  for (Json const& s : steps) {
    if (!s.is_object()) { why = "步骤必须是对象"; return false; }
    if (!s.contains("id") || !s["id"].is_string() || s["id"].get<std::string>().empty()) {
      why = "步骤缺 id（非空字符串）"; return false;
    }
    if (!s.contains("goal") || !s["goal"].is_string() || s["goal"].get<std::string>().empty()) {
      why = "步骤 '" + s.value("id", "?") + "' 缺 goal（非空字符串）"; return false;
    }
    std::string const id = s["id"].get<std::string>();
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) { why = "步骤 id 重复: " + id; return false; }
    ids.push_back(id);
  }
  for (std::size_t i = 0; i < steps.size(); ++i) {
    if (!steps[i].contains("deps")) continue;
    auto const& deps = steps[i]["deps"];
    if (deps.is_null()) continue;
    if (!deps.is_array()) { why = "步骤 '" + steps[i].value("id", "?") + "' 的 deps 必须是数组"; return false; }
    for (auto const& d : deps) {
      if (!d.is_string()) { why = "deps 元素必须是字符串"; return false; }
      std::string const dn = d.get<std::string>();
      auto const it = std::find(ids.begin(), ids.end(), dn);
      if (it == ids.end()) { why = "步骤 " + steps[i].value("id", "?") + " 依赖不存在的步: " + dn; return false; }
      if (static_cast<std::size_t>(it - ids.begin()) >= i) { why = "deps 必须只指向前面的步骤（无环）"; return false; }
    }
  }
  why.clear();
  return true;
}

Json normalize_plan(Json const& goal, Json const& steps) {
  Json plan{{"goal", goal},
            {"cursor", 0},
            {"strategy", Json{{"on_fail", "retry"}, {"max_retry", 0}}}};
  Json out = Json::array();
  for (auto s : steps)
    if (s.is_object()) {
      if (!s.contains("status")) s["status"] = "pending";
      out.push_back(std::move(s));
    }
  plan["steps"] = std::move(out);
  return plan;
}

std::optional<Json> json_block_of(std::string const& text) {
  auto const b = text.find('{');
  if (b == std::string::npos) return std::nullopt;
  int depth = 0; bool in_str = false; char esc = 0;
  std::size_t e = b;
  for (; e < text.size(); ++e) {
    char const c = text[e];
    if (in_str) { if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '"') in_str = false; continue; }
    if (c == '"') in_str = true;
    else if (c == '{') ++depth;
    else if (c == '}') { if (--depth == 0) break; }
  }
  if (depth != 0) return std::nullopt;
  nlohmann::json const j = nlohmann::json::parse(text.substr(b, e - b + 1), nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) return std::nullopt;
  std::optional<Json> out;
  out.emplace(j);
  return out;
}

// wfr-㊿ 长程：今天日期（YYYY-MM-DD，本地）
std::string today_str() {
  std::time_t const t = std::time(nullptr);
  std::tm tm{};
  ::localtime_r(&t, &tm);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
}

}  // namespace

// ---- B2（problems/39 优先级 2）：产物版本登记与新鲜度 ----
// 版本号由引擎单调源 next_artifact_version() 分配（模型/工具伪造不了）；存 state._artifact_ver
//（merge_slots 右胜）。verify probe 判据可声明 version 校验"产物新鲜度"。

// 写产物工具执行**成功**后登记：call.args 含 "path"（相对沙箱根串，file_write 的 schema 约定）
// → 给该 relpath 分配一个单调版本号写进 update["_artifact_ver"]。ok=false 不登记。
void b2_register_artifact(Json& update, Json const& call_args, bool ok) {
  if (!ok || !call_args.is_object() || !call_args.contains("path") ||
      !call_args["path"].is_string())
    return;
  Json ver = update.contains(kArtifactVerKey) ? update[kArtifactVerKey] : Json::object();
  ver[call_args["path"].get<std::string>()] = next_artifact_version();
  update[kArtifactVerKey] = std::move(ver);
}

// 读 state 里某 relpath 的当前版本；未登记 → -1（视为过时/未写入）。
std::int64_t b2_artifact_version(Json const& state, std::string const& relpath) {
  auto const it = state.find(kArtifactVerKey);
  if (it == state.end() || !it->is_object()) return -1;
  auto const p = it->find(relpath);
  if (p == it->end() || !p->is_number()) return -1;
  return p->get<std::int64_t>();
}

// 批② L2：verify 的确定性判据（probe / rule），不调 LLM。路径按 @config.workspace 解析
// （相对 → 工作区根；与 file_read/file_write 的沙箱根一致，probe 的就是同一棵树）。
bool verify_rule_check(Json const& v, Json const& state, std::string& why) {
  Json const ws = resolve_parameter(Json("@config.workspace"), state);
  std::filesystem::path const base =
      ws.is_string() ? std::filesystem::absolute(ws.get<std::string>())
                     : std::filesystem::current_path();
  std::string const p = v.value("path", "");
  if (p.empty()) { why = "verify 缺 path"; return false; }
  std::filesystem::path const target =
      std::filesystem::path(p).is_relative() ? base / p : std::filesystem::path(p);

  std::string const kind = v.value("kind", "");
  if (kind == "probe") {
    // contains → 子串探针（补的语义）；bytes → 仅大小约束（整数签名）
    tool::ProbeResult r = tool::ProbeResult::Unknown;
    if (v.contains("contains") && v["contains"].is_string()) {
      r = tool::probe_file_contains(target, v["contains"].get<std::string>());
    } else {
      std::optional<std::size_t> bytes;
      if (v.contains("bytes") && v["bytes"].is_number_unsigned()) bytes = v["bytes"].get<std::size_t>();
      r = tool::probe_file(target, std::nullopt, bytes);
    }
    if (r == tool::ProbeResult::Complete) {
      // B2（problems/39）：文件内容完整后，若判据声明 version → 校验"产物新鲜度"
      //（当前登记版本 ≥ 判据要求；未登记=从未写/跨 run 丢了 → 视为过时）。不新鲜映射 Partial
      //（证据在但不可接受，fail-closed），与"内容不匹配"同档。
      if (v.contains("version") && v["version"].is_number_unsigned()) {
        std::int64_t const have = b2_artifact_version(state, p);
        std::int64_t const want = v["version"].get<std::int64_t>();
        if (have < want) {
          why = "probe=Partial：路径 " + p + "（版本 " + std::to_string(have) + " < 要求 " +
                std::to_string(want) + "）——证据过时";
          return false;
        }
      }
      why = "文件完整 → " + target.string();
      return true;
    }
    char const* n = r == tool::ProbeResult::Partial ? "Partial"
                    : r == tool::ProbeResult::Absent ? "Absent" : "Unknown";
    why = std::string("probe=") + n + "：" + target.string();
    return false;
  }
  // kind == "rule"
  std::string const op = v.value("op", "");
  if (op == "file_exists") {
    bool const ok = std::filesystem::is_regular_file(target);
    why = std::string(ok ? "存在" : "不存在") + "：" + target.string();
    return ok;
  }
  if (op == "file_nonempty") {
    std::error_code ec;
    std::uintmax_t const sz = std::filesystem::file_size(target, ec);
    bool const ok = !ec && sz > 0;
    why = std::string(ok ? "非空(" + std::to_string(sz) + "B)" : "空/缺失") + "：" + target.string();
    return ok;
  }
  if (op == "json_key") {
    std::ifstream in(target);
    if (!in) { why = "读不到 " + target.string(); return false; }
    std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto const j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    std::string const key = v.value("key", "");
    bool const ok = !j.is_discarded() && !key.empty() && j.contains(key);
    why = ok ? ("JSON 含键 " + key) : ("JSON 缺键 " + key + "（解析: " +
                                      std::string(j.is_discarded() ? "失败" : "成功") + "）");
    return ok;
  }
  why = "未知 rule op: " + op;
  return false;
}

// 批③ L3：模型裁判（kind=model）。固定提示词 + 输入 = 步骤目标 + 本步执行记录文本（注入面最小）；
// 只认 reply 里的 {"pass":bool,"feedback":str}。模型错/解析失败 → fail-closed（不当作 pass）。
bool judge_model_review(Json const& step, Json const& v, Json const& state,
                        model::ChatModel* m, std::string const& judge_sys, std::string& why) {
  if (m == nullptr) { why = "judge 模型未配置（verify 节点缺 model）"; return false; }
  std::string const goal = step.value("goal", "");
  std::string const criteria = v.value("criteria", "");
  // 本步执行记录：取最近若干条消息的文本（announce 之后的模型回答 + 工具结果）
  std::string ctx;
  auto const msgs = messages_from_state(state);
  std::size_t const take = std::min<std::size_t>(msgs.size(), 8);
  for (std::size_t i = msgs.size() - take; i < msgs.size(); ++i) {
    std::string const t = msgs[i].text();
    if (t.empty()) continue;
    char const* role = msgs[i].type == message::Role::Human ? "用户" :
                       msgs[i].type == message::Role::Ai ? "助手" : "工具";
    ctx.append(role).append("：").append(t).append("\n");
  }
  message::Message const sys = message::system_message(
      judge_sys.empty()
          ? "你是任务步骤验收员。依据【验收标准】判断【执行记录】是否达成【步骤目标】。"
            "只输出 JSON：{\"pass\":true或false,\"feedback\":\"一句理由\"}，不要输出别的。"
          : judge_sys);
  message::Message const usr =
      message::human_message("【步骤目标】" + goal + "\n【验收标准】" + criteria +
                             "\n【执行记录】\n" + ctx);
  auto resp = m->invoke({sys, usr}, {});
  if (!resp) { why = "judge 调用失败：" + resp.error().message; return false; }
  auto const j = json_block_of(resp->text());
  if (!j.has_value()) { why = "judge 未返回可解析 JSON"; return false; }
  if (!j->contains("pass")) { why = "judge 缺 pass 字段"; return false; }
  bool const pass = j->value("pass", false);
  why = std::string(pass ? "模型裁判通过" : "模型裁判不通过")
        + (j->contains("feedback") && (*j)["feedback"].is_string()
               ? std::string("：") + (*j)["feedback"].get<std::string>() : std::string());
  return pass;
}

// ---- judge_batch_review（wfr-㊾）：批级独立终审（Reviewer 角色）----
// 与 judge_model_review 的区别 = **输入隔离**：只喂 整批计划 + 每一步的判定（plan_log），
// 不把执行思考/全局会话带进去 —— 架构 21 §一：Reviewer 独立审查者，不盲从执行者自述。
bool judge_batch_review(Json const& plan, Json const& plan_log, std::string const& criteria,
                        model::ChatModel* m, std::string const& rev_sys, std::string& why) {
  if (m == nullptr) { why = "review 模型未配置（verify review 节点缺 model）"; return false; }
  // 整批计划摘要：目标 + 每步（id/goal/status + plan_log 里该步的判定 verdict）
  std::string plan_text;
  std::string const goal = plan.value("goal", "");
  if (!goal.empty()) plan_text += "【任务目标】" + goal + "\n";
  plan_text += "【步骤与判定】\n";
  if (plan.contains("steps") && plan["steps"].is_array()) {
    std::size_t i = 0;
    for (Json const& s : plan["steps"]) {
      ++i;
      std::string line = "  " + std::to_string(i) + ". [" + s.value("id", "?") + "] " +
                         s.value("goal", "") + " —— 状态:" + s.value("status", "pending");
      // 从 plan_log 补上该步的判定（executor 自查 judge 的结果），供 Reviewer 复核
      if (plan_log.is_array())
        for (Json const& e : plan_log)
          if (e.is_object() && e.value("step", "") == s.value("id", "") && e.contains("verdict"))
            line += "（步内判定 " + e["verdict"].get<std::string>() + e.value("why", "") + ")";
      plan_text += line + "\n";
    }
  }
  message::Message const sys = message::system_message(
      rev_sys.empty()
          ? "你是独立审查员。依据【验收标准】对【任务目标】的各步骤判定做独立复核。"
            "你只看步骤目标与步内判定，不偏信执行者的口头自述。"
            "步骤判定为 unverified（未验证）的必须独立核对产物或明确放行理由，不得仅凭执行者自述放行。"
            "只输出 JSON：{\"pass\":true或false,\"feedback\":\"一句理由\"}，不要输出别的。"
          : rev_sys);
  message::Message const usr =
      message::human_message("【验收标准】" + criteria + "\n" + plan_text);
  auto resp = m->invoke({sys, usr}, {});
  if (!resp) { why = "review 调用失败：" + resp.error().message; return false; }
  auto const j = json_block_of(resp->text());
  if (!j.has_value()) { why = "review 未返回可解析 JSON"; return false; }
  if (!j->contains("pass")) { why = "review 缺 pass 字段"; return false; }
  bool const pass = j->value("pass", false);
  why = std::string(pass ? "批级终审通过" : "批级终审不通过")
        + (j->contains("feedback") && (*j)["feedback"].is_string()
               ? std::string("：") + (*j)["feedback"].get<std::string>() : std::string());
  return pass;
}

// ---- plan_review（批④）：步骤边界任务级 HITL（interrupt 悬挂 → continue/rollback_to:K/abort）----
void v_plan_review(NodeIR const& n, NodeType::ValidateContext const&,
                   NodeType::ErrorSink const& err) {
  if (n.next.empty()) err("VN20", "plan_review 节点 '" + n.id + "' 缺少 next");
}

void c_plan_review(NodeIR const& n, NodeType::CompileContext& ctx) {
  std::string const node_id = n.id;
  std::string const confirm_spec = n.confirm;
  std::string const break_target = ctx.goto_resolve ? ctx.goto_resolve(kBreakName) : kBreakName;
  // 治理（problems/33）：取消 = 终态档 `_cancelled`（不再是"跳 loop 出口冒充完成"）
  std::string const cancelled_target =
      ctx.goto_resolve ? ctx.goto_resolve(kCancelledName) : kCancelledName;
  ctx.graph.add_node(
      n.id, [node_id, confirm_spec, break_target, cancelled_target](Json const& state,
                                                                   NodeContext& nc) {
    Json plan = plan_from_state(state);
    Json steps = plan.contains("steps") && plan["steps"].is_array() ? plan["steps"] : Json::array();
    long const cursor = plan.value("cursor", 0L);
    std::string const confirm_eff = confirm_spec.empty() ? resolve_text_parameter("@config.plan_confirm", state)
                                                     : confirm_spec;
    bool const confirm = confirm_eff != "false" && confirm_eff != "no";  // 缺省 true = 默认确认门
    Json results = Json::object();
    Json plan_log = state.contains(kPlanLogStateKey) && state[kPlanLogStateKey].is_array()
                        ? state[kPlanLogStateKey] : Json::array();
    Json update = Json::object();

    if (!confirm || cursor < 0 || cursor >= static_cast<long>(steps.size())) {
      // 关确认门 或 无下一步（全部完成）→ 直接穿过
      results[node_id] = Json{{"action", "skip"},
                              {"note", confirm ? "无下一步" : "确认门关闭"}};
      update[kResultsKey] = std::move(results);
      return Command::with_update(std::move(update));
    }

    // 悬挂 payload：进度 + 下一步（人据此裁决）
    std::string done_summary;
    for (Json const& s : steps) {
      std::string const st = s.value("status", "pending");
      // 治理（problems/33）：unverified（完成但未验证）也计入进度摘录，HITL 人闸才看得到
      if (st == "done" || st == "unverified")
        done_summary += (s.value("id", "?") + std::string(st == "done" ? "✓ " : "◌ ") +
                         s.value("goal", "") + "\n");
    }
    std::string const next_goal =
        cursor >= 0 && cursor < static_cast<long>(steps.size())
            ? steps[static_cast<std::size_t>(cursor)].value("goal", "")
            : "(全部完成)";
    Json const payload = Json{{"kind", "plan_review_request"},
                              {"node", node_id},
                              {"plan", Json{{"goal", plan.value("goal", "")},
                                            {"cursor", cursor},
                                            {"count", steps.size()},
                                            {"done", done_summary},
                                            {"next", next_goal}}}};
    Json const decision = interrupt(payload, nc);
    std::string const action = decision.is_object() ? decision.value("action", "") : "";

    if (action == "continue" || action.empty()) {
      results[node_id] = Json{{"action", "continue"}, {"cursor", cursor}};
    } else if (action == "rollback_to") {
      long const k = decision.is_object() ? decision.value("to", 0L) : 0L;
      long const to = std::max(0L, std::min(k, static_cast<long>(steps.size())));
      std::vector<std::string> rolled;
      for (long i = to; i < static_cast<long>(steps.size()); ++i) {
        std::string const st = steps[static_cast<std::size_t>(i)].value("status", "pending");
        if (st != "pending") rolled.push_back(steps[static_cast<std::size_t>(i)].value("id", "?"));
        steps[static_cast<std::size_t>(i)]["status"] = "pending";  // 回退 → 重新做
      }
      plan["cursor"] = to;
      plan["steps"] = std::move(steps);
      update[kPlanStateKey] = plan;
      plan_log.push_back(Json{{"kind", "rollback"}, {"action", "rollback_to"}, {"to", to},
                              {"rolled", rolled}, {"priority", 1}});
      update[kPlanLogStateKey] = std::move(plan_log);
      results[node_id] = Json{{"action", "rollback_to"}, {"to", to}, {"rolled", rolled}};
    } else {  // abort / 未知（fail-closed 按 abort）→ 治理统一取消档
      plan["cancelled"] = true;
      for (auto& s : steps)
        if (s.is_object() && s.value("status", "pending") == "pending")
          s["status"] = "cancelled";
      plan["steps"] = std::move(steps);
      update[kPlanStateKey] = plan;
      plan_log.push_back(Json{{"kind", "cancel"}, {"action", "abort"}});
      update[kPlanLogStateKey] = std::move(plan_log);
      results[node_id] = Json{{"action", "abort"}};
      update[kResultsKey] = std::move(results);
      return Command{std::move(update), {cancelled_target}, {}};  // 取消 → _cancelled 终端
    }
    update[kResultsKey] = std::move(results);
    return Command::with_update(std::move(update));
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ---- task（wfr-㊿ 长程）：begin 建档/还原 + end 写回档案，跨批唯一接口 ----
void v_task(NodeIR const& n, NodeType::ValidateContext const&,
            NodeType::ErrorSink const& err) {
  if (n.phase != "begin" && n.phase != "end")
    err("VN20", "task 节点 '" + n.id + "' 的 phase 必须为 begin|end");
  if (n.next.empty()) err("VN20", "task 节点 '" + n.id + "' 缺少 next");
}

std::filesystem::path task_archive_path(Json const& state, std::string const& task) {
  Json const ws = resolve_parameter(Json("@config.workspace"), state);
  std::filesystem::path const base =
      ws.is_string() ? std::filesystem::path(ws.get<std::string>())
                     : std::filesystem::current_path();
  return base / ".wfr" / "tasks" / (task + ".json");
}

std::optional<Json> read_json_file(std::filesystem::path const& p) {
  std::ifstream in(p);
  if (!in) return std::nullopt;
  std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto const j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) return std::nullopt;
  std::optional<Json> out;
  out.emplace(j);
  return out;
}
void write_json_file(std::filesystem::path const& p, Json const& j) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::trunc);
  if (out) out << j.dump(2) << '\n';
}

void c_task(NodeIR const& n, NodeType::CompileContext& ctx) {
  std::string const node_id = n.id;
  std::string const phase = n.phase;
  std::string const task_spec = n.task;
  ctx.graph.add_node(n.id, [node_id, phase, task_spec](Json const& state, NodeContext&) {
    std::string const task = resolve_text_parameter(task_spec.empty() ? "@config.task" : task_spec,
                                                    state);
    Json results = Json::object();
    if (phase == "begin") {
      std::filesystem::path arch = task_archive_path(state, task);
      Json plan;
      bool created = false;
      // 批间可改：@config.task_revision=N → 从 .history/batchN.json（第 N 批结束快照）续，而非最新档案
      std::string const rev = resolve_text_parameter("@config.task_revision", state);
      if (!rev.empty() && rev != "null") {
        long const n = std::max(0L, std::stol(rev));
        std::filesystem::path const h =
            arch.parent_path() / (arch.stem().string() + ".history") /
            ("batch" + std::to_string(n) + ".json");
        if (auto const old = read_json_file(h); old.has_value() && old->is_object()) {
          arch = h;
          plan = *old;
        }
      }
      if (plan.empty()) {
        if (auto const loaded = read_json_file(arch); loaded.has_value() && loaded->is_object()) {
          plan = *loaded;
        }
      }
      if (!plan.empty()) {
        plan["batch_no"] = plan.value("batch_no", 1L) + 1;  // 进入下一批
        created = false;
      } else if (auto const loaded = read_json_file(task_archive_path(state, task));
                 loaded.has_value() && loaded->is_object()) {
        // latest 兜底（revision 缺失历史时的回退）
        plan = *loaded;
        plan["batch_no"] = plan.value("batch_no", 1L) + 1;
        created = false;
      } else {
        // 建档：以 @config.plan_steps（JSON 步骤数组字符串）播种
        Json const steps_cfg = resolve_parameter(Json("@config.plan_steps"), state);
        Json steps;
        if (steps_cfg.is_string() && !steps_cfg.get<std::string>().empty()) {
          auto const par = nlohmann::json::parse(steps_cfg.get<std::string>(),
                                                 nullptr, /*allow_exceptions=*/false);
          std::string why;
          if (!par.is_discarded() && par.is_array() && validate_plan_steps(par, why)) steps = par;
        }
        if (steps.is_array() && !steps.empty()) {
          std::string const goal = resolve_text_parameter("@config.goal", state);
          plan = normalize_plan(goal, steps);
          plan["task"] = task;
          plan["created"] = today_str();
          plan["batch_no"] = 1;
          created = true;
        } else {
          results[node_id] = Json{{"action", "missing_seed"}};
          return Command::with_update(Json{{kResultsKey, std::move(results)}});
        }
      }
      // A2：批间复查 —— restores 且 @config.recheck_prev=on、当前不是首批、且有上批摘要时，
      // 在游标处插入「复查上批摘要」判据步（L3 模型裁判；不推进 cursor，随批内第一步执行）
      std::string const rp = resolve_text_parameter("@config.recheck_prev", state);
      std::string const prev2 = plan.value("last_summary", "");
      if (!created && (rp == "on" || rp == "true" || rp == "1") &&
          plan.value("batch_no", 1L) >= 2 && !prev2.empty() && plan.contains("steps") &&
          plan["steps"].is_array()) {
        auto& steps = plan["steps"];
        long const pos = std::min<long>(plan.value("cursor", 0L),
                                        static_cast<long>(steps.size()));
        Json audit = Json{{"id", "audit_prev"},
                          {"goal", "复查上批摘要与本批目标/已知事实的一致性；若涉及时变事实请指出变化"},
                          {"deps", Json::array()},
                          {"verify", Json{{"kind", "model"},
                                          {"criteria", "上批摘要：" + prev2}}}};
        steps.insert(steps.begin() + pos, std::move(audit));
      }
      Json update;
      update[kPlanStateKey] = plan;
      // B2（problems/39）：档案里上次的产物版本表 → 恢复进运行期 state（跨 run 续跑保持单调）
      if (plan.contains("artifact_versions") && plan["artifact_versions"].is_object() &&
          !plan["artifact_versions"].empty())
        update[kArtifactVerKey] = plan["artifact_versions"];
      std::string const prev = plan.value("last_summary", "");
      std::string const day = [&] {
        std::string const d = resolve_text_parameter("@config.task_date", state);
        return (d.empty() || d == "null") ? today_str() : d;
      }();
      std::string const add = "📅 第 " + std::to_string(plan.value("batch_no", 1L)) +
                              " 批 @ " + day + (created ? "（建档）" : "") +
                              (prev.empty() ? "" : "\n上批摘要：" + prev) +
                              (prev.empty() && plan.value("batch_no", 1L) > 1 ? "\n（本批为次日续跑）" : "");
      update[kMessagesKey] = Json::array({message::human_message(add)});
      if (created) write_json_file(arch, plan);
      results[node_id] = Json{{"action", "begin"}, {"batch", plan.value("batch_no", 1L)},
                            {"cursor", plan.value("cursor", 0L)}};
      update[kResultsKey] = std::move(results);
      return Command::with_update(std::move(update));
    }
    // ---- end：写回档案
    Json const plan = plan_from_state(state);
    if (plan.empty()) {
      results[node_id] = Json{{"action", "end_noop"}};
      return Command::with_update(Json{{kResultsKey, std::move(results)}});
    }
    std::string last;
    auto const msgs = messages_from_state(state);
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
      if (it->type == message::Role::Ai) { last = it->text(); break; }
    Json plan2 = plan;
    plan2["last_summary"] = last;
    // B2（problems/39）：产物版本表快照进档案（跨 run 续跑恢复；merge_slots 运行期不冲突）
    {
      auto const av = state.find(kArtifactVerKey);
      if (av != state.end() && av->is_object() && !av->empty())
        plan2["artifact_versions"] = *av;
    }
    // 批② 护栏：plan 无步骤（begin 缺种子等）不得覆盖档案
    if (!(plan2.contains("steps") && plan2["steps"].is_array() && !plan2["steps"].empty())) {
      results[node_id] = Json{{"action", "end_noop"}};
      return Command::with_update(Json{{kResultsKey, std::move(results)}});
    }
    plan2["batch_no"] = plan.value("batch_no", 1L);
    std::filesystem::path const arch = task_archive_path(state, task);
    write_json_file(arch, plan2);
    // 历史快照（批间可改 / revision 续跑用）
    write_json_file(arch.parent_path() / (arch.stem().string() + ".history") /
                        ("batch" + std::to_string(plan2.value("batch_no", 1L)) + ".json"),
                    plan2);
    results[node_id] = Json{{"action", "end"}, {"cursor", plan2.value("cursor", 0L)},
                           {"batch", plan2.value("batch_no", 1L)}};
    return Command::with_update(Json{{kResultsKey, std::move(results)}});
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ---- subworkflow（wfr-㊿ proxyNode，见 02-arch-design/24）：运行期调用子 workflow ----
// 编译期：通过 subworkflow_loader 读子 workflow JSON → parse → validate → compile_workflow
// （fail-fast：找不到/校验不过/编译失败 → 主编译即报错）。子 workflow 编译复用宿主全套契约
// （registry/components/config_keys/guard/shared_registry/max_steps/max_parallel），并共享
// subflow_active 编译链（环/深度可检出——深度上限经 cctx.max_subflow_depth）。
// 运行期：sync = 同步跑完子图，子最终 state 的 results 通道键提升为 results.<nodeId> 顶层键
//        + _proxy 元信息；detach = fire-and-forget（后台线程跑子图，结果落盘审计）。
void v_subworkflow(NodeIR const& n, NodeType::ValidateContext const&,
                   NodeType::ErrorSink const& err) {
  if (n.sub_workflow.empty())
    err("VN24", "subworkflow 节点 '" + n.id + "' 缺 workflow（子 workflow 名/路径）");
  if (!n.subflow_mode.empty() && n.subflow_mode != "sync" && n.subflow_mode != "detach")
    err("VN24", "subworkflow 节点 '" + n.id + "' 的 mode 必须是 sync|detach（缺省 sync）");
  for (std::string const& k : n.copy_state)
    if (k.empty()) err("VN24", "subworkflow 节点 '" + n.id + "' 的 copy_state 含空键");
  if (!n.sub_input.is_object())
    err("VN24", "subworkflow 节点 '" + n.id + "' 的 input 必须是对象");
  if (n.next.empty()) err("VN24", "subworkflow 节点 '" + n.id + "' 缺少 next");
}

// 子 workflow 引用文本（input 里可含 $ / @config 引用，与 tool.parameters 同套路）
std::vector<std::string> t_subworkflow(NodeIR const& n) {
  std::vector<std::string> out;
  collect_strings(n.sub_input, out);
  return out;
}

void c_subworkflow(NodeIR const& n, NodeType::CompileContext& ctx) {
  if (ctx.subworkflow_loader == nullptr) {
    ctx.error = "subworkflow 节点 '" + n.id +
                "'：宿主未提供 subworkflow_loader（CompileOptions.subworkflow_loader），子 workflow 无法编译";
    return;
  }
  if (ctx.registry == nullptr) {
    ctx.error = "subworkflow 节点 '" + n.id + "'：宿主未提供注册表（子 workflow 编译需要 registry）";
    return;
  }
  // 编译子 workflow（fail-fast：编译期读 JSON + 校验 + 编译，找不到/坏 IR 都在此处炸）。
  // 用 cctx 的 ctor 参数之外：loader 返回完整可 parse 的 JSON（宿主负责路径解析/import 展开）。
  std::optional<Json> raw = ctx.subworkflow_loader(n.sub_workflow);
  if (!raw) {
    ctx.error = "subworkflow 节点 '" + n.id + "'：找不到子 workflow '" + n.sub_workflow + "'";
    return;
  }
  auto sub_ir = WorkflowIR::parse(*raw);
  if (!sub_ir) {
    ctx.error = "subworkflow 节点 '" + n.id + "' 子 workflow '" + n.sub_workflow +
                "' IR 解析失败: " + sub_ir.error();
    return;
  }
  // 校验子 IR：复用宿主契约（@config / 组件 / 工具注册表）。空则跳过（校验器允许）。
  if (ctx.registry != nullptr || ctx.components != nullptr || ctx.config_keys.has_value()) {
    ValidationResult const vr = validate(*sub_ir, ValidatorOptions{ctx.registry, ctx.config_keys, ctx.components});
    if (!vr.ok()) {
      ctx.error = "subworkflow 节点 '" + n.id + "' 子 workflow '" + n.sub_workflow +
                  "' 校验未过：" + vr.describe();
      return;
    }
  }
  // 深度上限（编译链现有成员 = 祖辈深度；顶层 workflow 也占一位，见 compiler.cpp 的 chain guard）。
  if (ctx.subflow_active != nullptr && ctx.subflow_active->size() >= ctx.max_subflow_depth) {
    ctx.error = "subworkflow 节点 '" + n.id + "' 嵌套过深（>= " +
                std::to_string(ctx.max_subflow_depth) + " 层，疑似循环引用）";
    return;
  }
  // 编译子 workflow：复用宿主组件/工具/guard/并行度；loader 与编译链继续下发（子内可再嵌子）。
  CompileOptions sub_opts;
  sub_opts.components = ctx.components;
  sub_opts.max_steps = ctx.max_steps;
  sub_opts.max_parallel = ctx.max_parallel;
  sub_opts.config_keys = ctx.config_keys;
  sub_opts.config_dir = ctx.config_dir;  // P1.5：声明式配置基准目录（子自带声明时同根解析）
  sub_opts.tool_policy = ctx.guard;
  sub_opts.shared_registry = ctx.shared_registry;
  sub_opts.tool_lib_dirs = ctx.tool_lib_dirs;
  sub_opts.subworkflow_loader = ctx.subworkflow_loader;
  sub_opts.max_subflow_depth = ctx.max_subflow_depth;
  sub_opts.subflow_active = ctx.subflow_active;
  auto sub_wf = compile_workflow(*sub_ir, *ctx.registry, /*cp=*/nullptr, sub_opts);
  if (!sub_wf) {
    ctx.error = "subworkflow 节点 '" + n.id + "' 子 workflow 编译失败: " + sub_wf.error().message;
    return;
  }
  auto child = std::make_shared<CompiledWorkflow>(std::move(*sub_wf));
  std::string const mode = n.subflow_mode.empty() ? "sync" : n.subflow_mode;
  ctx.graph.add_node(
      n.id, [child, mode, copy_state = n.copy_state, input = n.sub_input, spec = n.sub_workflow,
             node_id = n.id](Json const& state, NodeContext&) -> Command {
        // 子初始 state：config 继承（主 config 原样给子，再并入**子自己的声明缺省** + 运行变量）
        // → copy_state 拷贝 → input 解析（右胜覆盖）。子 config 里的 runId/workspace 由主 fire
        // 预留（主合并后的 config 含 runId），故 inject_run_vars 在子侧同样成义。
        Json cfg = state.contains(kConfigKey) && state[kConfigKey].is_object()
                       ? state[kConfigKey].get<Json>()
                       : Json::object();
        merge_declared_config(*child, cfg);
        inject_run_vars(cfg, cfg);
        Json sub_state{{kConfigKey, std::move(cfg)}};
        for (std::string const& k : copy_state)
          if (state.contains(k)) sub_state[k] = state[k];
        Json const inp = resolve_parameter(input, state);
        if (inp.is_object())
          for (auto it = inp.begin(); it != inp.end(); ++it) sub_state[it.key()] = it.value();

        if (mode == "detach") {
          // fire-and-forget：立即返回 detached；后台线程跑子图并落盘审计（workspace 缺失则跳过）。
          std::string const workdir = [&] {
            Json const cfg = sub_state.value(kConfigKey, Json::object());
            return cfg.is_object() && cfg.contains("workspace") && cfg["workspace"].is_string()
                       ? cfg["workspace"].get<std::string>()
                       : std::string{};
          }();
          std::thread t([child, sub_state = std::move(sub_state), spec, node_id, workdir]() {
            try {
              CompiledWorkflow run = *child;  // 每个 detach 独立拷贝 → 并发调用互不干扰
              std::vector<graph::StreamEvent> events;
              graph::InvokeOptions iopts;
              iopts.mode = graph::EventMode::All;
              iopts.sink = [&events](graph::StreamEvent const& ev) { events.push_back(ev); };
              auto const r = run.graph.invoke(sub_state, "", iopts);
              if (!workdir.empty()) {
                Json audit = r ? Json{{"ok", true}, {"flow", spec},
                                      {"response", *r}, {"state", *r}, {"events", Json::array()}}
                               : Json{{"ok", false}, {"flow", spec}, {"error", r.error().message}};
                if (audit.contains("events")) {
                  Json evs = Json::array();
                  for (graph::StreamEvent const& e : events)
                    evs.push_back(Json{{"node", e.node}, {"kind", e.type}, {"step", e.step}});
                  audit["events"] = std::move(evs);
                }
                write_json_file(std::filesystem::path(workdir) / ".wfr" / "subflows" /
                                    (spec + ".json"),
                                audit);
              }
            } catch (...) {  // 后台线程 panic/异常不得拉垮主流程（fire-and-forget 也要可观测）
              if (!workdir.empty()) {
                Json audit{{"ok", false}, {"flow", spec}, {"error", "detached thread threw"}};
                write_json_file(std::filesystem::path(workdir) / ".wfr" / "subflows" /
                                    (spec + ".json"),
                                audit);
              }
            }
          });
          t.detach();
          Json update{{kResultsKey,
                       Json{{node_id, Json{{"detached", true},
                                          {"_proxy", Json{{"mode", "detach"}, {"workflow", spec}}}}}}},
                      {kLastExecKey, node_id}};
          return Command::with_update(std::move(update));
        }

        // sync：同步跑完，把子最终 state 的 results 通道键提升为帧顶层键。
        CompiledWorkflow run = *child;
        auto const r = run.graph.invoke(sub_state, "", graph::InvokeOptions{});
        if (!r)
          throw tool::ToolException("subworkflow 节点 '" + node_id + "' 子 workflow '" + spec +
                                    "' 执行失败: " + r.error().message);
        Json frame;
        if (r->contains("results") && (*r)["results"].is_object())
          frame = (*r)["results"];  // 子 workflow 各节点结果 → 帧顶层键（$<id>.result.<子节点id>）
        else
          frame = Json::object();
        frame["_proxy"] = Json{{"mode", "sync"}, {"workflow", spec},
                               {"ok", true}, {"state", *r}};
        Json update{{kResultsKey, Json{{node_id, std::move(frame)}}}, {kLastExecKey, node_id}};
        return Command::with_update(std::move(update));
      });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ---- plan：产出/校验 state.plan（default_plan 预置 或 模型生成）----
void v_plan(NodeIR const& n, NodeType::ValidateContext const&,
            NodeType::ErrorSink const& err) {
  if (!n.default_plan.is_null()) {
    if (!n.default_plan.is_object() || !n.default_plan.contains("steps")) {
      err("VN20", "plan 节点 '" + n.id + "' 的 default_plan 必须是 {goal, steps:[…]}");
      return;
    }
    std::string why;
    if (!validate_plan_steps(n.default_plan["steps"], why))
      err("VN20", "plan 节点 '" + n.id + "' 的 default_plan 非法：" + why);
  } else if (n.model.empty()) {
    err("VN20", "plan 节点 '" + n.id + "' 必须给 default_plan 或 model（模型生成）之一");
  }
  if (n.next.empty()) err("VN20", "plan 节点 '" + n.id + "' 缺少 next");
}

void c_plan(NodeIR const& n, NodeType::CompileContext& ctx) {
  model::ChatModel* m = (ctx.components != nullptr) ? ctx.components->model(n.model) : nullptr;
  std::string const node_id = n.id;
  nlohmann::json const default_plan = n.default_plan;
  std::string const sys = n.system_prompt;
  std::string const goal_spec = n.goal;  // 目标，可写 @config.*
  ctx.graph.add_node(n.id, [m, default_plan, sys, goal_spec, node_id](Json const& state,
                                                                      NodeContext&) {
    Json plan = Json();  // null = 未产出
    std::string action = "adopted";
    (void)action;
    // @config.plan_steps（三级对照用）：显式注入步骤数组（JSON 字符串）优先于 default_plan
    Json const cfg_steps = resolve_parameter(Json("@config.plan_steps"), state);
    if (cfg_steps.is_string() && !cfg_steps.get<std::string>().empty()) {
      auto const parsed = nlohmann::json::parse(cfg_steps.get<std::string>(),
                                                nullptr, /*allow_exceptions=*/false);
      std::string why;
      if (!parsed.is_discarded() && parsed.is_array() && validate_plan_steps(parsed, why)) {
        plan = normalize_plan("", parsed);
        action = "config_steps";
      } else {
        action = "config_invalid:" + why;
      }
    } else if (default_plan.is_object() && default_plan.contains("steps")) {
      std::string goal = default_plan.value("goal", "");
      if (!goal_spec.empty()) {
        Json const g = resolve_parameter(Json(goal_spec), state);
        if (g.is_string()) goal = g.get<std::string>();
      }
      plan = normalize_plan(goal, default_plan["steps"]);
    } else if (m != nullptr) {
      std::string target = resolve_text_parameter("@config.goal", state);
      if (target.empty()) {  // 缺省目标 = 最后一条 human 消息（wfr 的输入即目标）
        auto const msgs = messages_from_state(state);
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
          if (it->type == message::Role::Human) { target = it->text(); break; }
      }
      message::Message const sys_msg = message::system_message(
          sys.empty()
              ? "你是任务规划器。只输出 JSON：{\"steps\":[{\"id\":\"s1\",\"goal\":\"一句可执行的步骤\","
                "\"deps\":[],\"verify\":null}]}。goal 要具体可执行。除 JSON 外不要输出任何字。"
              : sys);
      message::Message const usr =
          message::human_message(target.empty() ? "请规划一个简短任务（2~3 步）" : target);
      auto resp = m->invoke({sys_msg, usr}, {});
      if (resp) {
        std::string const text = resp->text();
        if (auto const j = json_block_of(text); j.has_value()) {
          Json const steps = j->value("steps", Json::array());
          std::string why;
          if (validate_plan_steps(steps, why)) { plan = normalize_plan(target, steps); action = "model"; }
          else action = "model_invalid:" + why;
        } else action = "model_unparsed";
      } else action = "model_error";
    } else action = "no_source";

    Json update = Json::object();
    Json results = Json::object();
    if (!plan.is_null()) {
      // A1：@config.plan_strategy（JSON 字符串）覆盖计划策略（on_fail / max_auto_rollback）
      Json const cfg = resolve_parameter(Json("@config.plan_strategy"), state);
      if (cfg.is_string() && !cfg.get<std::string>().empty()) {
        auto const ps = nlohmann::json::parse(cfg.get<std::string>(), nullptr, /*allow_exceptions=*/false);
        if (!ps.is_discarded() && ps.is_object()) plan["strategy"] = ps;
      }
      update["plan"] = plan;
    }
    results[node_id] = Json{{"action", action},
                            {"steps", plan.is_null() ? 0LL : static_cast<long>(plan["steps"].size())}};
    update[kResultsKey] = std::move(results);
    return Command::with_update(std::move(update));
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ---- verify：每步门。announce=把当前步骤喂给模型 + in_progress；judge=判 done/fail + 推进 cursor；
//      review（wfr-㊾）=批级独立终审：读整份 plan + plan_log 判定，不读执行思考（输入隔离）----
// A6（problems/39）：verify kind=agent 判据函数前置声明（定义在其后；c_verify 用它须先见声明）。
struct AgentReviewOutcome {
  std::string verdict;    // "done" | "fail" | "unverified"
  std::string evidence;
  bool advance;
};
AgentReviewOutcome judge_agent_review(Json const& step, Json const& v, Json const& state,
                                      model::ChatModel* m, tool::ToolRegistry const* ro,
                                      std::string const& judge_sys);

void v_verify(NodeIR const& n, NodeType::ValidateContext const&,
              NodeType::ErrorSink const& err) {
  if (n.phase != "announce" && n.phase != "judge" && n.phase != "review")
    err("VN20", "verify 节点 '" + n.id + "' 的 phase 必须为 announce|judge|review，收到 '" + n.phase + "'");
  if (n.next.empty()) err("VN20", "verify 节点 '" + n.id + "' 缺少 next");
}

void c_verify(NodeIR const& n, NodeType::CompileContext& ctx) {  std::string const node_id = n.id;
  std::string const phase = n.phase;
  // wfr-㊹ 批④：做完后 goto _break（引擎 goto 容器名解析已支持）
  std::string const break_target = ctx.goto_resolve ? ctx.goto_resolve(kBreakName) : kBreakName;
  // L3 裁判模型（批③）：verify 节点的 model 组件；缺省用 main（wfr 装配时指到"main"）
  model::ChatModel* judge =
      (ctx.components != nullptr) ? ctx.components->model(n.model) : nullptr;
  std::string const judge_sys = n.system_prompt;
  std::string const judge_role = n.role;
  std::string const batch_criteria = n.criteria;  // review 相位的批级判据（可 @config.*）
  // A6（problems/39）：验收者只读工具集 —— 从 toolset 组件名解析，只拷贝 read_only 工具。
  // 写工具不在 registry 里 → 模型发不出写调用（"不存在而非被拒"）。
  auto ro_registry = std::make_shared<tool::ToolRegistry>();
  if (ctx.components != nullptr && !n.toolset.empty())
    if (auto const* ts = ctx.components->toolset(n.toolset); ts != nullptr)
      for (std::string const& tname : *ts)
        if (tool::ToolSpec const* sp = ctx.registry->find(tname); sp != nullptr)
          if (sp->read_only) ro_registry->add(*sp);  // 只读才收；写工具跳过（不给验收者）
  ctx.graph.add_node(n.id, [node_id, phase, judge, judge_sys, judge_role, batch_criteria,
                            ro_registry,
                            break_target](Json const& state, NodeContext&) {
    Json plan = plan_from_state(state);
    Json steps = plan.contains("steps") && plan["steps"].is_array() ? plan["steps"] : Json::array();
    long cursor = plan.value("cursor", 0L);

    if (phase == "announce") {
      if (cursor < 0 || cursor >= static_cast<long>(steps.size()))
        return Command{{}, {break_target}, {}};  // 全部完成 → 跳出循环（引擎解析 _break）
      Json step = steps[static_cast<std::size_t>(cursor)];
      std::string const id = step.value("id", "s" + std::to_string(cursor + 1));
      std::string const goal = step.value("goal", "");
      std::string const vd =
          step.contains("verify") && step["verify"].is_object()
              ? std::string("（验证:" + step["verify"].value("kind", "?") + "）")
              : std::string();
      std::string const text = "📋 执行步骤 " + std::to_string(cursor + 1) + "/" +
                               std::to_string(steps.size()) + " [" + id + "]：" + goal + vd;
      steps[static_cast<std::size_t>(cursor)]["status"] = "in_progress";
      plan["steps"] = std::move(steps);
      Json update{{kPlanStateKey, plan},
                  {kMessagesKey, Json::array({message::human_message(text)})},
                  {kResultsKey, Json{{node_id, Json{{"phase", "announce"}, {"step", id}}}}}};
      return Command::with_update(std::move(update));
    }

    Json plan_log = state.contains(kPlanLogStateKey) && state[kPlanLogStateKey].is_array()
                        ? state[kPlanLogStateKey] : Json::array();

    // ---- review（wfr-㊾）：批级独立终审（Reviewer 角色）----
    // 不做 cursor 推进、不改步骤状态；只读整批计划 + plan_log，产出批级 verdict 落 plan_log。
    if (phase == "review") {
      std::string criteria = batch_criteria;
      if (criteria.empty()) criteria = plan.value("goal", "");
      else {
        std::string const eff = resolve_text_parameter(batch_criteria, state);
        if (!eff.empty()) criteria = eff;
      }
      std::string why;
      bool const ok = judge_batch_review(plan, plan_log, criteria, judge, judge_sys, why);
      Json verdict_obj = Json{{"step", "*"}, {"kind", "review"},
                              {"verdict", ok ? "done" : "fail"}, {"why", why}};
      if (!judge_role.empty()) verdict_obj["role"] = judge_role;
      plan_log.push_back(std::move(verdict_obj));
      // review 不改 cursor/steps，批级判定只落 plan_log（facts）；results 里透出 verdict 供宿主/调试台读
      Json update{{kPlanLogStateKey, std::move(plan_log)},
                  {kResultsKey, Json{{node_id, Json{{"phase", "review"},
                                                    {"verdict", ok ? "done" : "fail"}}}}}};
      return Command::with_update(std::move(update));
    }

    // ---- judge ----
    if (cursor < 0 || cursor >= static_cast<long>(steps.size()))
      return Command::with_update(Json{});  // 防御：无步可判（正常由 loop max 收口）
    Json const step = steps[static_cast<std::size_t>(cursor)];
    std::string const id = step.value("id", "?");

    bool const has_verify = step.contains("verify") && step["verify"].is_object() &&
                            step["verify"].contains("kind");
    Json verdict_obj;
    bool advance = true;
    if (has_verify) {
      Json const v = step["verify"];
      std::string const kind = v.value("kind", "");
      if (kind == "probe" || kind == "rule") {
        // 批②/③：kind=probe|rule → 确定性判据（不调 LLM）；kind=model → 裁判模型（批③）
        std::string why;
        bool const ok = verify_rule_check(v, state, why);
        verdict_obj = Json{{"step", id}, {"kind", kind}, {"verdict", ok ? "done" : "fail"},
                          {"why", ok ? "规则判据通过：" + why : "规则判据失败：" + why}};
        advance = ok;
      } else if (kind == "model") {
        // 批③ L3 LLM 档：规则先过（本步无规则）→ 裁判模型背书 Pass/Fail+feedback。
        // 注入面按 23 纪律：judge 提示词固定、输入 = 步骤目标 + 本步执行过程的文本（不给自由发挥入口）。
        std::string why;
        bool const ok = judge_model_review(step, v, state, judge, judge_sys, why);
        verdict_obj = Json{{"step", id}, {"kind", "model"}, {"verdict", ok ? "done" : "fail"},
                          {"why", why}};
        advance = ok;
      } else if (kind == "agent") {
        // A6（problems/39 优先级 1）：独立验收 subagent（只读工具真检查）。
        // 三态 done/fail/unverified；判不了 → unverified + 推进（诚实标注，K2 fail-open 免疫）。
        auto const r = judge_agent_review(step, v, state, judge, ro_registry.get(), judge_sys);
        verdict_obj = Json{{"step", id}, {"kind", "agent"}, {"verdict", r.verdict},
                          {"why", r.evidence}, {"role", judge_role}};
        advance = r.advance;
      } else {
        advance = false;
        verdict_obj = Json{{"step", id}, {"kind", kind}, {"verdict", "fail"},
                          {"why", "未知 verify kind: " + kind}};
      }
    } else {
      // wfr-㊹ 治理（problems/33）：无 verify 判据 → **unverified**（完成但未验证，L1 语义升级）。
      // 不再当 done 静默通过 —— Reviewer 批级终审可据此问责（总纲 G5 裁决：ReAct+ANSWER 诚实标未验证）。
      verdict_obj = Json{{"step", id}, {"kind", "none"}, {"verdict", "unverified"},
                         {"why", "L1 无验证（无判据，诚实标注 unverified）"}};
    }
    // wfr-㊾：judge 落行的"谁判的"（审计角色），非空才写（既有 workflow 不带 role，不破坏）
    if (!judge_role.empty()) verdict_obj["role"] = judge_role;
    // A1（收尾）：strategy.on_fail == rollback_deps 且未达上限 → 失败时**自动**回退到依赖步重做。
    bool auto_rolled = false;
    if (!advance) {
      Json const strategy = plan.contains("strategy") && plan["strategy"].is_object()
                                ? plan["strategy"] : Json::object();
      long const cap = strategy.value("max_auto_rollback", 1L);
      long const used = plan.value("auto_rollbacks", 0L);
      if (strategy.value("on_fail", "retry") == "rollback_deps" && used < cap) {
        long target = 0;
        auto const& deps = step.contains("deps") && step["deps"].is_array() ? step["deps"]
                                                                             : Json::array();
        long min_idx = -1;
        if (deps.is_array()) {
          for (std::size_t i = 0; i < steps.size(); ++i)
            if (steps[i].is_object() && steps[i].contains("id"))
              for (auto const& d : deps)
                if (d.is_string() && steps[i]["id"].get<std::string>() == d.get<std::string>())
                  if (min_idx < 0 || static_cast<long>(i) < min_idx) min_idx = static_cast<long>(i);
        }
        if (min_idx >= 0) target = min_idx;
        if (0 <= target && target <= cursor) {
          std::vector<std::string> rolled;
          for (long i = target; i <= cursor; ++i) {
            std::string const st = steps[static_cast<std::size_t>(i)].value("status", "pending");
            if (st != "pending") rolled.push_back(steps[static_cast<std::size_t>(i)].value("id", "?"));
            steps[static_cast<std::size_t>(i)]["status"] = "pending";  // 从依赖步重做
          }
          cursor = target;
          plan["auto_rollbacks"] = used + 1;
          plan_log.push_back(Json{{"kind", "rollback"}, {"action", "auto_rollback_deps"},
                                  {"to", target}, {"reason", verdict_obj.value("why", "")},
                                  {"rolled", rolled}});
          auto_rolled = true;
        }
      }
    }
    if (!auto_rolled) {
      // 治理（problems/33）：advance 时 verdict=done→done、unverified→unverified（完成但未验证）。
      // A6：agent 判据判不了也产 unverified（推进+诚实标注）；无判据档 kind=none 同。
      // A7：impossible（做不到）→ 独立档（区别于 failed，供 Reviewer/plan_log 区分"重做无望"）。
      bool const unverified = verdict_obj.value("verdict", "") == "unverified";
      bool const impossible = verdict_obj.value("verdict", "") == "impossible";
      if (advance)
        steps[static_cast<std::size_t>(cursor)]["status"] = unverified ? "unverified" : "done";
      else if (impossible)
        steps[static_cast<std::size_t>(cursor)]["status"] = "impossible";
      else
        steps[static_cast<std::size_t>(cursor)]["status"] = "failed";
    }
    if (advance) cursor += 1;
    plan["steps"] = std::move(steps);
    plan["cursor"] = cursor;
    // 先取 verdict（plan_log.push_back 会 move 走 verdict_obj → 之后不能再读它）
    std::string const final_verdict = verdict_obj.value("verdict", "");
    plan_log.push_back(std::move(verdict_obj));
    // 治理（problems/33）：results 发布 verdict/outcome，供 expr 门（jgate/rgate）引用。
    // outcome 用 auto_retry 区分 rollback_deps（否则被 jgate 误送 _fail）。
    // A7：impossible 独立第 5 值（≠ fail，jgate 的 `outcome=="fail"` 不会误送 _fail）。
    Json const outcome =
        auto_rolled ? "auto_retry"
                    : (advance ? (final_verdict == "unverified" ? "unverified" : "done")
                               : (final_verdict == "impossible" ? "impossible" : "fail"));
    Json update{{kPlanStateKey, plan}, {kPlanLogStateKey, std::move(plan_log)},
                {kResultsKey, Json{{node_id, Json{{"phase", "judge"}, {"cursor", cursor},
                                                   {"count", plan["steps"].size()},
                                                   {"verdict", outcome},
                                                   {"outcome", outcome}}}}}};
    (void)cursor;
    return Command::with_update(std::move(update));  // 静态 next=_continue → 回 loop 头；max=steps 收口
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ================= wfr-㊾ 批③：parallel + executor（Send 并行波 的 workflow 表达，arch 21 §四批③）=================
// 三个缺口（批② 收口）：
//   · workflow 层此前没有任何节点能产 Command.sends —— wave 只在 graph 层被 native 节点触发。
//   · 波成员 = 单节点 fn 的一次调用，现有 model/run_tools/verify 拆开的子链放不进波（子链在
//     join 后串行）。复合 worker 必须"一次调用"里完成 模型→工具→自查。
//   · 弧线限制：plan_log 无 reducer（compiler.cpp 只给 results/loop_counts/messages 配了），
//     wave 内多个 Executor 写 plan_log → ConcurrentWriteConflict。所以 Executor **不写
//     plan/plan_log**，各写 handoff.<step_id>（merge_slots）+ messages + results；`parallel`
//     收拢时**唯一串行单写者**折叠进 plan/plan_log（避免给 plan_log 加 append reducer 破坏
//     plan_review/verify 的整数组写回 → 每行双份）。
// 确定性契约：wave join 后按声明序归约；parallel 串行收拢 → max_parallel=1 vs >1 最终 state 逐键相等。

// 与 judge_model_review（L1620）同款 JSON 契约（{"pass":bool,"feedback":str}，fail-closed），但
// 输入**全显式**（goal/criteria/step_text 由 parallel 塞进 Send partial）——wave 成员只见局部
// state，`judge_model_review` 读 messages_from_state 会拿到空/错上下文，不能复用。
bool judge_step_self(std::string const& goal, std::string const& criteria,
                     std::string const& step_text, model::ChatModel* m,
                     std::string const& judge_sys, std::string& why) {
  if (m == nullptr) { why = "executor 自查模型未配置（executor 节点缺 model）"; return false; }
  message::Message const sys = message::system_message(
      judge_sys.empty()
          ? "你是任务步骤验收员。依据【验收标准】判断【执行记录】是否达成【步骤目标】。"
            "只输出 JSON：{\"pass\":true或false,\"feedback\":\"一句理由\"}，不要输出别的。"
          : judge_sys);
  message::Message const usr =
      message::human_message("【步骤目标】" + goal + "\n【验收标准】" + criteria +
                             "\n【执行记录】\n" + step_text);
  auto resp = m->invoke({sys, usr}, {});
  if (!resp) { why = "executor 自查调用失败：" + resp.error().message; return false; }
  auto const j = json_block_of(resp->text());
  if (!j.has_value()) { why = "executor 自查未返回可解析 JSON"; return false; }
  if (!j->contains("pass")) { why = "executor 自查缺 pass 字段"; return false; }
  bool const pass = j->value("pass", false);
  why = std::string(pass ? "executor 自查通过" : "executor 自查不通过")
        + (j->contains("feedback") && (*j)["feedback"].is_string()
               ? std::string("：") + (*j)["feedback"].get<std::string>() : std::string());
  return pass;
}

// ---- A6（problems/39 优先级 1）：verify kind=agent 独立验收 subagent ----
// 治 K1（判官只读 transcript 的洞）：验收者**带只读工具真读产物/真跑检查**，返回三态
// done/fail/unverified。来源：discuss 19 A6/A4（agent 型钩子，"verification requires
// inspecting actual files or test output"）。
// 实现纪律：
//   · 验收 agent 是独立 Agent（复用 agent::Agent），只给只读 registry → 写工具"不存在而非被拒"
//     （agent.cpp:327 只发 registry 里的工具，模型发不出写调用）。
//   · transcript 完全隔离：局部 msgs 调 invoke，不进 state.messages（同 judge_model_review）。
//   · 判不了/证据不足 → unverified + **推进**（对齐 problems/33 无判据档，K2 fail-open 免疫）；
//     Reviewer review 相位可查 unverified 批级兑责（governance_test.cpp:274-278 先例）。
//   · 模型/解析失败 fail-closed → fail（同 judge_model_review 纪律）。

// 同一 judge 模型实例的串行锁（subagent.cpp 的 serial_for 未公开；语义同款：
// 多个 verify 节点/并行 Executor 不并发打同一模型）。条目永不删除（model 寿命长）。
std::mutex& auditor_serial_for(model::ChatModel const* m) {
  static std::mutex guard;
  static std::unordered_map<model::ChatModel const*, std::mutex> locks;
  std::lock_guard g(guard);
  return locks[m];
}

// 验收 agent 的 system prompt（固定，无注入面）。给验收条目与 workspace，
// 它用只读工具真检查后只输出一行 JSON。
constexpr char kAuditorPrompt[] =
    "你是独立验收员。用给定的只读工具检查产物文件是否满足【验收条目】。"
    "只输出一行 JSON：{\"verdict\":\"done\"或\"fail\"或\"unverified\"或\"impossible\","
    "\"evidence\":\"具体证据（引用了哪个文件/哪次检查）\"}。"
    "证据不足判不了 → unverified；前置条件缺失/目标确实不可达成 → impossible；"
    "不得声称你无法验证的内容；不要修改任何文件。";

// 把计划里一步的产物路径展开成提示文本（paths 数组；缺省只给 workspace）。
std::string auditor_paths_hint(Json const& v, Json const& state) {
  std::string ws = as_string(resolve_parameter(Json("@config.workspace"), state));
  if (v.contains("paths") && v["paths"].is_array()) {
    std::string hint;
    for (auto const& p : v["paths"])
      if (p.is_string()) hint += "\n  - " + p.get<std::string>();
    return "【工作目录】" + ws + "\n【产物路径】" + hint;
  }
  return "【工作目录】" + ws;
}

AgentReviewOutcome judge_agent_review(Json const& step, Json const& v, Json const& state,
                                      model::ChatModel* m, tool::ToolRegistry const* ro,
                                      std::string const& judge_sys) {
  if (m == nullptr) return {"fail", "验收模型未配置（verify 节点缺 model）", false};
  std::string const request =
      "【步骤目标】" + step.value("goal", "") +
      "\n【验收条目】" + v.value("criteria", "") +
      "\n" + auditor_paths_hint(v, state) +
      "\n请只读检查产物是否满足条目，输出三态 JSON。";
  // 独立验收 agent：现构 + 串行（同 subagent.cpp:53 语义）。transcript 不进主 state。
  std::lock_guard lock(auditor_serial_for(m));
  agent::Agent auditor(*m, *ro, agent::AgentOptions{
                                     .system_prompt = judge_sys.empty() ? kAuditorPrompt
                                                                         : judge_sys,
                                     .max_iterations = 8,  // 对齐 A6 "最多 50 轮"精神，落地缩 8
                                 });
  auto ret = auditor.invoke({message::human_message(request)});
  if (!ret) return {"fail", "验收 agent 调用失败：" + ret.error().message, false};
  auto const j = json_block_of(ret->text());
  if (!j.has_value()) return {"unverified", "验收者未返回可解析 JSON", true};
  std::string const verdict = j->value("verdict", "");
  std::string const evidence =
      j->contains("evidence") && (*j)["evidence"].is_string()
          ? (*j)["evidence"].get<std::string>()
          : std::string();
  if (verdict == "done") return {"done", "验收通过：" + evidence, true};
  if (verdict == "fail") return {"fail", "验收不通过：" + evidence, false};
  if (verdict == "impossible") return {"impossible", "验收判定不可达成：" + evidence, false};  // A7：做不到→停+独立档
  return {"unverified", "验收者判定未决：" + evidence, true};  // 判不了 → 推进+诚实标注
}

// 本步 AI 消息（含其 tool_calls 的 args）里 file_write 的目标路径 → 交接件 artifacts。
// 只认 name=="file_write" 且 args.path 是字符串（mch 并行 Executor 的正确落盘形态：产物即交接者）。
std::vector<std::string> artifacts_from_ai(message::Message const& ai) {
  std::vector<std::string> out;
  for (message::ToolCall const& tc : ai.tool_calls)
    if (tc.name == "file_write" && tc.args.is_object())
      if (auto const it = tc.args.find("path"); it != tc.args.end() && it->is_string())
        out.push_back(it->get<std::string>());
  return out;
}

// ---- executor（复合 worker）：一次节点调用完成 模型→工具→自查，只写自属键 ----
void v_executor(NodeIR const& n, NodeType::ValidateContext const& ctx,
                NodeType::ErrorSink const& err) {
  if (n.model.empty()) err("VN22", "executor 节点 '" + n.id + "' 缺少 model（组件名）");
  else if (ctx.components != nullptr && ctx.components->model(n.model) == nullptr)
    err("VN22", "executor 节点 '" + n.id + "' 引用的模型组件 '" + n.model + "' 不在注册表里");
  if (n.next.empty()) err("VN22", "executor 节点 '" + n.id + "' 缺少 next");
}

void c_executor(NodeIR const& n, NodeType::CompileContext& ctx) {
  model::ChatModel* m =
      (ctx.components != nullptr) ? ctx.components->model(n.model) : nullptr;
  if (m == nullptr) {
    ctx.error = "executor 节点 '" + n.id + "' 引用的组件 '" + n.model + "' 不可用";
    return;
  }
  Json tools_json = Json::array();
  if (!n.toolset.empty() && ctx.components != nullptr)
    if (auto const* ts = ctx.components->toolset(n.toolset); ts != nullptr)
      for (std::string const& tname : *ts)
        if (auto const* sp = ctx.registry->find(tname); sp != nullptr)
          tools_json.push_back(sp->to_openai());

  // 并行 LLM 调用的串行化：模型组件是编译期共享指针，curl multi 句柄非线程安全。
  // 诚实的取舍：LLM 调用步间串行（模型收拢是单写者），文件/工具步骤仍并行（波内并发）。
  // wfr-㊸ 并行 admin 的 ExecutionGuard.audit 也要互斥（main.cpp 的 audit lambda 无锁改 tmp）。
  std::shared_ptr<std::mutex> model_mu = std::make_shared<std::mutex>();
  tool::ExecutionGuard const* guard = ctx.guard;
  tool::ToolRegistry const* reg = ctx.registry;
  std::string const sys = n.system_prompt;
  std::string const role = n.role;
  auto sink_p = ctx.sink;  // NodeLog（docs/12 §6.4）

  ctx.graph.add_node(
      n.id, [m, tools_json, sys, role, guard, reg, model_mu, sink_p, node_id = n.id](
                Json const& state, NodeContext&) {
    // Send 局部输入：parallel 扇出时塞的 {step_id, goal, criteria, dep_artifacts, status}
    std::string const step_id = state.value("step_id", "");
    std::string const step_goal = state.value("goal", "");
    std::string const criteria = state.value("criteria", "");
    std::vector<std::string> dep_artifacts;
    if (state.contains("dep_artifacts") && state["dep_artifacts"].is_array())
      for (auto const& a : state["dep_artifacts"])
        if (a.is_string()) dep_artifacts.push_back(a.get<std::string>());

    // 组上下文：本步目标 + 上一步产物路径（dep_artifacts）→ 模型可 file_read 它们续做。
    // **不读全局 messages**（wave 成员只见 Send partial；读全局 = 空上下文静默错）。
    std::vector<message::Message> msgs;
    if (!sys.empty()) msgs.push_back(message::system_message(sys));
    std::string brief = "【当前步骤】" + step_goal;
    if (!dep_artifacts.empty()) {
      brief += "\n【上一步产物，可先 file_read】";
      for (std::string const& a : dep_artifacts) brief += "\n  - " + a;
    }
    msgs.push_back(message::human_message(brief));

    // 复合 worker = 单节点内的**多轮 agent loop**（模型 → 工具 → 再模型，≤max_rounds）。
    // 对比串行 path 的 model→route→approve→act→judge：executor 把一轮"调用+执行"压缩进
    // 单节点（wave 成员=单节点 fn 一次调用）。多轮让"取数 → 落盘"这类两步工作流能在一次
    // executor 调用内完成（否则第一轮只回计划、不动手，产物不落盘——上一版真机暴露的坑）。
    // 诚实的取舍：LLM 调用在 wave 内 **mutex 串行**（curl multi 句柄非线程安全），不同步的
    // 工具/文件步骤仍并行（波内并发）；多轮同为一个 step 服务，步间仍是并行的。
    model::CallOptions co;
    co.tools = tools_json;
    co.reasoning_echo = model::ReasoningEcho::Drop;  // 并行可回传可关；默认 drop（对齐 wave 语义）
    int const max_rounds = 4;                        // 单 step 内模型往返上限（防病态循环）
    std::vector<message::Message> updates;
    std::string step_text;
    std::vector<std::string> all_artifacts;      // 所有轮次 AI 的 file_write 产物路径（交接件引用）
    tool::ToolNode tn(reg);

    for (int round = 0; round < max_rounds; ++round) {
      std::unique_lock<std::mutex> lk(*model_mu);  // curl multi 非线程安全 → 串行化模型调用
      auto rr = m->invoke(msgs, co);
      lk.unlock();
      if (!rr) throw tool::ToolException("executor 节点 '" + node_id + "' 模型调用失败：" +
                                         rr.error().message);
      // 回传会话（add_messages）：AI 消息 + 后续工具消息
      updates.push_back(*rr);
      for (std::string const& a : artifacts_from_ai(*rr)) all_artifacts.push_back(a);
      // 本轮 AI 也追加进模型上下文（否则下一轮只看到 tool 结果、缺自己的 AI 消息 → 断链）
      msgs.push_back(*rr);
      std::string const ai_text = rr->text();
      if (!ai_text.empty()) {
        if (!step_text.empty()) step_text += "\n";
        step_text += ai_text;
      }
      for (auto const& blk : rr->content)
        if (auto const* rb = std::get_if<message::ReasoningBlock>(&blk)) {
          if (!step_text.empty()) step_text += "\n";
          step_text += rb->reasoning;
        }

      if (rr->tool_calls.empty()) break;  // 模型给出最终答复（不再调工具）→ 本轮完成

      // 执行本轮的 tool_calls（guard 防护，同 c_run_tools 的 prepare/run）
      std::vector<message::Message> tool_msgs;
      if (guard == nullptr) {
        tool_msgs = tn.run(*rr);
      } else {
        std::size_t const width = std::max<std::size_t>(1, tn.options().max_parallel);
        std::size_t const N = rr->tool_calls.size();
        struct P { tool::ExecutionGuard::Prepared prep; tool::ToolSpec const* spec = nullptr; };
        std::vector<P> plans(N);
        for (std::size_t i = 0; i < N; ++i) {
          plans[i].spec = reg ? reg->find(rr->tool_calls[i].name) : nullptr;
          plans[i].prep = guard->prepare(rr->tool_calls[i], plans[i].spec, "model", false);
        }
        tool_msgs.resize(N);
        for (std::size_t base = 0; base < N; base += width) {
          std::size_t const end = std::min(base + width, N);
          std::vector<std::future<message::Message>> batch;
          batch.reserve(end - base);
          for (std::size_t i = base; i < end; ++i)
            batch.push_back(std::async(std::launch::async, [&, i] {
              if (!plans[i].prep.execute) return plans[i].prep.report;
              return guard->run(rr->tool_calls[i], plans[i].spec, rr->tool_calls[i].args);
            }));
          for (std::size_t i = base; i < end; ++i) tool_msgs[i] = batch[i - base].get();
        }
      }
      for (message::Message& tm : tool_msgs) {
        for (auto& blk : tm.content)
          if (auto* tb = std::get_if<message::TextBlock>(&blk)) message::sanitize_utf8(tb->text);
        // 工具往返结果先**副本**追加给下一轮模型的上下文（msgs），再 move 进 updates：
        // 同一对象 move 进 updates 后 `tm` 已空，直接 push 会给下一轮空 Tool 消息。
        msgs.push_back(tm);
        updates.push_back(std::move(tm));
        // 执行记录文本 = 工具结果（回喂给自查裁判 / 评审）
        std::string const t = msgs.back().text();
        if (!t.empty()) {
          if (!step_text.empty()) step_text += "\n[工具] ";
          else step_text = "[工具] ";
          step_text += t;
        }
      }
    }  // for round：模型→工具迭代（多轮 agent loop 到此结束）

    // 自查三档（与 verify judge 相位同构，但输入在 Send partial 里）：
    //   1) verify.kind ∈ {probe, rule} → 确定性判据（文件存在/内容，独立于执行者自述）；
    //   2) verify.kind == model / criteria 非空 → 模型裁判（锁内调用共享模型）；
    //   3) 无 verify → L1 无验证（直接 done——诚实标注，Reviewer 可据 plan_log 判据缺失拒绝）。
    // workspace 由 parallel 塞下来（wave 成员无 config），probe 判据据此定位文件。
    Json const verify_obj =
        state.contains("verify") && state["verify"].is_object() ? state["verify"] : Json::object();
    std::string const vkind = verify_obj.value("kind", "");
    Json frame;
    if (vkind == "probe" || vkind == "rule") {
      // 构造一个带 @config.workspace 的迷你 state 供 verify_rule_check 解析
      Json mini = Json::object();
      std::string const ws = state.value("workspace", "");
      if (!ws.empty()) mini["config"] = Json{{"workspace", ws}};
      std::string why;
      bool const ok = verify_rule_check(verify_obj, mini, why);
      frame = Json{{"step", step_id}, {"goal", step_goal}, {"kind", vkind},
                   {"verdict", ok ? "done" : "fail"}, {"why", ok ? ("判据通过：" + why) : ("判据失败：" + why)}};
    } else if (!criteria.empty() || vkind == "model") {
      // 自查裁判也碰共享模型 → 同一把锁（curl multi 句柄非线程安全）
      std::lock_guard<std::mutex> jk(*model_mu);
      std::string why;
      bool const ok = judge_step_self(step_goal, criteria.empty() ? vkind : criteria,
                                      step_text, m, "", why);
      frame = Json{{"step", step_id}, {"goal", step_goal}, {"kind", "model"},
                   {"verdict", ok ? "done" : "fail"}, {"why", why}};
    } else {
      // 治理（problems/33）：并行 Executor 无判据步同样诚实标 unverified（与串行 judge 一致）
      frame = Json{{"step", step_id}, {"goal", step_goal}, {"kind", "none"},
                   {"verdict", "unverified"}, {"why", "L1 无验证（并行 Executor 无判据步）"}};
    }
    if (!role.empty()) frame["role"] = role;
    frame["artifacts"] = Json::array();  // 交接件产物引用（真机 .out/* 落盘断言的依据）
    for (std::string const& a : all_artifacts) frame["artifacts"].push_back(a);
    if (sink_p) sink_p->log_node(node_id, "info", "executor '" + node_id + "' 完成一步（并行）",
                                 Json{{"step", step_id}, {"verdict", frame["verdict"]},
                                      {"artifacts", all_artifacts.size()}});

    // 更新：本步对话（add_messages）+ handoff.<step_id>（merge_slots）+ results[exec_<step>]
    Json update{{kMessagesKey, std::move(updates)}};
    update[kHandoffStateKey] = Json{{step_id, std::move(frame)}};
    Json results = Json::object();
    results["exec_" + step_id] = Json{{"step", step_id}, {"verdict", frame["verdict"]}};
    update[kResultsKey] = std::move(results);
    return Command::with_update(std::move(update));
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ---- parallel（扇出+收拢编排节点）----
// 语义：loop body 里 `parallel(target: executor) → executor(next: parallel)` 双节点环。
// parallel 每次进入 = 收拢（把 handoff 中已完成的步折进 plan/plan_log）+ 分派（把 deps 满足且
// 未派发的步骤以 Send 扇出）。不注册静态边（同 c_loop），路由全靠 Command（sends / goto / 空）。
void v_parallel(NodeIR const& n, NodeType::ValidateContext const&,
                NodeType::ErrorSink const& err) {
  if (n.target.empty()) err("VN22", "parallel 节点 '" + n.id + "' 缺少 target（Executor 节点 id）");
}

void c_parallel(NodeIR const& n, NodeType::CompileContext& ctx) {
  // 与 switch 语义一致：parallel 是容器角色，_break 解析到循环出口（同 c_verify / c_plan_review）。
  std::string const break_target = ctx.goto_resolve ? ctx.goto_resolve(kBreakName) : kBreakName;
  std::string const target = n.target;
  std::string const node_id = n.id;
  ctx.graph.add_node(
      n.id, [node_id, target, break_target](Json const& state, NodeContext&) {
    Json plan = plan_from_state(state);
    bool const has_plan = plan.contains("steps") && plan["steps"].is_array();
    Json plan_log = state.contains(kPlanLogStateKey) && state[kPlanLogStateKey].is_array()
                        ? state[kPlanLogStateKey] : Json::array();
    Json handoff = state.contains(kHandoffStateKey) && state[kHandoffStateKey].is_object()
                       ? state[kHandoffStateKey] : Json::object();
    Json dispatched = plan.contains("par_dispatched") && plan["par_dispatched"].is_array()
                          ? plan["par_dispatched"] : Json::array();
    std::unordered_set<std::string> dispatched_set;
    for (Json const& d : dispatched)
      if (d.is_string()) dispatched_set.insert(d.get<std::string>());

    // 分支 0：无计划 → 硬错（fail-closed，不静默；parallel 依赖上游 plan/begin 产出）
    if (!has_plan)
      throw tool::ToolException("parallel '" + node_id +
                                "' 收拢：state.plan 缺失（上游 plan/begin 未产出）");

    auto steps = plan["steps"];
    long const start_cursor = std::max<long>(0L, plan.value("cursor", 0L));

    // ⓐ 收拢：把 handoff 中"已派发、且已返回"的步折进 plan/plan_log（幂等）。
    // 只折"plan_log 里尚无该步 verdict"的（重复进入的 go(parallel) 折叠 0 → 自灭）。
    bool folded_any = false;
    for (Json& s : steps) {
      if (!s.is_object()) continue;
      std::string const sid = s.value("id", "");
      if (sid.empty() || !handoff.contains(sid)) continue;
      auto const& h = handoff[sid];
      if (!h.is_object()) continue;
      bool already = false;
      for (Json const& e : plan_log)
        if (e.is_object() && e.value("step", "") == sid && e.contains("verdict")) {
          already = true; break;
        }
      if (already) continue;  // 幂等：这批早就折过
      std::string const vd = h.value("verdict", "fail");
      // 治理（problems/33）+ A7：四档 —— done / unverified（完成但未验证）/ impossible（做不到）/ failed
      s["status"] = (vd == "done") ? "done"
                  : (vd == "unverified") ? "unverified"
                  : (vd == "impossible") ? "impossible" : "failed";
      Json row = Json{{"step", sid}, {"kind", h.value("kind", "none")},
                      {"verdict", vd}, {"why", h.value("why", "")}};
      if (h.contains("role") && h["role"].is_string()) row["role"] = h["role"];
      if (h.contains("artifacts") && h["artifacts"].is_array()) row["artifacts"] = h["artifacts"];
      if (h.contains("goal") && h["goal"].is_string()) row["goal"] = h["goal"];
      plan_log.push_back(std::move(row));
      dispatched_set.erase(sid);
      folded_any = true;
    }
    if (folded_any) {
      // cursor 前移：跳过连续 done / unverified 步（完成或完成但未验证都算推进）
      long nc = start_cursor;
      while (nc < static_cast<long>(steps.size())) {
        std::string const st = steps[static_cast<std::size_t>(nc)].value("status", "pending");
        if (st != "done" && st != "unverified") break;
        ++nc;
      }
      plan["cursor"] = nc;
      plan["steps"] = std::move(steps);
      // 更新 par_dispatched（去掉已收拢的 id）
      Json nd = Json::array();
      for (std::string const& s : dispatched_set) nd.push_back(s);
      plan["par_dispatched"] = std::move(nd);
    }

    // ⓑ 全部完成 → 恰好一次 goto _break（跳出循环）。重复进入的全 done 靠 par_done marker 自灭，
    // 否则 N 个 executor 的 forward 会产生 N 个 go(parallel)，每个都 goto break → review 跑 N 次。
    bool all_done = true;
    for (Json const& s : plan["steps"])
      if (s.is_object() && s.value("status", "pending") != "done") { all_done = false; break; }
    bool const already_done = plan.value("par_done", false);
    if (all_done && !already_done) {
      plan["par_done"] = true;
      // 交接件落盘（wfr-㊾ 批③）：全批结束时的 A→B 交接件（真机对照/审计的磁盘断言）。
      // best-effort：@config.workspace 可解析且手账非空才写；幂等（par_done 恰好一次进入）。
      Json const ws = resolve_parameter(Json("@config.workspace"), state);
      std::string const task = resolve_text_parameter("@config.task", state);
      if (ws.is_string() && !plan_log.empty()) {
        Json done = Json::array();
        for (Json const& e : plan_log)
          if (e.is_object() && e.contains("step") && e["step"].is_string() &&
              e.value("step", "") != "*" && e.contains("verdict"))
            done.push_back(e);
        Json handoff_file{{"from", "executor"},
                          {"to", "reviewer"},
                          {"goal", plan.value("goal", "")},
                          {"done", std::move(done)}};
        std::filesystem::path const base = std::filesystem::path(ws.get<std::string>());
        std::filesystem::path const hf =
            base / ".wfr" / "handoffs" /
            ((task.empty() || task == "null" ? std::string("session") : task) + ".json");
        write_json_file(hf, handoff_file);
      }
      Json update{{kPlanStateKey, plan}, {kPlanLogStateKey, std::move(plan_log)},
                  {kResultsKey, Json{{node_id, Json{{"phase", "gather"},
                                                    {"verdict", "done"}, {"cursor", plan["cursor"]}}}}}};
      return Command{update, {break_target}, {}};  // goto _break（覆盖静态/条件边）
    }
    if (already_done)
      return Command::with_update(Json{});  // 已结束过 → 自灭（无出边）

    // ⓒ 死锁：还有 pending、无 deps 满足且**未派发**的步 → 硬错（不静默终止，防"永不推进"）。
    // 已派发的（dispatched_set 里有）不算死锁——它们在跑，等收拢时会推进。
    bool has_pending = false, any_dispatchable = false, any_dispatched = false;
    for (Json const& s : plan["steps"]) {
      if (!s.is_object() || s.value("status", "pending") != "pending") continue;
      has_pending = true;
      std::string const sid = s.value("id", "");
      if (dispatched_set.count(sid)) { any_dispatched = true; continue; }
      bool deps_ok = true;
      Json const ds = s.contains("deps") && s["deps"].is_array() ? s["deps"] : Json::array();
      for (Json const& d : ds) {
        if (!d.is_string()) continue;
        bool found = false;
        for (Json const& p : plan["steps"])
          if (p.is_object() && p.value("id", "") == d.get<std::string>() &&
              p.value("status", "") == "done") { found = true; break; }
        if (!found) { deps_ok = false; break; }
      }
      if (deps_ok) any_dispatchable = true;
    }
    if (has_pending && !any_dispatchable && !any_dispatched) {
      // pending 但全死锁 → 硬错（NodeError，fail-closed）——deps 环或依赖步 failed 而无法推进
      throw tool::ToolException("parallel '" + node_id +
                                "' 收拢死锁：存在 pending 步骤但无 deps 满足且未派发（deps 环或依赖失败）");
    }

    // ⓓ 分派：把 deps 满足、status pending、未派发的步以 Send 扇出
    std::vector<graph::Send> sends;
    for (Json const& s : plan["steps"]) {
      if (!s.is_object()) continue;
      std::string const sid = s.value("id", "");
      if (sid.empty() || dispatched_set.count(sid)) continue;
      if (s.value("status", "pending") != "pending") continue;
      bool deps_ok = true;
      Json const ds = s.contains("deps") && s["deps"].is_array() ? s["deps"] : Json::array();
      for (Json const& d : ds) {
        if (!d.is_string()) continue;
        bool found = false;
        for (Json const& p : plan["steps"])
          if (p.is_object() && p.value("id", "") == d.get<std::string>() &&
              p.value("status", "") == "done") { found = true; break; }
        if (!found) { deps_ok = false; break; }
      }
      if (!deps_ok) continue;
      // Send partial：Executor 的局部输入（不读全局）。criteria / workspace / verify 都在
      // **主 state** 预解析/携带（wave 成员局部 state 没有 config，@config.* 在那里解析会静默成空）。
      std::string criteria;
      Json verify_obj = Json::object();
      if (s.contains("verify") && s["verify"].is_object()) {
        verify_obj = s["verify"];
        criteria = resolve_text_parameter(s["verify"].value("criteria", ""), state);
      }
      Json const ws = resolve_parameter(Json("@config.workspace"), state);
      Json pst = Json{{"step_id", sid}, {"goal", s.value("goal", "")},
                      {"criteria", std::move(criteria)},
                      {"verify", std::move(verify_obj)}};
      if (ws.is_string()) pst["workspace"] = ws;
      Json deps_arts = Json::array();
      for (Json const& d : ds) {
        if (!d.is_string()) continue;
        if (handoff.contains(d.get<std::string>()) &&
            handoff[d.get<std::string>()].is_object())
          for (auto const& a : handoff[d.get<std::string>()].value("artifacts", Json::array()))
            if (a.is_string()) deps_arts.push_back(a);
      }
      pst["dep_artifacts"] = std::move(deps_arts);
      sends.push_back(graph::Send{target, std::move(pst)});
      dispatched_set.insert(sid);
    }
    if (!sends.empty()) {
      Json nd = Json::array();
      for (std::string const& s : dispatched_set) nd.push_back(s);
      plan["par_dispatched"] = std::move(nd);
      Json update{{kPlanStateKey, std::move(plan)},
                  {kResultsKey, Json{{node_id, Json{{"phase", "dispatch"},
                                                    {"count", sends.size()}}}}}};
      return Command{update, {}, std::move(sends)};
    }

    // ⓔ 无折叠、无分派、未全 done（等待已在跑的成员回来）→ 空 Command 自灭（无出边）
    Json update;
    if (folded_any) {
      update[kPlanStateKey] = plan;
      update[kPlanLogStateKey] = std::move(plan_log);
    }
    return Command::with_update(std::move(update));
  });
  ++ctx.registered;
  // 不注册静态边：路由全靠 Command（sends / goto _break / 空自灭）。同 c_loop 的手法。
}

// ================= wfr-㊹ 治理：tool_registry（动态工具源，problems/34）=================
// 声明一组工具源（{from,type,load}）。两种源：
//   · type="so"   —— 运行期 dlopen `lib<from>.so` 加载列出的工具进**共享 ToolRegistry**
//     （NodeContext.shared_tools 的持有者）；动态工具可顶替同名内置（mock 对照）。
//   · type="select" from="builtin" —— **仅激活**内置工具（不拷贝定义，定义留在宿主内置表），
//     load 名在编译/校验期必须能于内置注册表里解析。
// 本节点把**活动工具名单**（两类源的 load 并集）发布进 `results.<id>.active_tools`，
// think/model 与 run_tools 从 context 取（共享优先、内置兜底）；无本节点 → 零回归。
// 注意：须排在所有消费节点之前（find 指针稳定性 + 运行期先发布 active_tools）。
void v_tool_registry(NodeIR const& n, NodeType::ValidateContext const& ctx,
                     NodeType::ErrorSink const& err) {
  if (n.next.empty()) err("VN23", "tool_registry 节点 '" + n.id + "' 缺少 next");
  if (n.tool_sources.empty()) err("VN23", "tool_registry 节点 '" + n.id + "' 缺 tools（工具源声明）");
  for (auto const& s : n.tool_sources) {
    if (s.from.empty()) err("VN23", "tool_registry 节点 '" + n.id + "' 的 from 为空");
    if (s.type != "so" && s.type != "select")
      err("VN23", "tool_registry 节点 '" + n.id + "' 的 type='" + s.type +
                      "' 不在词表里（so / select）");
    if (s.type == "select" && s.from != "builtin")
      err("VN23", "tool_registry 节点 '" + n.id + "' 的 select 源 from='" + s.from +
                      "' 必须为 \"builtin\"（select 只激活内置工具，不加载动态库）");
    if (s.load.empty()) err("VN23", "tool_registry 节点 '" + n.id + "' 的 load 为空（from='" +
                                        s.from + "'）");
    if (s.type == "select" && ctx.registry != nullptr)
      for (std::string const& name : s.load)
        if (ctx.registry->find(name) == nullptr)
          err("VN23", "tool_registry 节点 '" + n.id + "' 的 select 源引用内置注册表里不存在的工具 '" +
                          name + "'");
  }
  // 排序提示（编排约定，不构成图拓扑规则）：tool_registry 应放在 workflow 起始 nodes[0]，
  // 消费节点（model/run_tools）才能在 results 里读到 active_tools。
}

void c_tool_registry(NodeIR const& n, NodeType::CompileContext& ctx) {
  tool::ToolRegistry* shared = ctx.shared_registry;
  if (shared == nullptr) {
    // 宿主没配共享注册表 → 加载无处可放（fail-closed；这是 workflow 层的装配错误）
    ctx.error = "tool_registry 节点 '" + n.id + "'：宿主未提供 shared_registry（CompileOptions）";
    return;
  }
  // 纵深防御：compile_workflow 公开路径先过 validator（同一 registry），此处兜手工构造 IR 的路径。
  if (ctx.registry != nullptr)
    for (auto const& s : n.tool_sources)
      if (s.type == "select")
        for (std::string const& name : s.load)
          if (ctx.registry->find(name) == nullptr) {
            ctx.error = "tool_registry 节点 '" + n.id + "' 的 select 源引用内置注册表里不存在的工具 '" +
                        name + "'";
            return;
          }
  auto const lib_dirs = ctx.tool_lib_dirs;  // 可为 null（只按相对/绝对路径）
  auto const sources = n.tool_sources;
  auto sink_p = ctx.sink;
  ctx.graph.add_node(n.id, [shared, lib_dirs, sources, sink_p, node_id = n.id](
                               Json const&, NodeContext&) {
    Json results = Json::object();
    Json loaded = Json::array();
    std::vector<std::string> active;  // 活动工具集：本节点所有源的 load 并集（保序去重）
    std::unordered_set<std::string> seen;
    for (auto const& s : sources) {
      if (s.type == "select") {
        // 仅激活内置（定义留内置注册表）；名字在编译/校验期已验证可解析
        for (std::string const& nm : s.load)
          if (seen.insert(nm).second) active.push_back(nm);
        continue;
      }
      // 库路径：tool_lib_dirs 里找 `lib<from>.so`；找不到 → 相对/绝对路径
      std::string const fname = "lib" + s.from + ".so";
      std::string lib_path = fname;
      if (lib_dirs != nullptr) {
        for (auto const& d : *lib_dirs) {
          std::filesystem::path const cand = std::filesystem::path(d) / fname;
          if (std::filesystem::exists(cand)) { lib_path = cand.string(); break; }
        }
      }
      auto const specs = tool::load_dynamic_tools(lib_path, s.load);
      if (!specs) {
        // fail-closed：任一工具源加载失败 → 整体失败（不部分加载）
        throw tool::ToolException("tool_registry '" + node_id + "' 加载工具源 from='" + s.from +
                                  "' 失败：" + specs.error());
      }
      for (auto const& spec : *specs) {
        shared->add(spec);  // 按名覆盖 → 重跑幂等（find 取最新）
        loaded.push_back(spec.name);
        if (seen.insert(spec.name).second) active.push_back(spec.name);
      }
    }
    Json active_json = Json::array();
    for (std::string const& a : active) active_json.push_back(a);
    if (sink_p) sink_p->log_node(node_id, "info", "tool_registry '" + node_id + "' 装配活动工具",
                                 Json{{"count", active.size()},
                                      {"loaded", loaded},
                                      {"active_tools", active_json}});
    results[node_id] = Json{{"action", "load"},
                            {"loaded", loaded},
                            {"active_tools", std::move(active_json)}};
    return Command::with_update(Json{{kResultsKey, std::move(results)}});
  });
  ctx.graph.add_edge(n.id, ctx.resolve(n.next));
  ++ctx.registered;
}

// ================= Context（wfr-context，⑤：环境 / skill 上下文注入）=================
// 运行时：env 键值对 + system 条目（from 文本 | file 读文件）→ system 消息数组，
// 写入 state[system_context]（kSystemContextKey）。model 节点装配请求时拼在系统提示
// 之后、历史之前；**不进 messages 通道/会话日志** —— 上下文只对本 run 生效，replay 不回溯。
inline constexpr std::size_t kContextFileCap = 64u * 1024;  // system[].file 的读入上限（字节）

// ---- 环境块默认键：当前时间（日期+时间+时区）与简约主机名 ----
// "env 中默认加入当前日期+时间+时区 / 当前设备的名称（简约版）"：
//   每次 context 运行都求一次值（now 是"此时此刻"）；workflow 在 env 里显式写同名键 →
//   覆盖默认。取名 now / host（模型 直觉，Feed 给模型直接可用）。
std::string current_datetime_tz() {
  std::time_t const t = std::time(nullptr);
  std::tm ltm{}, gtm{};
  ::localtime_r(&t, &ltm);
  ::gmtime_r(&t, &gtm);
  // 时区偏移用 mktime 差值求（可移植；严格 -std=c++26 下 tm_gmtoff 会被特性宏藏起）。
  std::tm const lsrc = ltm, gsrc = gtm;
  long const off = static_cast<long>(::mktime(const_cast<std::tm*>(&lsrc))) -
                   static_cast<long>(::mktime(const_cast<std::tm*>(&gsrc)));
  char date[32];
  std::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &ltm);
  char tz[16];
  std::snprintf(tz, sizeof tz, "%+03d:%02d", static_cast<int>(off / 3600),
                static_cast<int>(std::abs(off % 3600) / 60));
  return std::string(date) + " " + tz;
}

std::string short_hostname() {
  char buf[256] = {};
  if (::gethostname(buf, sizeof buf) != 0) return "localhost";
  buf[sizeof buf - 1] = '\0';
  std::string const h(buf);
  auto const dot = h.find('.');   // 简约版：只留主机名段，丢域名后缀
  return dot == std::string::npos ? h : h.substr(0, dot);
}

// UTF-8 安全截断（不切断多字节字符；上限近似按字节）
std::string utf8_cut(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

std::vector<std::string> t_context(NodeIR const& n) {
  std::vector<std::string> out;
  if (n.context_env.is_object())
    for (auto it = n.context_env.begin(); it != n.context_env.end(); ++it)
      if (it.value().is_string()) out.push_back(it.value().get<std::string>());
  if (n.context_system.is_array())
    for (Json const& e : n.context_system) {
      if (!e.is_object()) continue;
      if (e.contains("from") && e["from"].is_string()) out.push_back(e["from"].get<std::string>());
      if (e.contains("file") && e["file"].is_string()) out.push_back(e["file"].get<std::string>());
    }
  return out;
}

void v_context(NodeIR const& n, NodeType::ValidateContext const&, NodeType::ErrorSink const& err) {
  if (n.next.empty()) err("VN11", "context 节点 '" + n.id + "' 缺少 next");
  if (n.context_env.is_object()) {
    for (auto it = n.context_env.begin(); it != n.context_env.end(); ++it)
      if (!it.value().is_string())
        err("VN11", "context 节点 '" + n.id + "' 的 env['" + it.key() +
                        "'] 必须是字符串（可为 @config.* 引用）");
  }
  if (n.context_system.is_array()) {
    for (Json const& e : n.context_system) {
      if (!e.is_object()) {
        err("VN11", "context 节点 '" + n.id + "' 的 system 条目必须是对象 {from|file}");
        continue;
      }
      bool const has_from = e.contains("from") && e["from"].is_string();
      bool const has_file = e.contains("file") && e["file"].is_string();
      if (!has_from && !has_file)
        err("VN11", "context 节点 '" + n.id + "' 的 system 条目需含 from（字符串）或 file（字符串）");
    }
  }
}

void c_context(NodeIR const& n, NodeType::CompileContext& ctx) {
  Json const env = n.context_env;
  Json const sys = n.context_system;
  auto sink_p = ctx.sink;
  ctx.graph.add_node(n.id, [env, sys, sink_p](Json const& state, NodeContext& nc) {
    Json arr = Json::array();
    auto add_sys = [&arr](std::string const& text) {
      if (!text.empty()) arr.push_back(message::system_message(text));
    };
    // 1) 环境块：默认键（当前时间/主机名）先占位，workflow 显式 env 键覆盖（同键），
    //    再逐条 `key: value` 聚成一场「环境」system 消息。
    {
      std::map<std::string, std::string> pairs{{"now", current_datetime_tz()},
                                               {"host", short_hostname()}};
      if (env.is_object())
        for (auto it = env.begin(); it != env.end(); ++it) {
          if (!it.value().is_string()) continue;
          Json const v = resolve_parameter(it.value(), state);
          if (v.is_null()) continue;  // @config 缺键 → 跳过（不把 "null" 喂给模型）
          std::string const val = v.is_string() ? v.get<std::string>() : message::utf8_safe_dump(v);
          pairs[it.key()] = val;      // 显式配置覆盖默认（含同名 now/host）
        }
      if (!pairs.empty()) {
        std::string block;
        for (auto const& [k, v] : pairs) block += "- " + k + ": " + v + "\n";
        add_sys("【当前环境】\n" + block);
      }
    }
    // 2) system 条目：from = 文本引用（@config 等）；file = 读文件内容（宿主侧可信读）
    if (sys.is_array()) {
      for (Json const& entry : sys) {
        if (!entry.is_object()) continue;
        if (entry.contains("from") && entry["from"].is_string()) {
          Json const v = resolve_parameter(entry["from"], state);
          add_sys(v.is_string()   ? v.get<std::string>()
                  : v.is_null()   ? std::string{}
                  : message::utf8_safe_dump(v));
        } else if (entry.contains("file") && entry["file"].is_string()) {
          Json const v = resolve_parameter(entry["file"], state);
          std::string const path = v.is_string() ? v.get<std::string>() : "";
          if (path.empty()) continue;
          std::error_code ec;
          auto const canon = std::filesystem::weakly_canonical(path, ec);
          if (!ec && std::filesystem::is_regular_file(canon)) {
            std::ifstream in(canon, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            content = utf8_cut(content, kContextFileCap);
            add_sys("【skill: " + canon.filename().string() + "】\n" + content);
          } else if (sink_p) {
            sink_p->log_node(nc.node_name, "warn", "context: skill 文件不存在或不可读: " + path);
          }
        }
      }
    }
    if (sink_p)
      sink_p->log_node(nc.node_name, "info",
                       "context: 注入 " + std::to_string(arr.size()) + " 条 system 消息");
    if (arr.empty()) return Command{};
    return Command::with_update(Json{{kSystemContextKey, std::move(arr)}});
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
         if (node.contains("stream") && node["stream"].is_boolean()) out.stream = node["stream"].get<bool>();
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
         if (n.stream) node["stream"] = true;
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

      // ---------------- approve_tools（wfr-㊷）----------------
      // 副作用审批：deny 名单 + read_only 免审；denied → error ToolMessage 回喂。
      {NodeKind::ApproveTools, "approve_tools",
       [](Json const& n) { return kind_is(n, "approve_tools"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "next", out.next);
         if (node.contains("deny") && node["deny"].is_array())
           for (Json const& d : node["deny"])
             if (d.is_string()) out.deny.push_back(d.get<std::string>());
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "approve_tools";
         node["next"] = n.next;
         if (!n.deny.empty()) node["deny"] = n.deny;
       },
       [](NodeIR const& n) {
         std::string d = "approve_tools deny=[";
         for (std::size_t i = 0; i < n.deny.size(); ++i)
           d += (i ? "," : "") + n.deny[i];
         d += "] → " + n.next;
         return d;
       }, v_approve_tools, c_approve_tools, e_next, t_approve_tools},

      // ---------------- summarize（wfr-㊵）----------------
      // 把较早的一段历史折叠成一条 LLM 摘要；产出 fold op 落日志（重跑不重新摘要）。
      {NodeKind::Summarize, "summarize",
       [](Json const& n) { return kind_is(n, "summarize"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "model", out.model);
         get_str(out, node, "system_prompt", out.system_prompt);
         get_str(out, node, "when", out.when);
         get_str(out, node, "max_msgs", out.max_msgs);
         get_str(out, node, "max_tokens", out.max_tokens);
         get_str(out, node, "keep_last", out.keep_last);
         get_str(out, node, "style", out.style);
         get_str(out, node, "summary_role", out.summary_role);
         get_str(out, node, "on_fail", out.on_fail);
         get_str(out, node, "next", out.next);
         get_str(out, node, "reasoning_echo", out.reasoning_echo);
         if (node.contains("params") && !node["params"].is_null()) out.model_params = node["params"];
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "summarize";
         node["model"] = n.model;
         if (!n.system_prompt.empty()) node["system_prompt"] = n.system_prompt;
         for (auto const& [k, v] : std::initializer_list<std::pair<char const*, std::string const*>>{
                  {"when", &n.when}, {"max_msgs", &n.max_msgs}, {"max_tokens", &n.max_tokens},
                  {"keep_last", &n.keep_last}, {"style", &n.style},
                  {"summary_role", &n.summary_role}, {"on_fail", &n.on_fail},
                  {"reasoning_echo", &n.reasoning_echo}})
           if (!v->empty()) node[k] = *v;
         if (!n.model_params.empty()) node["params"] = n.model_params;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         return "summarize " + n.model + " when=" + (n.when.empty() ? "messages" : n.when) +
                " style=" + (n.style.empty() ? "concise" : n.style) +
                " keep=" + (n.keep_last.empty() ? "6" : n.keep_last) + " → " + n.next;
       }, v_summarize, c_summarize, e_next, t_summarize},

      // ---------------- refine_history（wfr-㊵）----------------
      // 按规则精简/裁剪历史；产出 transform op 落日志。
      {NodeKind::RefineHistory, "refine_history",
       [](Json const& n) { return kind_is(n, "refine_history"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "preset", out.preset);
         get_str(out, node, "clip_scope", out.clip_scope);
         get_str(out, node, "clip_mode", out.clip_mode);
         get_str(out, node, "tool_budget", out.tool_budget);
         get_str(out, node, "ai_budget", out.ai_budget);
         get_str(out, node, "artifact_dir", out.artifact_dir);
         get_str(out, node, "next", out.next);
         if (node.contains("rules") && node["rules"].is_array())
           for (Json const& r : node["rules"])
             if (r.is_string()) out.rules.push_back(r.get<std::string>());
         if (node.contains("categories"))
           for (Json const& c : node["categories"].is_array()
                                    ? node["categories"]
                                    : Json::array({node["categories"]}))
             if (c.is_string()) out.categories.push_back(c.get<std::string>());
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "refine_history";
         for (auto const& [k, v] : std::initializer_list<std::pair<char const*, std::string const*>>{
                  {"preset", &n.preset}, {"clip_scope", &n.clip_scope},
                  {"clip_mode", &n.clip_mode}, {"tool_budget", &n.tool_budget},
                  {"ai_budget", &n.ai_budget}, {"artifact_dir", &n.artifact_dir}})
           if (!v->empty()) node[k] = *v;
         if (!n.rules.empty()) node["rules"] = n.rules;
         if (!n.categories.empty()) node["categories"] = n.categories;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         std::string d = "refine_history";
         if (!n.preset.empty()) d += " preset=" + n.preset;
         if (!n.rules.empty()) d += " rules=" + std::to_string(n.rules.size());
         if (!n.clip_mode.empty()) d += " clip=" + n.clip_mode;
         return d + " → " + n.next;
       }, v_refine_history, c_refine_history, e_next, t_refine_history},

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

      // ---------------- context（wfr-context，⑤：环境/skill 上下文注入）----------------
      // env 键值对 + system 条目（from 文本 | file 读文件）→ system 消息写入
      // state[system_context]，由 model 节点装配请求时拼在系统提示之后。
      {NodeKind::Context, "context",
       [](Json const& n) { return kind_is(n, "context"); },
       [](NodeIR& out, Json const& node) {
         if (node.contains("env") && node["env"].is_object()) out.context_env = node["env"];
         if (node.contains("system") && node["system"].is_array())
           out.context_system = node["system"];
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "context";
         if (!n.context_env.empty()) node["env"] = n.context_env;
         if (!n.context_system.empty()) node["system"] = n.context_system;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         std::string desc;
         if (n.context_env.is_object() && !n.context_env.empty())
           desc += std::to_string(n.context_env.size()) + " env";
         if (n.context_system.is_array() && !n.context_system.empty()) {
           if (!desc.empty()) desc += " + ";
           desc += std::to_string(n.context_system.size()) + " sys";
         }
         if (desc.empty()) desc = "-";
         return "context (" + desc + ") → " + n.next;
       }, v_context, c_context, e_next, t_context},

      // ---------------- plan（wfr-㊹）：产出/校验 state.plan ----------------
      {NodeKind::Plan, "plan",
       [](Json const& n) { return kind_is(n, "plan"); },
       [](NodeIR& out, Json const& node) {
         if (!node.contains("default_plan") || node["default_plan"].is_null())
           out.default_plan = Json(nullptr);
         else
           out.default_plan = node["default_plan"];
         get_str(out, node, "goal", out.goal);       // 目标（可 @config.*）
         get_str(out, node, "model", out.model);     // 模型生成档
         get_str(out, node, "system_prompt", out.system_prompt);
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "plan";
         if (!n.default_plan.is_null()) node["default_plan"] = n.default_plan;
         if (!n.goal.empty()) node["goal"] = n.goal;
         if (!n.model.empty()) node["model"] = n.model;
         if (!n.system_prompt.empty()) node["system_prompt"] = n.system_prompt;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         std::string d = "plan goal=" + (n.goal.empty() ? "-" : n.goal);
         if (!n.default_plan.is_null() && n.default_plan.contains("steps"))
           d += " steps=" + std::to_string(n.default_plan["steps"].size());
         else if (!n.model.empty()) d += " model=" + n.model;
         return d + " → " + n.next;
       }, v_plan, c_plan, e_next, t_none},

      // ---------------- verify（wfr-㊹）：每步门 announce/judge；批级终审 review（wfr-㊾） ----------------
      {NodeKind::Verify, "verify",
       [](Json const& n) { return kind_is(n, "verify"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "phase", out.phase);
         get_str(out, node, "model", out.model);       // judge/review 的 LLM 裁判（批③ / wfr-㊾）
         get_str(out, node, "toolset", out.toolset);   // A6：验收者只读工具集组件名（kind=agent 用）
         get_str(out, node, "system_prompt", out.system_prompt);  // reviewer 独立身份（wfr-㊾）
         get_str(out, node, "role", out.role);         // 审计角色名（wfr-㊾）
         get_str(out, node, "criteria", out.criteria); // 批级终审判据 review（wfr-㊾）
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "verify";
         node["phase"] = n.phase;
         if (!n.model.empty()) node["model"] = n.model;
         if (!n.toolset.empty()) node["toolset"] = n.toolset;
         if (!n.system_prompt.empty()) node["system_prompt"] = n.system_prompt;
         if (!n.role.empty()) node["role"] = n.role;
         if (!n.criteria.empty()) node["criteria"] = n.criteria;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         std::string d = "verify " + n.phase;
         if (!n.role.empty()) d += " role=" + n.role;
         return d + " → " + n.next;
       },
       v_verify, c_verify, e_next, t_none},

      // ---------------- plan_review（wfr-㊹ 批④）：步骤边界任务级 HITL ----------------
      {NodeKind::PlanReview, "plan_review",
       [](Json const& n) { return kind_is(n, "plan_review"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "confirm", out.confirm);      // "true"/"false"（可 @config.*）
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "plan_review";
         if (!n.confirm.empty()) node["confirm"] = n.confirm;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         std::string d = "plan_review";
         if (!n.confirm.empty()) d += " confirm=" + n.confirm;
         return d + " → " + n.next;
       }, v_plan_review, c_plan_review, e_next, t_none},

      // ---------------- tool_registry（wfr-㊹ 治理，problems/34）：动态工具源 ----------------
      {NodeKind::ToolRegistry, "tool_registry",
       [](Json const& n) { return kind_is(n, "tool_registry"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "next", out.next);
         if (node.contains("tools") && node["tools"].is_array())
           for (auto const& t : node["tools"]) {
             if (!t.is_object()) continue;
             NodeIR::ToolSource s;
             if (t.contains("from") && t["from"].is_string()) s.from = t["from"].get<std::string>();
             if (t.contains("type") && t["type"].is_string()) s.type = t["type"].get<std::string>();
             if (t.contains("load") && t["load"].is_array())
               for (auto const& l : t["load"])
                 if (l.is_string()) s.load.push_back(l.get<std::string>());
             out.tool_sources.push_back(std::move(s));
           }
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "tool_registry";
         Json arr = Json::array();
         for (auto const& s : n.tool_sources) {
           Json j = Json{{"from", s.from}, {"type", s.type}};
           Json ls = Json::array();
           for (auto const& l : s.load) ls.push_back(l);
           j["load"] = std::move(ls);
           arr.push_back(std::move(j));
         }
         node["tools"] = std::move(arr);
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         std::string d = "tool_registry (" + std::to_string(n.tool_sources.size()) + " 源)";
         for (auto const& s : n.tool_sources) {
           d += " from=" + s.from + "[" + s.type + "] load=" + std::to_string(s.load.size());
         }
         return d + " → " + n.next;
       }, v_tool_registry, c_tool_registry, e_next, t_none},
      // ---------------- task（wfr-㊿ 长程）：任务档案 begin/end ----------------
      {NodeKind::Task, "task",
       [](Json const& nn) { return kind_is(nn, "task"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "task", out.task);
         get_str(out, node, "phase", out.phase);
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "task";
         if (!n.task.empty()) node["task"] = n.task;
         node["phase"] = n.phase;
         node["next"] = n.next;
       },
       [](NodeIR const& n) { return std::string("task ") + n.phase +
                                    (n.task.empty() ? "" : " task=" + n.task) + " → " + n.next; },
       v_task, c_task, e_next, t_none},
      // ---------------- subworkflow（wfr-㊿ proxyNode，arch 24）：运行期调用子 workflow ----------------
      {NodeKind::SubWorkflow, "subworkflow",
       [](Json const& n) { return kind_is(n, "subworkflow"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "workflow", out.sub_workflow);
         get_str(out, node, "mode", out.subflow_mode);
         if (node.contains("copy_state") && node["copy_state"].is_array())
           for (auto const& k : node["copy_state"])
             if (k.is_string()) out.copy_state.push_back(k.get<std::string>());
         // input 原样收下（含非对象：让 VN24 报错，而不是 parse 静默丢掉）
         if (node.contains("input") && !node["input"].is_null()) out.sub_input = node["input"];
         get_str(out, node, "next", out.next);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "subworkflow";
         node["workflow"] = n.sub_workflow;
         if (!n.subflow_mode.empty()) node["mode"] = n.subflow_mode;
         if (!n.copy_state.empty()) {
           Json arr = Json::array();
           for (std::string const& k : n.copy_state) arr.push_back(k);
           node["copy_state"] = std::move(arr);
         }
         if (!n.sub_input.empty()) node["input"] = n.sub_input;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         return "subworkflow " + n.sub_workflow +
                (n.subflow_mode.empty() || n.subflow_mode == "sync" ? "" : " (" + n.subflow_mode + ")") +
                (n.copy_state.empty() ? "" : " copy=(" + [&] {
                   std::string s;
                   for (std::string const& k : n.copy_state) s += (s.empty() ? "" : ",") + k;
                   return s;
                 }() + ")") + " → " + n.next;
       }, v_subworkflow, c_subworkflow, e_next, t_subworkflow},
      // ---------------- executor（wfr-㊾ 批③）：复合 worker，parallel 的 Send 目标 ----------------
      {NodeKind::Executor, "executor",
       [](Json const& n) { return kind_is(n, "executor"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "model", out.model);
         get_str(out, node, "tools", out.toolset);
         get_str(out, node, "system_prompt", out.system_prompt);
         get_str(out, node, "role", out.role);
         get_str(out, node, "next", out.next);
         if (node.contains("params") && !node["params"].is_null()) out.model_params = node["params"];
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "executor";
         node["model"] = n.model;
         if (!n.toolset.empty()) node["tools"] = n.toolset;
         if (!n.system_prompt.empty()) node["system_prompt"] = n.system_prompt;
         if (!n.role.empty()) node["role"] = n.role;
         if (!n.model_params.empty()) node["params"] = n.model_params;
         node["next"] = n.next;
       },
       [](NodeIR const& n) {
         return "executor " + n.model + (n.role.empty() ? "" : " role=" + n.role) +
                (n.toolset.empty() ? "" : " tools=" + n.toolset) + " → " + n.next;
       }, v_executor, c_executor, e_executor, t_model},
      // ---------------- parallel（wfr-㊾ 批③）：扇出+收拢编排 ----------------
      {NodeKind::Parallel, "parallel",
       [](Json const& n) { return kind_is(n, "parallel"); },
       [](NodeIR& out, Json const& node) {
         get_str(out, node, "target", out.target);
       },
       [](Json& node, NodeIR const& n) {
         node["kind"] = "parallel";
         if (!n.target.empty()) node["target"] = n.target;
       },
       [](NodeIR const& n) {
         return "parallel → " + (n.target.empty() ? "(缺 target)" : n.target) + " (Send 并行波)";
       }, v_parallel, c_parallel, e_parallel, t_none},
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
