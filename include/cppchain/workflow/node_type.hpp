// cppchain —— workflow/node_type.hpp
// 节点类型接口与注册表（M12 重构 ①，见 02-arch-design/12）。
//
// 要解决的问题（见该文 §2）：加一种节点原先要在 6 个文件、9 处分别补齐——因为节点的
// "JSON 声明"与"C++ 实现"之间没有绑定。本文件把两者绑在一处，于是
// **加一种节点 = 加一个 NodeType + 一行注册**。
//
// 分发：② 起图规则由 edges() 统一驱动；⑤ 起**框架分发一律按名**（node_type_of_name，
// 内置先、导入后；NodeKind 只留给 label/goto/on_error 等结构性判定，见 §4.3）。
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/graph/graph.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/session_log.hpp"  // SessionSink

namespace cppchain::tool {
class ExecutionGuard;  // 前置声明：CompileContext 只持有非拥有指针，规避 tool.hpp 重 include
}

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

  // 校验期上下文：节点只看到它需要的（不把整个 ValidatorOptions 灌进来）
  struct ValidateContext {
    tool::ToolRegistry const* registry = nullptr;
    ComponentRegistry const* components = nullptr;
  };
  using ErrorSink = std::function<void(std::string rule, std::string message)>;

  // 类型专属校验（VN5–VN11 那一类）。**通用图规则**（目标存在/无环/可达/死 label）
  // 不在这里——它们由框架按 edges() 统一做（②）。
  std::function<void(NodeIR const&, ValidateContext const&, ErrorSink const&)> validate;

  // 编译期上下文：节点把自己变成图节点所需的一切。
  // 用引用成员（graph 不可拷贝）；error 非空表示编译失败，调用方就此中止。
  struct CompileContext {
    graph::StateGraph& graph;
    tool::ToolRegistry const* registry = nullptr;
    ComponentRegistry const* components = nullptr;
    // label / _turn_end / _fail → 图上的目标名（由 compiler 提供）
    std::function<std::string(std::string const&)> resolve;
    std::size_t registered = 0;  // 成功注册的图节点数（供 registered_nodes()）
    std::string error;
    // 触发期注入的会话日志句柄（docs/12 §6.4 / §7）：节点 wrapper 捕获它，
    // 运行期经 sink->nlog() 写 source=node 的诊断日志；引擎结构事件由 compiler 的
    // LoggingStateGraph 统一埋点，节点不用管。可为空（不落日志的触发）。
    std::shared_ptr<SessionSink> sink;
    // wfr-㊷：副作用执行点防护（非拥有；null = 不防护）。c_tool / c_run_tools 编译时捕获，
    // 运行期在工具执行点调用。由 CompileOptions::tool_policy 注入。
    tool::ExecutionGuard const* guard = nullptr;
  };
  std::function<void(NodeIR const&, CompileContext&)> compile;

  // **出边** —— 通用图规则（VG1/VG2/VG7/VG8）唯一需要知道的东西。
  // 有了它，那几条规则就不必再按 kind 各写一遍（原先 28 处 if 链的来源）。
  struct Edge {
    std::string target;          // 原始目标名（label 名 / 节点 id / _turn_end / _fail）
    std::string label;           // 出边字段名，仅用于报错（"next" / "cases.text" / "on_error.fallback"…）
    bool forward = true;         // false = 回边（只有 goto 是）
  };
  std::function<std::vector<Edge>(NodeIR const&)> edges;

  // 本节点里**可能含引用**的文本片段（path/expr/parameters 里的字符串…）。
  // VR1 的语法检查与 VR 段的引用收集**共用这一个钩子** —— 原先两处各自按 kind 走，
  // 都只覆盖 Tool/Expr，于是 history 节点的 path/session 里的引用**两边都漏**
  // （拼错的 @config 键能一路通过校验，运行期 resolve 成 "null" 当目录用）。
  std::function<std::vector<std::string>(NodeIR const&)> ref_texts;
};

// 内置节点类型表（9 种）。静态、不可变、生命周期贯穿进程。
[[nodiscard]] std::vector<NodeType> const& builtin_node_types();

// 按内部判别取类型（内置表）。Dynamic 节点**不要**用它 —— 分发一律按名字走。
[[nodiscard]] NodeType const* node_type_of(NodeKind k);

// ---- ⑤ 运行期导入（docs/12 §5）----
// 内置 + 运行期导入的**统一按名查找**：内置先、导入后；找不到 → nullptr。
// 这是 ③④⑤ 之后框架的**唯一分发入口**（parse/validator/compiler/展示都用它）。
[[nodiscard]] NodeType const* node_type_of_name(std::string_view name);

// 注册一个导入的节点类型（宿主在 dlopen/校验后调用）。名字与内置冲突 / 重复导入 → false。
// 传入的 name 会被**进程期稳定持有**，NodeType::name 指向它（与内置表的生命周期同规）。
[[nodiscard]] bool register_dynamic_node_type(std::string name, std::unique_ptr<NodeType> t);

// 一个节点的规范类型名：内置 → NodeType::name（历史写法也归一到它）；
// 动态 → JSON 里的 kind 字面量。用于所有按名分发点。
[[nodiscard]] std::string type_name_of(NodeIR const& n);

// 判别 + 查找一个 JSON 节点（parse 用）：内置按 matches（显式/历史写法，命中>1 歧义 → 不返回），
// 未命中内置时按显式 kind 名找导入节点；都不中 → nullptr（调用方报"未知 kind"）。
[[nodiscard]] NodeType const* node_type_for_node(Json const& node);

}  // namespace cppchain::workflow
