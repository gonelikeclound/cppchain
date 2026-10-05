// cppchain —— src/workflow/compiler.cpp
#include "cppchain/workflow/compiler.hpp"

#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "cppchain/state/state.hpp"
#include "cppchain/tool/tool.hpp"
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
  ValidationResult vr = validate(ir, ValidatorOptions{&registry, opts.config_keys});
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

  auto resolve = [&](std::string const& t) -> std::string {
    if (t == "_turn_end") return graph::kEnd;
    auto it = cont.find(t);
    return it == cont.end() ? t : it->second;
  };

  // 3) StateSchema：results / loop_counts 用合并 reducer，其余键 overwrite
  state::StateSchema schema;
  schema.set_reducer(std::string(kResultsKey), merge_slots);
  schema.set_reducer(std::string(kLoopCountsKey), merge_slots);

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

      case NodeKind::Label:
        break;  // 不注册（指针已替换）
    }
  }

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