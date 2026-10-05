// cppchain —— graph/graph.hpp
// 极简 Graph 引擎（M6）：StateGraph → compile → CompiledGraph.invoke。
// 顺序 super-step + 每步 checkpoint + 从快照恢复（并行/Streaming/Interrupt 押后到 M10）。
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cppchain/state/checkpointer.hpp"
#include "cppchain/state/state.hpp"

namespace cppchain::graph {

using Json = nlohmann::json;

inline constexpr char kEnd[] = "__end__";

// 节点返回值：状态更新 + 控制流跳转（Command 简版）
struct Command {
  Json update = Json::object();
  std::vector<std::string> goto_nodes;  // 非空则覆盖静态/条件边

  static Command with_update(Json u) { return Command{std::move(u), {}}; }
  static Command jump(std::vector<std::string> nodes) { return Command{Json::object(), std::move(nodes)}; }
};

struct NodeContext {
  std::string node_name;
  std::string thread_id;
  int step = 0;
};

using NodeFn = std::function<Command(Json const& state, NodeContext& ctx)>;
using RouterFn = std::function<std::vector<std::string>(Json const& state)>;

struct GraphError {
  enum class Kind { InvalidGraph, UnknownNode, RecursionLimit, NodeError };
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

  // thread_id 非空且已有快照 → **从最新快照恢复**（input 被忽略）
  std::expected<Json, GraphError> invoke(Json const& input, std::string const& thread_id = "");

  [[nodiscard]] std::size_t last_steps() const { return last_steps_; }
  [[nodiscard]] std::string const& last_checkpoint_id() const { return last_checkpoint_; }

 private:
  std::expected<std::vector<std::string>, GraphError> next_nodes(std::string const& node,
                                                                Command const& cmd,
                                                                Json const& state) const;

  state::StateSchema schema_;
  std::unordered_map<std::string, NodeFn> nodes_;
  std::unordered_map<std::string, std::string> static_edges_;
  std::unordered_map<std::string, RouterFn> conditional_edges_;
  std::string entry_;
  state::Checkpointer* cp_;
  CompileOptions opts_;
  std::size_t last_steps_ = 0;
  std::string last_checkpoint_;
};

}  // namespace cppchain::graph