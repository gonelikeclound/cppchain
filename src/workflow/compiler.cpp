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
#include "cppchain/workflow/node_type.hpp"
#include "cppchain/workflow/history_file.hpp"
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

// `_fail` 的落地节点名（见 ir.hpp 的 kFailName）。
// 各节点类型的"编译"已迁到 node_types.cpp；本文件只剩图装配与 schema。
constexpr char kFailTarget[] = "__fail__";

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

  // 3) 逐节点编译：走**注册表**（加一种节点不必再改这里）
  NodeType::CompileContext cctx{g, &registry, opts.components, resolve, 0, {}};
  for (NodeIR const& n : ir.nodes) {
    NodeType const* type = node_type_of(n.kind);
    if (type == nullptr || !type->compile)
      return invalid("internal: 该节点类型没有注册 compile（node '" + n.id + "'）");
    type->compile(n, cctx);
    if (!cctx.error.empty()) return invalid(cctx.error);
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

  CompiledWorkflow out{std::move(*compiled), ir.id, ir.trigger, cctx.registered};
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