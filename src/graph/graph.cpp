// cppchain —— src/graph/graph.cpp
#include "cppchain/graph/graph.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace cppchain::graph {

StateGraph& StateGraph::add_node(std::string name, NodeFn fn) {
  if (nodes_.find(name) == nodes_.end()) order_.push_back(name);
  nodes_[name] = std::move(fn);
  return *this;
}

StateGraph& StateGraph::add_edge(std::string from, std::string to) {
  static_edges_[std::move(from)] = std::move(to);
  return *this;
}

StateGraph& StateGraph::add_conditional_edges(std::string from, RouterFn router) {
  conditional_edges_[std::move(from)] = std::move(router);
  return *this;
}

StateGraph& StateGraph::set_entry_point(std::string name) {
  entry_ = std::move(name);
  return *this;
}

std::expected<CompiledGraph, GraphError> StateGraph::compile(state::Checkpointer* cp,
                                                             CompileOptions opts) const {
  auto err = [](std::string msg) {
    return std::unexpected(GraphError{GraphError::Kind::InvalidGraph, std::move(msg)});
  };

  if (nodes_.empty()) return err("graph has no nodes");
  if (entry_.empty()) return err("entry point not set");
  if (nodes_.find(entry_) == nodes_.end()) return err("entry point is not a node: " + entry_);

  // 静态边与条件边不可并存于同一节点（对齐 langchain 约束）
  for (auto const& [from, router] : conditional_edges_) {
    (void)router;
    if (nodes_.find(from) == nodes_.end()) return err("conditional edge from unknown node: " + from);
    if (static_edges_.find(from) != static_edges_.end())
      return err("node '" + from + "' has both static and conditional edges");
  }

  for (auto const& [from, to] : static_edges_) {
    if (nodes_.find(from) == nodes_.end()) return err("edge from unknown node: " + from);
    if (to != kEnd && nodes_.find(to) == nodes_.end()) return err("edge to unknown node: " + to);
  }

  return CompiledGraph(schema_, nodes_, static_edges_, conditional_edges_, entry_, cp, opts);
}

std::expected<std::vector<std::string>, GraphError> CompiledGraph::next_nodes(
    std::string const& node, Command const& cmd, Json const& state) const {
  // 1) Command.goto 优先（控制流原语）
  if (!cmd.goto_nodes.empty()) return cmd.goto_nodes;

  // 2) 静态边
  if (auto const it = static_edges_.find(node); it != static_edges_.end())
    return std::vector<std::string>{it->second};

  // 3) 条件边
  if (auto const it = conditional_edges_.find(node); it != conditional_edges_.end())
    return it->second(state);

  return std::vector<std::string>{};  // 无出边 → 结束
}

std::expected<Json, GraphError> CompiledGraph::invoke(Json const& input,
                                                      std::string const& thread_id) {
  last_steps_ = 0;
  last_checkpoint_ = "";

  Json state = input;
  std::vector<std::string> current{entry_};
  std::optional<std::string> parent;

  // 有快照 → 从最新快照恢复（状态 + 待跑节点），继续推进
  if (cp_ != nullptr && !thread_id.empty()) {
    if (auto snap = cp_->get_tuple(thread_id, "", std::nullopt); snap.has_value()) {
      state = snap->values;
      current.clear();
      for (auto const& p : snap->next)
        current.push_back(p.node);  // P0 过渡：M10 前 next 均为"全局状态包"，先取回名字
      parent = snap->config.value("checkpoint_id", "");
      if (current.empty()) return state;  // 上次已跑完
    }
  }

  while (!current.empty()) {
    if (last_steps_ >= opts_.max_steps)
      return std::unexpected(GraphError{GraphError::Kind::RecursionLimit,
                                        "exceeded max_steps=" + std::to_string(opts_.max_steps)});

    std::string const node = current.front();
    if (node == kEnd) break;

    auto const it = nodes_.find(node);
    if (it == nodes_.end())
      return std::unexpected(GraphError{GraphError::Kind::UnknownNode, "unknown node: " + node});

    NodeContext ctx{node, thread_id, static_cast<int>(last_steps_)};
    Command cmd;
    try {
      cmd = it->second(state, ctx);
    } catch (std::exception const& e) {
      return std::unexpected(GraphError{GraphError::Kind::NodeError,
                                        "node '" + node + "' threw: " + e.what()});
    }

    state = schema_.apply(state, cmd.update);  // 经 per-key reducer 归约

    auto next = next_nodes(node, cmd, state);
    if (!next) return std::unexpected(next.error());

    // END 不是节点：先从 next 里剔除，避免污染快照（恢复语义 = "待执行的节点"）
    next->erase(std::remove(next->begin(), next->end(), std::string(kEnd)), next->end());

    // 每步落一个快照：values + next（恢复语义 = 回到该步边界继续）
    if (cp_ != nullptr && !thread_id.empty()) {
      Json meta = {{"step", last_steps_}, {"node", node}};
      std::vector<state::Packet> pkts;  // M10 前 next 仍是节点名 → 等价于"全局状态包"
      pkts.reserve(next->size());
      for (auto const& n : *next) pkts.emplace_back(n);
      last_checkpoint_ = cp_->put(thread_id, "", parent, state, pkts, meta);
      parent = last_checkpoint_;
    }

    current = *next;
    ++last_steps_;
  }

  return state;
}

}  // namespace cppchain::graph