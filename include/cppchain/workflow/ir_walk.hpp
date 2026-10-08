// cppchain —— workflow/ir_walk.hpp
// IR 的**递归遍历**与循环作用域（wfr-㉚：loop 容器引入后，节点不再只有一层）。
//
// 为什么不 mangle 子节点 id：调试台的图、会话日志、state.results / loop_counts 的键
// 都按**节点 id** 对齐。加前缀（import 那套 `pfx__id`）会让"图上看到的 id"与"日志里的
// id"分叉，调试时对不上。代价是作者要给不同 loop 里的同名节点自己取不同的名字 —— 本
// 项目只允许一层 loop，代价可控。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppchain/workflow/ir.hpp"

namespace cppchain::workflow {

// 一个节点所处的循环作用域；nullptr = 顶层（不在任何 loop body 内）。
// exit_target 是该 loop 的 next **原始写法**（可为节点 id 或 _turn_end/_fail）——
// 本项目不支持 loop 套 loop，所以它必然是外层（顶层）的目标，不必再链式解析。
struct LoopScope {
  std::string loop_id;
  std::string exit_target;
};

namespace detail {

template <typename F>
void walk_node_list(std::vector<NodeIR> const& nodes, LoopScope const* scope, F& fn) {
  for (NodeIR const& n : nodes) {
    if (!fn(n, scope)) return;
    if (n.kind == NodeKind::Loop && !n.body.empty()) {
      LoopScope const inner{n.id, n.next};
      walk_node_list(n.body, &inner, fn);
    }
  }
}

}  // namespace detail

// 前序遍历：loop 节点头先用**外层**作用域访问，再以其 body 用**内层**作用域访问。
// fn 返回 false 可提前结束整棵遍历。
template <typename F>
void for_each_node(WorkflowIR const& ir, F&& fn) {
  auto f = std::forward<F>(fn);
  detail::walk_node_list(ir.nodes, nullptr, f);
}

// 前序扁平序号：loop 头 → 它的 body → 其后节点。
// 用来替代"nodes 数组下标"判"谁在上游"（VR2/VR3/VG9）：**loop-free 时与旧下标逐字节相同**。
[[nodiscard]] inline std::unordered_map<std::string, std::size_t> flatten_index(
    WorkflowIR const& ir) {
  std::unordered_map<std::string, std::size_t> idx;
  std::size_t i = 0;
  for_each_node(ir, [&](NodeIR const& n, LoopScope const*) {
    idx.emplace(n.id, i++);
    return true;
  });
  return idx;
}

// id → 所属 loop 的 id（顶层节点不在表里）。调试台按它给节点打归属徽标。
[[nodiscard]] inline std::unordered_map<std::string, std::string> loop_owner(
    WorkflowIR const& ir) {
  std::unordered_map<std::string, std::string> owner;
  for_each_node(ir, [&](NodeIR const& n, LoopScope const* scope) {
    if (scope != nullptr) owner.emplace(n.id, scope->loop_id);
    return true;
  });
  return owner;
}

// id → 它的作用域（只有 loop body 内的节点在表里）。
[[nodiscard]] inline std::unordered_map<std::string, LoopScope> loop_scopes(
    WorkflowIR const& ir) {
  std::unordered_map<std::string, LoopScope> scopes;
  for_each_node(ir, [&](NodeIR const& n, LoopScope const* scope) {
    if (scope != nullptr) scopes.emplace(n.id, *scope);
    return true;
  });
  return scopes;
}

// 该目标名是不是 loop 的回边（`_continue`）。**只看向量本身**，与作用域无关 ——
// 所以 NodeType::edges()（签名只吃一个节点）也能正确标出回边，VG2 的判据因此不用改。
[[nodiscard]] inline bool edge_is_backward(std::string_view target) {
  return target == kContinueName;
}

}  // namespace cppchain::workflow
