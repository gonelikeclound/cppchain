// cppchain —— graph/graph.hpp
// Graph 引擎（M6 极简起步；M10 补齐 Streaming / Interrupt / Send）。
// 顺序 super-step + 每步 checkpoint + 从快照恢复。
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cppchain/state/checkpointer.hpp"
#include "cppchain/state/state.hpp"

namespace cppchain::graph {

using Json = nlohmann::json;

inline constexpr char kEnd[] = "__end__";

// 运行时流式事件开关（位标志）
enum class EventMode : std::uint32_t {
  Off      = 0,
  Updates  = 1u << 0,  // 节点边界 + 状态增量（默认）
  Snapshot = 1u << 1,  // 每包结束后吐出完整 state
  Deltas   = 1u << 2,  // 转发节点内经 NodeContext::emit 的流式事件（模型 delta 等）
  All      = Updates | Snapshot | Deltas,
};

// enum class 无内建位运算，补最小 operator|/&
inline EventMode operator|(EventMode a, EventMode b) {
  return static_cast<EventMode>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
inline EventMode operator&(EventMode a, EventMode b) {
  return static_cast<EventMode>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

// 运行期事件信封。字段与 workflow 层 session_log 对齐（seq/type/step/…），
// workflow 的投影适配器据此直投既有日志，不改其内部编码。
struct StreamEvent {
  std::uint64_t seq = 0;          // 引擎内单调（node_delta 与引擎事件共用同一序列）
  std::string type;               // "node_start"/"node_end"/"values"/"interrupt"/"resume"/节点自定
  std::string node;               // 空 = 全局事件（values）
  std::string node_kind;          // workflow 层注入；graph 引擎默认空
  int step = 0;                   // 包执行计数
  Json payload = Json::object();
  bool ok = true;
  std::optional<std::string> status;  // "ok"/"error"/"interrupt"
};

using EventSink = std::function<void(StreamEvent const&)>;

// invoke 的扩展选项：注入事件 sink / 恢复续跑（M10）
struct InvokeOptions {
  EventSink sink = nullptr;                    // 注入即触发流；nullptr = 不吐事件（零开销）
  EventMode mode = EventMode::Updates;
  std::optional<Json> resume = std::nullopt;   // 对上次 interrupt 的答复（P2）
};

// 悬挂信号：节点内 interrupt(payload) 首次调用抛出，引擎据此暂停 + 落盘。
// 故意不继承 std::exception —— 节点 catch(std::exception) 不得把它当失败吞掉。
class InterruptSignal {
 public:
  explicit InterruptSignal(Json payload) : payload_(std::move(payload)) {}
  [[nodiscard]] Json const& payload() const { return payload_; }

 private:
  Json payload_;
};

// 引擎 last_interrupt() 的返回：调用方以 interrupted 区分"挂着 vs 跑完"
struct InterruptStatus {
  bool interrupted = false;
  std::string node;
  Json payload = Json::object();
};

// 动态扇出目标（P3）：Send(node, partial_state) 展开为一个独立"包"
struct Send {
  std::string node;
  Json state = Json::object();
};

// 节点返回值：状态更新 + 控制流跳转 + 动态扇出（Command，M6 简版 → M10 补齐）
struct Command {
  Json update = Json::object();
  std::vector<std::string> goto_nodes;  // 非空则覆盖静态/条件边
  std::vector<Send> sends = {};         // 非空 → 路由 = 仅 sends（覆盖 goto 与出边）

  static Command with_update(Json u) { return Command{std::move(u), {}, {}}; }
  static Command jump(std::vector<std::string> nodes) {
    return Command{Json::object(), std::move(nodes), {}};
  }
  static Command with_sends(std::vector<Send> ss) {
    return Command{Json::object(), {}, std::move(ss)};
  }
};

// 节点上下文：引擎注入运行时设施（sink/mode/seq → 节点内 emit 流式事件；
// pending_resume → P2 interrupt 恢复重跑）。持续按引用传给 NodeFn。
struct NodeContext {
  std::string node_name;
  std::string thread_id;
  int step = 0;

  EventSink const* sink = nullptr;
  EventMode mode = EventMode::Off;
  std::uint64_t* seq = nullptr;                    // 引擎侧共享序列（NodeContext::emit 用）
  std::optional<Json> pending_resume;              // interrupt 恢复重跑时注入的值

  // 节点自报事件（模型 delta 等）；sink 空 或 mode 不含 Deltas 时静默。
  void emit(std::string_view type, Json payload);
};

using NodeFn = std::function<Command(Json const& state, NodeContext& ctx)>;
using RouterFn = std::function<std::vector<std::string>(Json const& state)>;

// 中断原语：节点内调用以请求外部输入（M10）。
// 首次调用抛 InterruptSignal（引擎捕获 → 悬挂 + 落盘）；恢复重跑时返回注入的 resume 值。
// 约束：interrupt 之前的副作用在重跑时会重复执行 —— 副作用必须放在 interrupt 之后或幂等。
inline Json interrupt(Json const& payload, NodeContext& ctx) {
  if (ctx.pending_resume) return *ctx.pending_resume;
  throw InterruptSignal(payload);
}

struct GraphError {
  enum class Kind { InvalidGraph, UnknownNode, RecursionLimit, NodeError,
                    InterruptUnavailable, ResumeNotInterrupted };
  Kind kind = Kind::InvalidGraph;
  std::string message;

  bool operator==(GraphError const&) const = default;
};

inline std::string_view graph_error_kind(GraphError::Kind k) {
  switch (k) {
    case GraphError::Kind::InvalidGraph: return "invalid_graph";
    case GraphError::Kind::UnknownNode: return "unknown_node";
    case GraphError::Kind::RecursionLimit: return "recursion_limit";
    case GraphError::Kind::NodeError: return "node_error";
    case GraphError::Kind::InterruptUnavailable: return "interrupt_unavailable";
    case GraphError::Kind::ResumeNotInterrupted: return "resume_not_interrupted";
  }
  return "unknown";
}

struct CompileOptions {
  std::size_t max_steps = 100;  // super-step 上限（防无限循环）
};

class CompiledGraph;

class StateGraph {
 public:
  explicit StateGraph(state::StateSchema schema) : schema_(std::move(schema)) {}

  // virtual 供 workflow 层包装节点（docs/12 §6.3：在"节点边界"统一写结构事件，
  // 子类 LoggingStateGraph 只在 add_node 一处收口，9 个节点类型无需各自埋点）。
  virtual ~StateGraph() = default;
  virtual StateGraph& add_node(std::string name, NodeFn fn);
  StateGraph& add_edge(std::string from, std::string to);
  StateGraph& add_conditional_edges(std::string from, RouterFn router);
  StateGraph& set_entry_point(std::string name);

  // 结构检查 + 挂运行时依赖（checkpointer 可为空 → 不落快照）
  std::expected<CompiledGraph, GraphError> compile(state::Checkpointer* cp = nullptr,
                                                   CompileOptions opts = {}) const;

  [[nodiscard]] std::size_t node_count() const { return nodes_.size(); }

 private:
  friend class CompiledGraph;
  state::StateSchema schema_;
  std::vector<std::string> order_;  // 保持注册顺序（便于确定性的错误信息）
  std::unordered_map<std::string, NodeFn> nodes_;
  std::unordered_map<std::string, std::string> static_edges_;
  std::unordered_map<std::string, RouterFn> conditional_edges_;
  std::string entry_;
};

class CompiledGraph {
 public:
  CompiledGraph(state::StateSchema schema, std::unordered_map<std::string, NodeFn> nodes,
                std::unordered_map<std::string, std::string> static_edges,
                std::unordered_map<std::string, RouterFn> conditional_edges, std::string entry,
                state::Checkpointer* cp, CompileOptions opts)
      : schema_(std::move(schema)),
        nodes_(std::move(nodes)),
        static_edges_(std::move(static_edges)),
        conditional_edges_(std::move(conditional_edges)),
        entry_(std::move(entry)),
        cp_(cp),
        opts_(opts) {}

  // thread_id 非空且已有快照 → **从最新快照恢复**（input 被忽略）。
  // opts.sink 注入 → 逐包吐事件；opts.resume → 对上次 interrupt 的答复（P2）
  std::expected<Json, GraphError> invoke(Json const& input, std::string const& thread_id = "",
                                         InvokeOptions opts = {});

  [[nodiscard]] std::size_t last_steps() const { return last_steps_; }
  [[nodiscard]] std::string const& last_checkpoint_id() const { return last_checkpoint_; }
  [[nodiscard]] InterruptStatus last_interrupt() const { return last_interrupt_; }

  // 便捷收集：跑一遍并把事件全收下来（测试/demo 用；唯一执行通道仍是 invoke）
  std::vector<StreamEvent> stream(Json const& input, std::string const& thread_id = "");

 private:
  std::expected<std::vector<std::string>, GraphError> next_nodes(std::string const& node,
                                                                 Command const& cmd,
                                                                 Json const& state) const;

  void emit(StreamEvent ev);  // 分配 seq + 投递（sink_ 为空则静默）

  state::StateSchema schema_;
  std::unordered_map<std::string, NodeFn> nodes_;
  std::unordered_map<std::string, std::string> static_edges_;
  std::unordered_map<std::string, RouterFn> conditional_edges_;
  std::string entry_;
  state::Checkpointer* cp_;
  CompileOptions opts_;
  std::size_t last_steps_ = 0;
  std::string last_checkpoint_;
  InterruptStatus last_interrupt_;
  EventSink sink_;           // 仅本次 invoke 有效（invoke 开头设置、结束清空）
  EventMode mode_ = EventMode::Off;
  std::uint64_t last_seq_ = 0;
};

}  // namespace cppchain::graph