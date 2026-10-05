// cppchain —— workflow/plugin_node.hpp
// 动态库节点（docs/02-arch-design/12 §5）：符号约定 + C 描述符 + 抽象接口 + 宿主加载。
//
// 分层：
//   · 库与宿主只通过**一个符号 + 一个 C 结构**耦合（§5.1）——符号即
//     `graph_node_<name>_creator`，C 结构是 `cppchain_node_descriptor`（纯 C 布局）。
//   · 跨 .so 传 `std::function` 不安全，所以库实现的是**抽象接口 `DynamicNode`**
//     （vtable）；宿主把它包成 NodeType 的 std::function 适配器（std::function 在宿主侧
//     构造）。二者同一工具链（§5.1 末尾的限定）。
//   · 宿主加载流程 fail-closed（§5.1/§5.5）：dlopen 失败 / 符号取不到 / 版本不匹配 /
//     名字与符号不一致 / create 缺失 → 一律拒绝，不注册。
#pragma once

#include <expected>
#include <string>

#include "cppchain/workflow/node_type.hpp"

namespace cppchain::workflow {

// 版本常量（宿主侧）：加载库后**先校验**，不匹配即拒（fail-closed）。
inline constexpr int kNodeApiVersion = 1;

// 库侧导出符号须用（CMake 默认 -fvisibility=hidden 时符号不导出，这是最常见
// "节点明明写了却找不到"的根因，§5.4）。
#define CPPCHAIN_NODE_EXPORT __attribute__((visibility("default")))

// 纯 C 布局（链接规范只作用于函数/变量，不作用于类型）。四个字段各有存在理由（§5.1）：
//   · name    —— 按名加载时的自校验：符号名与声明名不一致（改名漏改）会被立刻发现
//   · version —— ABI 校验：宿主与库的接口布局一变就是静默内存错乱，不匹配 → 拒绝
//   · context —— 库自己的上下文，宿主原样持有、不解释；create/destroy 时回传
//   · create/destroy —— 由库提供：跨 .so 的 new/delete 必须配对
struct cppchain_node_descriptor {
  const char* name;       // 节点名：须与符号里的 <name> 一致，宿主校验；不一致直接拒
  int version;            // 本库编译时的 kNodeApiVersion
  void* context;          // 库自己的上下文；宿主不解释
  void* (*create)(void* context);               // 由库分配
  void  (*destroy)(void* node, void* context);  // 由库释放
};

// 库侧 creator 函数签名（名字即约定，<name> 必须是合法 C 标识符）：
//   extern "C" CPPCHAIN_NODE_EXPORT cppchain_node_descriptor const* graph_node_<name>_creator(void*);
using NodeCreatorFn = cppchain_node_descriptor const* (*)(void* host_ctx);

// 库侧实现的**抽象节点接口**：把"JSON ↔ NodeIR + 出边 + 引用 + 校验 + 编译"暴露给宿主。
// 宿主构造 NodeType 时把它包成函数表适配器。方法全 const：库的状态放自己的 context 里。
class DynamicNode {
 public:
  virtual ~DynamicNode() = default;
  virtual bool matches(Json const&) const = 0;
  virtual void parse(NodeIR&, Json const&) const = 0;
  virtual void write(Json&, NodeIR const&) const = 0;
  virtual std::string describe(NodeIR const&) const = 0;
  virtual std::vector<NodeType::Edge> edges(NodeIR const&) const = 0;
  virtual std::vector<std::string> ref_texts(NodeIR const&) const = 0;
  virtual void validate(NodeIR const&, NodeType::ValidateContext const&,
                        NodeType::ErrorSink const&) const = 0;
  virtual void compile(NodeIR const&, NodeType::CompileContext&) const = 0;
};

// ---- 宿主侧加载 ----
// 加载一个动态节点并注册（dlopen → dlsym → creator → 校验 → 注册）。失败 → 不注册、
// 返回人类可读错误（fail-closed）。name 已被占用（内置或已导入）→ 拒绝。
[[nodiscard]] std::expected<void, std::string> load_dynamic_node(std::string const& lib_path,
                                                                 std::string const& name);

// 顶层 `imports` 的解析 + 加载 + 注册（fail-closed），CLI/宿主在 parse 前调用：
//   · 无 imports 键 → 成功（什么都不做）；
//   · 有 imports 但 !allow_import → 拒绝（§5.5：加载 .so = 任意代码执行，需显式放行）；
//   · 逐个库、逐个节点 dlopen/dlsym/校验；**任一失败 → 整体失败，不注册任何东西**
//     （避免部分注册后 IR 判别产生半吊子状态）；
//   · host_log 非空 → 把"本次加载了什么"写进会话日志（§5.5：事后可查）。
// 返回实际注册的节点数。
[[nodiscard]] std::expected<std::size_t, std::string>
load_workflow_imports(Json const& ir_json, bool allow_import, SessionLog* host_log = nullptr);

}  // namespace cppchain::workflow