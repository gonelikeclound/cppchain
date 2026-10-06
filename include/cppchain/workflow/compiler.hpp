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
#include "cppchain/workflow/session_log.hpp"  // SessionLog / SessionSink

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

  // 编译期注入的 checkpointer（可空）。fire_once 用它预判"本次触发是否走恢复
  //（thread 有快照 / fork）"：恢复时引擎会忽略 input，`__input__` 事件不应照写。
  state::Checkpointer* cp = nullptr;

  // 触发期注入的会话日志句柄（节点 wrapper 共享同一份；fire_once 写 log/run）。
  std::shared_ptr<SessionSink> sink;

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

// 触发期选项（docs/12 §6、§7）：每次触发可注入一个会话日志 + 本轮输入。
struct FireOptions {
  // 会话日志（宿主按工作路径开启，见 session_log.hpp）。null → 本次不落日志：
  // 引擎不写结构事件、也不写 input 事件；节点经 sink->nlog() 拿到空 logger。
  SessionLog* log = nullptr;
  // 本轮输入消息（规范 JSON，Message 可序列化）。非空且 log 非空 → 引擎在 run 开头
  // 写一条 node:"__input__" 的 exit 事件（source=engine），使顺序正确（历史在前、本轮在后）
  // 且下个 run 的 replay 自动包含本轮输入 —— 不必像 D 阶段那样"宿主先写进会话文件"。
  std::vector<Json> input_messages = {};
  // 恢复续跑（与旧的 thread_id 重载等价的统一入口）。
  std::string thread_id;
  // M10: 引擎级事件通道（Streaming/Interrupt 的图事件）。非空 → fire_once 把它作为
  // invoke 的 sink（逐包吐 node_start/node_end/…），且 wrapper 结构日志**让位**
  // （SessionSink.streaming=true，防双源重复记录）；宿主用 project_stream_to_log 落地。
  graph::EventSink stream = nullptr;
  // M10: 时间旅行分叉 —— 从 source_thread 的历史 checkpoint 恢复并继续（新快照落到
  // thread_id）。需要编译期注入 checkpointer（cp 非空）。分叉点找不到 → fail-closed。
  std::optional<graph::ForkSpec> fork = std::nullopt;
};

// 把 graph 运行期事件投影成会话日志记录（供宿主在 FireOptions.stream 里自选落地）。
// 与默认 wrapper 写的信封相同：node_start→engine_enter，node_end(ok)→engine_exit(update)，
// node_end(!ok)→engine_exit(error)，interrupt→engine_interrupt。node_kind 为空时用 fallback。
void project_stream_to_log(SessionLog& log, std::string const& run, graph::StreamEvent const& ev,
                           std::string_view fallback_kind);
// 带 FireOptions 的触发：日志 + 输入 + （可选）线程恢复一次给齐。
[[nodiscard]] std::expected<Json, WorkflowError> fire_once(CompiledWorkflow& wf,
                                                           Json const& config,
                                                           FireOptions opts);

// 连续触发多次（每次全新状态），返回每次的最终状态 —— 调度模拟 / 断言用。
[[nodiscard]] std::vector<Json> simulate_fires(CompiledWorkflow& wf,
                                               std::vector<Json> const& configs);

}  // namespace cppchain::workflow