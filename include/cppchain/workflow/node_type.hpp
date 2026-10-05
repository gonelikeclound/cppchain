// cppchain —— workflow/node_type.hpp
// 节点类型接口与注册表（M12 重构 ①，见 02-arch-design/12）。
//
// 要解决的问题（见该文 §2）：加一种节点原先要在 6 个文件、9 处分别补齐——因为节点的
// "JSON 声明"与"C++ 实现"之间没有绑定。本文件把两者绑在一处，于是
// **加一种节点 = 加一个 NodeType + 一行注册**。
//
// 本步（①）保留 NodeKind 枚举作内部判别；彻底去掉枚举要等 ②（把图规则收敛到 edges()）。
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/workflow/ir.hpp"

namespace cppchain::workflow {

// 一个节点类型的全部"与类型有关"的行为。用函数表而非虚基类：与既有 ToolSpec 同风格，
// 可静态常量初始化，无需为每种节点声明一个类。
struct NodeType {
  NodeKind kind;                                       // 内部判别（enumerate）
  std::string_view name;                               // JSON 里的 kind 字面量（报错/展示用）

  // 判别。**必须覆盖历史写法**（靠 goto/expr/tool 键）与显式写法（kind:"…"）两种。
  std::function<bool(Json const&)> matches;

  // JSON ↔ NodeIR（只管**本类型自己的字段**，公共字段由 ir.cpp 统一处理）
  std::function<void(NodeIR&, Json const&)> parse;
  std::function<void(Json&, NodeIR const&)> write;

  // 一行易读描述（demo 展示用；把展示也纳入类型，免得再加一种节点还要改 demo 的 switch）
  std::function<std::string(NodeIR const&)> describe;

  // ---- 以下在 ①b / ①c 接上 ----
  // std::function<void(NodeIR const&, ValidationResult&)> validate;
  // std::function<void(NodeIR const&, CompileContext&)> compile;
};

// 内置节点类型表（9 种）。静态、不可变、生命周期贯穿进程。
[[nodiscard]] std::vector<NodeType> const& builtin_node_types();

// 按内部判别取类型；找不到 → nullptr（调用方应视为内部错误）
[[nodiscard]] NodeType const* node_type_of(NodeKind k);

}  // namespace cppchain::workflow
