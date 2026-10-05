// cppchain —— src/workflow/compiler.cpp
#include "cppchain/workflow/compiler.hpp"

#include <exception>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "cppchain/message/serializer.hpp"  // Message ↔ JSON（messages 通道需要）
#include "cppchain/state/state.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/tool/tool_node.hpp"  // M12: run_tools 复用 M3 的 ToolNode
#include "cppchain/workflow/expr.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/validator.hpp"

namespace cppchain::workflow {

using cppchain::graph::Command;
using cppchain::graph::NodeContext;

namespace {

// 运行时按子键"右胜左"的合并归约：results / loop_counts 必须合并，否则每个
// tool 节点的写入都会清掉上一个节点的输出（默认 reducer 是整体覆盖）。
Json merge_slots(Json const& left, Json const& right) {
  if (!right.is_object()) return right;
  if (!left.is_object()) return left.is_null() ? right : left;
  Json out = left;
  for (auto it = right.begin(); it != right.end(); ++it) out[it.key()] = it.value();
  return out;
}

// 把 tool 节点 parameters 里的每个值解析为运行时的 JSON 参数（引用/模板替换）。
Json resolve_args(Json const& params, Json const& state) {
  Json out = Json::object();
  for (auto it = params.begin(); it != params.end(); ++it)
    out[it.key()] = resolve_parameter(it.value(), state);
  return out;
}

// ---- M12：对话通道（state.messages）----
// 状态里的 messages 是**规范 JSON**（Message 的序列化契约），这里与 Message 互转。
std::vector<message::Message> messages_from_state(Json const& state) {
  std::vector<message::Message> out;
  auto const it = state.find(kMessagesKey);
  if (it == state.end() || !it->is_array()) return out;
  out.reserve(it->size());
  for (Json const& raw : *it) {
    try {
      out.push_back(raw.get<message::Message>());
    } catch (std::exception const&) {
      // 坏消息不静默吞：交由调用方语义处理（此处跳过，validator 上游已保证形状）
    }
  }
  return out;
}

// 最后一条 AI 消息的**归一化 outcome**（封闭词表，见 ir.hpp）。
// 没有 AI 消息时返回空串（不匹配任何 case）→ 强制走 route 的 default 分支，
// 而不是被当成正常文本回复。
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

// _fail 的落地：一个"一定会抛"的节点（见 ir.hpp 的 kFailName）
constexpr char kFailTarget[] = "__fail__";

// 命令构造器：带更新 + 跳转（fallback / goto 用）
Command jump_update(Json update, std::vector<std::string> goto_nodes) {
  Command c;
  c.update = std::move(update);
  c.goto_nodes = std::move(goto_nodes);
  return c;
}

}  // namespace

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

  // 2) 求解 label 续接映射（节点 id → 续接 id；非 label 映射到自身）
  std::unordered_map<std::string, std::string> cont;
  for (std::size_t i = 0; i < ir.nodes.size(); ++i) {
    NodeIR const& n = ir.nodes[i];
    cont[n.id] = n.id;  // 占位（非 label 用不到）
    if (n.kind == NodeKind::Label) {
      std::string c;
      for (std::size_t k = i + 1; k < ir.nodes.size(); ++k)
        if (ir.nodes[k].kind != NodeKind::Label) {
          c = ir.nodes[k].id;
          break;
        }
      cont[n.id] = c;  // VG3 保证非空
    }
  }

  bool need_fail_node = false;  // 只有真的用到 _fail 才注册那个节点（否则会污染 registered 计数）
  auto resolve = [&](std::string const& t) -> std::string {
    if (t == kTurnEndName) return graph::kEnd;
    if (t == kFailName) {
      need_fail_node = true;
      return kFailTarget;
    }
    auto it = cont.find(t);
    return it == cont.end() ? t : it->second;
  };

  // 3) StateSchema：results / loop_counts 用合并 reducer，其余键 overwrite
  state::StateSchema schema;
  schema.set_reducer(std::string(kResultsKey), merge_slots);
  schema.set_reducer(std::string(kLoopCountsKey), merge_slots);
  // messages 必须走 add_messages：model/run_tools 节点返回的是"新增消息"，
  // 默认的 overwrite 会把整个数组替换掉（历史当场丢光）。
  schema.set_reducer(std::string(kMessagesKey), state::add_messages);

  graph::StateGraph g(std::move(schema));
  std::size_t registered = 0;

