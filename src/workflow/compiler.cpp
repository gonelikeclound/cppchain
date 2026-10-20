// cppchain —— src/workflow/compiler.cpp
#include "cppchain/workflow/compiler.hpp"

#include "cppchain/workflow/ir_walk.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "cppchain/message/serializer.hpp"  // Message ↔ JSON（messages 通道需要）
#include "cppchain/state/state.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/tool/tool_node.hpp"  // M12: run_tools 复用 M3 的 ToolNode
#include "cppchain/workflow/expr.hpp"
#include "cppchain/workflow/node_type.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "cppchain/workflow/validator.hpp"

namespace cppchain::workflow {

using cppchain::graph::Command;
using cppchain::graph::InterruptSignal;
using cppchain::graph::NodeContext;

namespace {

// 跨触发的 run 序（仅用于可读的 run id；实际排序靠日志里的 seq）
std::atomic<std::int64_t> g_run_seq{0};

// 在"节点边界"统一写结构事件（docs/12 §6.3）。
// 唯一的 chokepoint 是 StateGraph::add_node —— 9 种节点类型的编译无需各自埋点，之后
// 加的每个新节点类型也自动带结构事件。wrapper 与 CompiledWorkflow 共享同一个
// SessionSink，fire_once 每触发周期注入 log/run。
class LoggingStateGraph final : public graph::StateGraph {
 public:
  using StateGraph::StateGraph;
  graph::StateGraph& add_node(std::string name, graph::NodeFn fn) override;

  std::shared_ptr<SessionSink> sink;
  std::unordered_map<std::string, std::string> kinds;  // 图节点 id → node_kind 字面量
  void note_kind(std::string id, std::string_view kind) {
    kinds[std::move(id)] = std::string(kind);
  }
};

graph::StateGraph& LoggingStateGraph::add_node(std::string name, graph::NodeFn fn) {
  std::string const node_id = std::move(name);
  auto const kind_it = kinds.find(node_id);
  std::string const kind = kind_it != kinds.end() ? kind_it->second : "node";
  auto sink_p = sink;
  return StateGraph::add_node(
      node_id, [node_id, kind, sink_p, fn = std::move(fn)](Json const& state,
                                                           NodeContext& nc) -> Command {
        SessionLog* slog = sink_p->log;
        // 双源互斥（M10）：host 经 FireOptions.stream 走引擎事件通道时 wrapper 让位，
        // 结构日志由宿主用 project_stream_to_log 投影，避免同一节点被记两遍。
        if (slog == nullptr || sink_p->streaming) return fn(state, nc);

        auto const t0 = std::chrono::steady_clock::now();
        slog->engine_enter(sink_p->run, node_id, kind);
        auto ms = [&] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
              .count();
        };
        try {
          Command c = fn(state, nc);
          // load_history 的 update 是"已有事件的回声"：不落 update 键，否则 replay 时
          // add_messages 会把整段消息再追加一遍（无 id 消息按追加处理）。历史本身
          // 已在更早的 input + 各节点 update 里，去掉这条回声 replay 仍完整。
          std::optional<Json> const update =
              (kind == "load_history") ? std::nullopt : std::optional<Json>(c.update);
          slog->engine_exit(sink_p->run, node_id, kind, true, "ok", ms(), update);
          return c;
        } catch (InterruptSignal const&) {
          // 悬挂不是失败：与 enter 配对写 interrupt，不误记 error（replay 铁律天然忽略）
          slog->engine_interrupt(sink_p->run, node_id, kind, ms());
          throw;
        } catch (...) {
          slog->engine_exit(sink_p->run, node_id, kind, false, "error", ms(), std::nullopt);
          throw;
        }
      });
}

// 运行时按子键"右胜左"的合并归约：results / loop_counts 必须合并，否则每个
// tool 节点的写入都会清掉上一个节点的输出（默认 reducer 是整体覆盖）。
Json merge_slots(Json const& left, Json const& right) {
  if (!right.is_object()) return right;
  if (!left.is_object()) return left.is_null() ? right : left;
  Json out = left;
  for (auto it = right.begin(); it != right.end(); ++it) out[it.key()] = it.value();
  return out;
}

// `_fail`/`_timeout`/`_cancelled` 的落地节点名（见 ir.hpp 的 kFailName/kTimeoutName/kCancelledName）。
// 各节点类型的"编译"已迁到 node_types.cpp；本文件只剩图装配与 schema。
constexpr char kFailTarget[] = "__fail__";
constexpr char kTimeoutTarget[] = "__timeout__";
constexpr char kCancelTarget[] = "__cancelled__";

}  // namespace

// 垫高进程内 run 计数（跨进程续写同一会话时由宿主调用，
// 见 session_log 的 max_run_seq 说明）。定义在匿名命名空间之外 → 导出符号。
void seed_run_seq(std::int64_t base) {
  g_run_seq.store(base, std::memory_order_relaxed);
}

