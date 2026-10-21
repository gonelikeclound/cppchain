// cppchain —— tool/tool_plugin.hpp
// 动态工具库（wfr-㊹ 治理，problems/34）：符号约定 + C 描述符 + 宿主加载。
//
// 与 workflow/plugin_node.hpp（动态节点）同纪律：
//   · 库与宿主只通过一个符号 + 一个 C 结构耦合 —— `tool_<name>_creator` + `cppchain_tool_descriptor`。
//   · 跨 .so 传 `std::function` / nlohmann 对象不安全（各自 new/delete 可能不配对），
//     所以 handler/probe 用**裸 C 函数指针**、parameters 用 **C 字符串 JSON literal**；
//     宿主侧构造 ToolSpec 时把裸 fn 包成 std::function（std::function 在宿主侧构造）。
//   · 宿主加载流程 fail-closed：dlopen 失败 / 符号取不到 / 版本不匹配 / 名字不一致 → 拒绝。
#pragma once

#include <cstdint>
#include <cstddef>

// 版本常量（宿主侧）：加载库后**先校验**，不匹配即拒（fail-closed）。
inline constexpr int kToolApiVersion = 1;

// 库侧导出符号须用（CMake 默认 -fvisibility=hidden 时符号不导出）。
#define CPPCHAIN_TOOL_EXPORT __attribute__((visibility("default")))

// 工具副作用效果分类（对齐 cppchain::tool::EffectClass：0/1/2）。
inline constexpr int kToolEffectRead = 0;
inline constexpr int kToolEffectIdempotent = 1;
inline constexpr int kToolEffectIrreversible = 2;

// 审批级别（对齐 cppchain::tool::ToolApprovalLevel：0/1/2）。
inline constexpr int kToolApprovalNever = 0;
inline constexpr int kToolApprovalPolicy = 1;
inline constexpr int kToolApprovalNeedHuman = 2;

// 工具执行 handler 的**裸 C 签名**（跨 DSO 安全）：
//   · args_json    —— 参数对象（JSON literal 字符串，宿主 parse 成 ToolSpec 参数用）
//   · out_json     —— 成功：写结果 JSON 字符串（宿主负责 free）
//   · err_out      —— 失败：写错误消息字符串（宿主负责 free）
// 返回 0 成功 / 非 0 失败（err_out 必有值）。
// 注意：宿主与库**同一分配器**（同一 libstdc++/libc），宿主 free 库 malloc 的字符串才安全
//（同节点插件的"同一工具链"限定，见 plugin_node.hpp §5.1 末尾）。
using ToolRunFn = int (*)(char const* args_json, char** out_json, char** err_out);

// 工具产物探针的裸 C 签名（崩溃缺环兜底；可为 null = 无探测 → ExecutionGuard fail-closed）。
// 返回 0=Complete / 1=Partial / 2=Absent / 3=Unknown（对齐 cppchain::tool::ProbeResult）。
using ToolProbeFn = int (*)(char const* args_json);

// 纯 C 布局。字段存在理由（对齐节点版 cppchain_node_descriptor）：
//   · name        —— 按名加载自校验：符号名与声明名不一致立刻发现
//   · version     —— ABI 校验：接口布局变就是静默内存错乱，不匹配 → 拒绝
//   · context     —— 库自己的上下文，宿主原样持有不解释
//   · description/parameters_json —— 宿主构造 ToolSpec 的 schema 素材（C 字符串 JSON literal）
//   · read_only/effect_class/approval —— 副作用契约（宿主 .with_effect_class / .approval 装配）
//   · ttl_ms      —— 结果时效（0 = 不过期）
//   · handler/probe —— 裸 C 函数指针（宿主包成 std::function）
struct cppchain_tool_descriptor {
  const char* name;             // 工具名：须与符号里的 <name> 一致，宿主校验
  int version;                  // 本库编译时的 kToolApiVersion
  void* context;                // 库自己的上下文；宿主不解释
  const char* description;      // 工具描述
  const char* parameters_json;  // JSON Schema literal 字符串（宿主 parse 进 ToolSpec.parameters）
  int read_only;                // 0/1：副作用标记
  int effect_class;             // kToolEffectRead/Idempotent/Irreversible
  int approval;                 // kToolApprovalNever/Policy/NeedHuman
  std::int64_t ttl_ms;          // 结果时效（0 = 不过期）
  ToolRunFn handler;            // 必须非 null
  ToolProbeFn probe;            // 可为 null（无探测 → fail-closed）
};

// 库侧 creator 函数签名（名字即约定，<name> 必须是合法 C 标识符）：
//   extern "C" CPPCHAIN_TOOL_EXPORT cppchain_tool_descriptor const* tool_<name>_creator(void*);
using ToolCreatorFn = cppchain_tool_descriptor const* (*)(void* host_ctx);