  for (NodeIR const& n : ir.nodes) {
    switch (n.kind) {
      case NodeKind::Tool: {
        std::string const node_id = n.id;
        std::string const tool = n.tool;
        Json const parameters = n.parameters;
        int const retry = n.on_error ? n.on_error->retry : 0;
        // fallback 与 next/then/else 走**同一条**解析：label → 它的续接节点、_turn_end → kEnd。
        // 原先直接把原始 id 塞进 Command.goto，而 label 并不注册为图节点
        // → 能过校验、能编译，一跑就是 "unknown node"（fail-closed 承诺失效）。
        std::optional<std::string> const fallback =
            (n.on_error && n.on_error->fallback)
                ? std::optional<std::string>(resolve(*n.on_error->fallback))
                : std::nullopt;
        tool::ToolRegistry const* reg = &registry;

        g.add_node(
            n.id, [node_id, tool, parameters, retry, fallback, reg](Json const& state, NodeContext&) {
              tool::ToolSpec const* spec = reg->find(tool);
              if (spec == nullptr)
                throw tool::ToolException("tool '" + tool + "' not in registry (node '" +
                                          node_id + "')");

              Json const args = resolve_args(parameters, state);
              tool::ToolContext tctx;
              tctx.tool_call_id = node_id;  // 幂等/审计兜底：节点 id 即本次调用的标识

              std::string last_err;
              for (int a = 0; a <= retry; ++a) {
                // handler **抛异常**与返回 unexpected 同等对待：都算本次尝试失败，
                // 交给 retry / fallback 处置。比照 ToolNode::execute_one 的纪律
                // （那里也是把异常转成错误、不让它穿透）—— 否则 on_error 在最常见的
                // 失败形态（工具抛错）下完全不生效：IR 明明声明了 fallback，照样直接崩。
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
                  Json update = Json{{kResultsKey, Json{{node_id, *r}}},
                                     {kLastExecKey, node_id}};
                  return Command::with_update(std::move(update));
                }
                last_err = r.error().message;
              }
              // 失败耗尽；无 fallback → 抛给引擎（→ WorkflowError::Runtime）
              if (fallback) return Command{{}, {*fallback}};
              // 带上最后一次的具体原因：只说"failed after N attempts"没法排查
              throw tool::ToolException("tool '" + tool + "' failed after " +
                                        std::to_string(retry + 1) + " attempts (node '" +
                                        node_id + "'): " + last_err);
            });
        // 成功出口：静态边（next 目标已由 resolve 替换 label/_turn_end）
        g.add_edge(n.id, resolve(n.next));
        ++registered;
        break;
      }

      case NodeKind::Expr: {
        std::string const then_t = resolve(n.then);
        std::string const else_t = resolve(n.else_);
        auto parsed = Expr::parse(n.expr);
        if (!parsed)
          return invalid("expr 节点 '" + n.id + "' 语法错误：" + parsed.error());
        auto expr_ptr = std::make_shared<Expr>(std::move(*parsed));

        // expr 节点 fn：no-op（它不产出输出，不动 _last_exec）；
        // 路由交给条件边 router（引擎在 fn 之后、apply 之后运行 router）。
        g.add_node(n.id, [](Json const&, NodeContext&) { return Command{}; });
        g.add_conditional_edges(
            n.id, [expr_ptr, then_t, else_t](Json const& state) {
              return std::vector<std::string>{expr_ptr->eval(state) ? then_t : else_t};
            });
        ++registered;
        break;
      }

      case NodeKind::Goto: {
        std::string const label = n.goto_target;
        std::string const target = label.empty() ? graph::kEnd : resolve(label);
        int const max = n.max;
        g.add_node(
            n.id, [n_ = n.id, target, max](Json const& state, NodeContext&) {
              long cur = 0;
              if (state.contains(kLoopCountsKey))
                if (auto const& lc = state.at(kLoopCountsKey);
                    lc.contains(n_) && lc[n_].is_number())
                  cur = lc[n_].get<long>();
              long const next = cur + 1;
              Json update = Json{{kLoopCountsKey, Json{{n_, next}}}};
              if (next >= max) return jump_update(update, {graph::kEnd});
              return jump_update(std::move(update), {target});
            });
        ++registered;
        break;
      }

      case NodeKind::Model: {
        model::ChatModel* m =
            (opts.components != nullptr) ? opts.components->model(n.model) : nullptr;
        if (m == nullptr)
          return invalid("model 节点 '" + n.id + "' 引用的组件 '" + n.model + "' 不可用");
        Json tools_json = Json::array();
        if (!n.toolset.empty() && opts.components != nullptr)
          if (auto const* ts = opts.components->toolset(n.toolset); ts != nullptr)
            for (std::string const& tname : *ts)
              if (auto const* sp = registry.find(tname); sp != nullptr)
                tools_json.push_back(sp->to_openai());
        std::string const sys = n.system_prompt;

        g.add_node(n.id, [m, tools_json, sys](Json const& state, NodeContext&) {
          std::vector<message::Message> msgs = messages_from_state(state);
          if (!sys.empty()) msgs.insert(msgs.begin(), message::system_message(sys));
          model::CallOptions co;
          co.tools = tools_json;
          auto r = m->invoke(msgs, co);
          if (!r) throw tool::ToolException("model 节点调用失败：" + r.error().message);
          // **只发一次**：把回复追加进 messages，循环由 route + 回边负责
          return Command::with_update(Json{{kMessagesKey, Json::array({*r})}});
        });
        g.add_edge(n.id, resolve(n.next));
        ++registered;
        break;
      }

      case NodeKind::Route: {
        // cases/default 在编译期就解析成目标名（label → 续接、_turn_end → kEnd、_fail → 失败节点）
        std::map<std::string, std::string> targets;
        for (auto const& [k, v] : n.cases) targets[k] = resolve(v);
        std::string const dflt = resolve(n.default_);

        g.add_node(n.id, [](Json const&, NodeContext&) { return Command{}; });  // no-op
        g.add_conditional_edges(n.id, [targets, dflt](Json const& state) {
          std::string const outcome = last_ai_outcome(state);
          auto const it = targets.find(outcome);
          return std::vector<std::string>{it != targets.end() ? it->second : dflt};
        });
        ++registered;
        break;
      }

      case NodeKind::RunTools: {
        tool::ToolNode tn(&registry);  // 复用 M3 的实现（并行、顺序对齐、错误回喂）
        g.add_node(n.id, [tn](Json const& state, NodeContext&) {
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
        g.add_edge(n.id, resolve(n.next));
        ++registered;
        break;
      }

      case NodeKind::Label:
        break;  // 不注册（指针已替换）
    }
  }

  // 3.5) _fail 的落地节点：只有真的被引用才注册（不污染 registered_nodes 计数）。
  //      语义 = 以错误终止本次触发（fail-closed），错误里带 IR id 便于定位；
  //      细节仍在 state.messages 里可查（不额外造错误载荷）。
  if (need_fail_node)
    g.add_node(kFailTarget, [ir_id = ir.id](Json const&, NodeContext&) -> Command {
      throw tool::ToolException("workflow '" + ir_id +
                                "' 路由到 _fail：本次触发以错误终止（原因见 state.messages）");
    });

  // 4) 入口 = nodes[0]；为 label 时取它的续接
  std::string entry = ir.nodes[0].id;
  if (ir.nodes[0].kind == NodeKind::Label) entry = resolve(entry);
  if (entry.empty() || entry == graph::kEnd)
    return invalid("入口无法解析");

  g.set_entry_point(entry);

  // 5) 编译（cp 可空）。配好 max_steps 兜底。
  auto compiled = g.compile(cp, graph::CompileOptions{opts.max_steps});
  if (!compiled)
    return std::unexpected(WorkflowError{WorkflowError::Kind::Compile, compiled.error().message});

  CompiledWorkflow out{std::move(*compiled), ir.id, ir.trigger, registered};
  return out;
}

std::expected<Json, WorkflowError> fire_once(CompiledWorkflow& wf, Json const& config) {
  Json input = Json{{kConfigKey, config}};
  auto r = wf.graph.invoke(input, "");
  if (!r) return std::unexpected(WorkflowError{WorkflowError::Kind::Runtime, r.error().message});
  return *r;
}

std::expected<Json, WorkflowError> fire_once(CompiledWorkflow& wf, Json const& config,
                                             std::string const& thread_id) {
  Json input = Json{{kConfigKey, config}};
  auto r = wf.graph.invoke(input, thread_id);
  if (!r) return std::unexpected(WorkflowError{WorkflowError::Kind::Runtime, r.error().message});
  return *r;
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