std::expected<CompiledWorkflow, WorkflowError> compile_workflow(
    WorkflowIR const& ir, tool::ToolRegistry const& registry, state::Checkpointer* cp,
    CompileOptions opts) {
  auto invalid = [](std::string m) {
    return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, std::move(m)});
  };

  // 1) 先过校验器（fail-closed：不编译坏 IR）
  ValidationResult vr =
      validate(ir, ValidatorOptions{&registry, opts.config_keys, opts.components});
  if (!vr.ok()) return invalid("IR 校验未过：" + vr.describe());
  if (ir.nodes.empty())
    return invalid("IR nodes 为空（生成器不应产出）");

  bool need_fail_node = false;      // 只有真的用到 _fail 才注册那个节点
  bool need_timeout_node = false;   // _timeout 同理（governed 的 loop on_max）
  bool need_cancel_node = false;    // _cancelled 同理（plan_review abort）
  auto resolve = [&](std::string const& t) -> std::string {
    if (t == kTurnEndName) return graph::kEnd;
    if (t == kFailName) {
      need_fail_node = true;
      return kFailTarget;
    }
    if (t == kTimeoutName) {
      need_timeout_node = true;
      return kTimeoutTarget;
    }
    if (t == kCancelledName) {
      need_cancel_node = true;
      return kCancelTarget;
    }
    return t;  // id 全局唯一：不再需要 label 那种"续接消解"
  };

  // 3) StateSchema：results / loop_counts 用合并 reducer，其余键 overwrite
  state::StateSchema schema;
  schema.set_reducer(std::string(kResultsKey), merge_slots);
  schema.set_reducer(std::string(kLoopCountsKey), merge_slots);
  // wfr-㊾ 批③：交接件 handoff 是 per-step 子键合并（merge_slots）——并行 Executor 各写各的
  // <step_id>，`parallel` 收拢按声明序折叠；同一键多写者（不同 step）不会 ConcurrentWriteConflict。
  schema.set_reducer(std::string(kHandoffStateKey), merge_slots);
  // messages 必须走 add_messages：model/run_tools 节点返回的是"新增消息"，
  // 默认的 overwrite 会把整个数组替换掉（历史当场丢光）。
  schema.set_reducer(std::string(kMessagesKey), state::add_messages);

  // 结构事件（docs/12 §6.3）：所有节点经 LoggingStateGraph 包装、在边界统一埋点。
  auto sink = std::make_shared<SessionSink>();
  LoggingStateGraph g(std::move(schema));
  g.sink = sink;
  // 3) 逐节点编译：走**注册表**（加一种节点不必再改这里）。loop 的 body 递归进来 ——
  //    编译每个节点前把 `resolve` 按当前作用域包一层，于是 `_continue` / `_break`
  //    在**编译期**就解析成图目标（body 之外的保留名是错误，fail-closed）。
  std::string compile_error;
  // 一份 cctx 复用（registered 要累加），只按当前作用域换 resolve。
  NodeType::CompileContext cctx{g, &registry, opts.components, resolve, nullptr, 0, {}, sink};
  cctx.guard = opts.tool_policy;  // wfr-㊷：副作用执行点防护（null = 不防护）
  std::function<void(std::vector<NodeIR> const&, LoopScope const*)> compile_list =
      [&](std::vector<NodeIR> const& nodes, LoopScope const* scope) {
        for (NodeIR const& n : nodes) {
          if (!compile_error.empty()) return;
          // 预登记 node_kind（wrapper 在运行期查表；按名分发含动态节点）
          NodeType const* type = node_type_of_name(type_name_of(n));
          if (type == nullptr || !type->compile) {
            compile_error = "internal: 该节点类型没有注册 compile（node '" + n.id + "'）";
            return;
          }
          g.note_kind(n.id, type->name);
          cctx.resolve = [&resolve, scope](std::string const& t) {
            if (scope == nullptr) return resolve(t);  // 顶层：保留名由 validator 拦
            if (t == kContinueName) return scope->loop_id;  // 回边 → loop 节点头
            if (t == kBreakName) return resolve(scope->exit_target);
            return resolve(t);
          };
          cctx.goto_resolve = [&resolve, scope](std::string const& t) {  // 同上，供 goto_nodes 用
            if (scope == nullptr) return t;
            if (t == kContinueName) return scope->loop_id;
            if (t == kBreakName) return resolve(scope->exit_target);
            return resolve(t);
          };
          cctx.error.clear();
          type->compile(n, cctx);
          if (!cctx.error.empty()) {
            compile_error = cctx.error;
            return;
          }
          if (n.kind == NodeKind::Loop && !n.body.empty()) {
            LoopScope const inner{n.id, n.next};
            compile_list(n.body, &inner);
          }
        }
      };
  compile_list(ir.nodes, nullptr);
  if (!compile_error.empty()) return invalid(compile_error);

  // 3.5) _fail 的落地节点：只有真的被引用才注册（不污染 registered_nodes 计数）。
  //      语义 = 以错误终止本次触发（fail-closed），错误里带 IR id 便于定位；
  //      细节仍在 state.messages 里可查（不额外造错误载荷）。
  if (need_fail_node) {
    g.note_kind(kFailTarget, "fail");
    g.add_node(kFailTarget, [ir_id = ir.id](Json const&, NodeContext&) -> Command {
      throw tool::ToolException("workflow '" + ir_id +
                                "' 路由到 _fail：本次触发以错误终止（原因见 state.messages）");
    });
  }

  // 3.6) _timeout / _cancelled 落地节点（治理终态四档，problems/33）。
  //      与 _fail 不同：**不抛异常**（超时/取消 ≠ 崩溃）——写 plan.terminal + plan_log 终态行，
  //      再 goto __end__ 正常收尾。终态无并发写出，plan overwrite 安全；须从 state 读现有 plan 补字段。
  if (need_timeout_node) {
    g.note_kind(kTimeoutTarget, "timeout");
    g.add_node(kTimeoutTarget, [ir_id = ir.id](Json const& state, NodeContext&) -> Command {
      Json update;
      Json plan = state.contains("plan") && state["plan"].is_object() ? state["plan"]
                                                                     : Json::object();
      plan["terminal"] = "timeout";
      update["plan"] = std::move(plan);
      Json pl = state.contains("plan_log") && state["plan_log"].is_array() ? state["plan_log"]
                                                                          : Json::array();
      pl.push_back(Json{{"step", "*"}, {"kind", "terminal"}, {"verdict", "timeout"},
                        {"why", "loop on_max 触达且未收口（超时档）"}});
      update["plan_log"] = std::move(pl);
      Json results = Json::object();
      results[kTimeoutTarget] = Json{{"terminal", "timeout"}, {"workflow", ir_id}};
      update[kResultsKey] = std::move(results);
      return Command{std::move(update), {graph::kEnd}, {}};
    });
  }
  if (need_cancel_node) {
    g.note_kind(kCancelTarget, "cancelled");
    g.add_node(kCancelTarget, [ir_id = ir.id](Json const& state, NodeContext&) -> Command {
      Json update;
      Json plan = state.contains("plan") && state["plan"].is_object() ? state["plan"]
                                                                     : Json::object();
      plan["terminal"] = "cancelled";
      update["plan"] = std::move(plan);
      Json pl = state.contains("plan_log") && state["plan_log"].is_array() ? state["plan_log"]
                                                                          : Json::array();
      pl.push_back(Json{{"step", "*"}, {"kind", "terminal"}, {"verdict", "cancelled"},
                        {"why", "用户中止 / 计划取消（取消档）"}});
      update["plan_log"] = std::move(pl);
      Json results = Json::object();
      results[kCancelTarget] = Json{{"terminal", "cancelled"}, {"workflow", ir_id}};
      update[kResultsKey] = std::move(results);
      return Command{std::move(update), {graph::kEnd}, {}};
    });
  }

  // 4) 入口 = nodes[0]；为 label 时取它的续接
  std::string entry = ir.nodes[0].id;
  if (entry.empty() || entry == graph::kEnd)
    return invalid("入口无法解析");

  g.set_entry_point(entry);

  // 5) 编译（cp 可空）。配好 max_steps / max_parallel 兜底。
  auto compiled = g.compile(cp, graph::CompileOptions{opts.max_steps, opts.max_parallel});
  if (!compiled)
    return std::unexpected(WorkflowError{WorkflowError::Kind::Compile, compiled.error().message});

  CompiledWorkflow out{std::move(*compiled), ir.id, ir.trigger, cctx.registered, cp,
                         std::move(g.kinds), std::move(sink)};
  return out;
}

