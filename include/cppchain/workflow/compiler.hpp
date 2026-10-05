// cppchain —— workflow/compiler.hpp
// IR → StateGraph 编译与触发运行（规范 11-ir-syntax.md §3–§7 + D1/D2）。
//
// 编译要点：
//   · label 不注册为图节点 —— 所有指向 label 的指针（next/then/else/goto/入口）
//     统一替换为 label 的"续接节点"（数组里它之后的第一个非 label 节点，D2 整体求解）；
//   · expr 节点 → 条件边（router 在状态上求值 expr，返回 then/else）；
//   · goto 节点 → 有界回跳（loop_counts 计数，超过 max 跳终止）；
//   · on_error：retry 在节点内循环；fallback 用 Command.jump 覆盖静态边；
//   · 触发（D1）：trigger 仅为元数据，不做 OS 调度 —— fire_once 即"模拟触发"，
//     每次触发全新状态（proactive 反复触发的语义由调用方控制时机）。
#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "cppchain/graph/graph.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"

namespace cppchain::workflow {

using Json = nlohmann::json;

struct WorkflowError {
  enum class Kind { Invalid, Compile, Runtime };
  Kind kind = Kind::Compile;
  std::string message;
};

struct CompileOptions {
  std::size_t max_steps = 200;  // 循环由 goto.max 界定，max_steps 兜底防病态 IR

  // 宿主将要传给 fire_once 的 config 键（B2）。语义同 ValidatorOptions::config_keys：
  // nullopt = 不检查；声明后，IR 里引用未声明的 @config.<键> 会在**编译期**被 VR4 拒绝，
  // 而不是运行期静默解析成 null（工具读到 null 才炸）。
  std::optional<std::vector<std::string>> config_keys = std::nullopt;

  // M12 组件注册表（nullptr = 含 model 节点时报错，因为无模型可用）
  ComponentRegistry const* components = nullptr;
};

struct CompiledWorkflow {
  graph::CompiledGraph graph;
  std::string ir_id;
  TriggerIR trigger;  // 元数据透传（非调度用）
  std::size_t graph_node_count_ = 0;  // 实际注册进图的节点数（label 不注册）

  [[nodiscard]] std::size_t registered_nodes() const { return graph_node_count_; }
};

// ir 必须已通过 validate；本函数内部会再校验一遍（fail-closed）。
// cp 可空 → 触发不落快照；非空 → fire_once 带 thread_id 时可中途恢复续跑。
[[nodiscard]] std::expected<CompiledWorkflow, WorkflowError>
compile_workflow(WorkflowIR const& ir, tool::ToolRegistry const& registry,
                 state::Checkpointer* cp = nullptr, CompileOptions opts = {});

// D1：手动/模拟触发 —— 直接跑编译好的链。每次触发全新状态。
// config 为 @config.* 占位值映射（§5），如 {"product_id": "P1001"}。
// 注意：invoke 会更新引擎的内部步数/快照计数 → wf 需非常量。
[[nodiscard]] std::expected<Json, WorkflowError> fire_once(CompiledWorkflow& wf,
                                                           Json const& config);

// 带 thread_id 的触发：配合编译期 cp 支持按线程快照恢复（续跑同一丝线）。
[[nodiscard]] std::expected<Json, WorkflowError> fire_once(CompiledWorkflow& wf,
                                                           Json const& config,
                                                           std::string const& thread_id);

// 连续触发多次（每次全新状态），返回每次的最终状态 —— 调度模拟 / 断言用。
[[nodiscard]] std::vector<Json> simulate_fires(CompiledWorkflow& wf,
                                               std::vector<Json> const& configs);

}  // namespace cppchain::workflow