std::expected<Json, WorkflowError> fire_once(CompiledWorkflow& wf, Json const& config) {
  return fire_once(wf, config, FireOptions{});
}

std::expected<Json, WorkflowError> fire_once(CompiledWorkflow& wf, Json const& config,
                                             std::string const& thread_id) {
  FireOptions o;
  o.thread_id = thread_id;
  return fire_once(wf, config, std::move(o));
}

std::expected<Json, WorkflowError> fire_once(CompiledWorkflow& wf, Json const& config,
                                             FireOptions opts) {
  if (wf.sink->log != nullptr)
    return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid,
                                         "reentrant fire: 上一次触发的日志句柄未清除"});
  // run id：跨触发单调递增（可读；日志内的实际排序是 seq）
  std::string const run = "r-" + std::to_string(++g_run_seq);
  wf.sink->run = run;
  wf.sink->log = opts.log;

  // 异常安全：invoke 可能**抛出**（非 std 异常 / 存储失败 / 宿主 sink 抛），裸代码收尾会
  // 让 sink->log 残留 → 该 CompiledWorkflow 永久进入 reentrant 拒绝。用 RAII guard 兜底。
  struct SinkGuard {
    CompiledWorkflow& wf;
    ~SinkGuard() {
      wf.sink->streaming = false;
      wf.sink->log = nullptr;
      wf.sink->run.clear();
    }
  } guard{wf};

  // 本轮输入由**引擎**写成 input 事件（docs/12 §6.3 + §7）：它在 run 开头、历史之后、
  // 本 run 节点之前 —— 下一个 run 的 load_history 会把它 replay 回来。
  // **只有真正会消费 input 的触发才写**：续跑（thread 已有快照）与分叉（fork）走恢复
  // 语义、引擎忽略 input，此时仍写 __input__ 会在下一次 replay 引入幻影消息。
  bool const recreates_state =
      !opts.fork.has_value() &&
      (opts.thread_id.empty() || wf.cp == nullptr ||
       !wf.cp->get_tuple(opts.thread_id, "", std::nullopt).has_value());
  if (recreates_state && opts.log != nullptr && !opts.input_messages.empty()) {
    opts.log->engine_enter(run, "__input__", "input");
    opts.log->engine_exit(run, "__input__", "input", true, "ok", 0,
                          std::optional<Json>(Json{{kMessagesKey, opts.input_messages}}));
  }

  Json input = Json{{kConfigKey, config}};
  wf.sink->streaming = opts.stream != nullptr;
  graph::InvokeOptions iopts;
  // 全模式事件：host 要的是事件流本身；没传 stream 时 sink=null → 零事件（仅构造无磨损）
  iopts.mode = graph::EventMode::All;
  iopts.fork = opts.fork;
  iopts.resume = opts.resume;  // wfr-㊸ @②：interrupt 悬挂的答复（人审裁决）
  // 包装 host 的 stream sink：给事件填 node_kind（引擎不默认图内 kind，kinds 表编译期建立；
  // 投影据此识别 load_history 之类的回声 update 并抑制 —— 与 wrapper 路径一致）
  if (opts.stream) {
    auto host = opts.stream;
    auto kinds = wf.node_kinds;
    iopts.sink = [host = std::move(host), kinds = std::move(kinds)](graph::StreamEvent const& e) {
      graph::StreamEvent ev = e;  // sink 回调签名是 const&，按值填字段后再投递
      if (auto it = kinds.find(ev.node); it != kinds.end()) ev.node_kind = it->second;
      host(ev);
    };
  }
  auto const r = wf.graph.invoke(input, opts.thread_id, iopts);

  if (!r) return std::unexpected(WorkflowError{WorkflowError::Kind::Runtime, r.error().message});
  return *r;
}

// 把 graph 运行期事件投影成会话日志记录（信封与默认 wrapper 一致；docs/12 §6.3 形状）。
// 默认宿主用 wrapper 直接写结构事件；传了 FireOptions.stream 时由本函数从事件通道落地。
void project_stream_to_log(SessionLog& log, std::string const& run, graph::StreamEvent const& ev,
                           std::string_view fallback_kind) {
  std::string const kind = ev.node_kind.empty() ? std::string(fallback_kind) : ev.node_kind;
  if (ev.type == "node_start") {
    log.engine_enter(run, ev.node, kind);
  } else if (ev.type == "node_end") {
    if (ev.ok)
      log.engine_exit(
          run, ev.node, kind, true, "ok", 0,
          (kind == "load_history")
              ? std::nullopt  // 回声 update 不落（与 wrapper 一致）：否则 replay 双写整段历史
              : (ev.payload.is_object()  // 与 wrapper 一致：空对象也落 "update":{}（replay 等价）
                     ? std::optional<Json>{ev.payload}
                     : std::nullopt));
    else
      log.engine_exit(run, ev.node, kind, false, "error", 0, std::nullopt);
  } else if (ev.type == "interrupt") {
    log.engine_interrupt(run, ev.node, kind, 0);
  }
  // "values"/节点自定 delta 事件 → 不落结构日志（values 是状态快照，node 日志走 nlog()）
}

std::vector<Json> simulate_fires(CompiledWorkflow& wf, std::vector<Json> const& configs) {
  std::vector<Json> out;
  out.reserve(configs.size());
  for (Json const& c : configs) {
    auto r = fire_once(wf, c);
    out.push_back(r ? *r : Json{{"error", r.error().message}});
  }
  return out;
}

}  // namespace cppchain::workflow