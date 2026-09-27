// cppchain —— workflow/ir.hpp
// 主动 workflow 的 IR 模型与解析（规范：主动智能/11-ir-syntax.md §1–§7）。
// IR = 一段顶层 JSON {id, trigger, nodes}；本模块只负责"看懂它"，不负责执行。
//
// 两条纪律：
//   · trigger 只表达"何时醒"（D1：元数据 + 校验，不做 OS 调度，编译期不使用）。
//   · 节点类型判定按 §3 优先级：显式 kind:label → goto → expr → tool；
//     同时出现多个决定性键 → 判为 ambiguous（比 §3 的"第一个命中即定"更严，fail-closed）。
#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace cppchain::workflow {

using Json = nlohmann::json;

// ---- trigger：v1 只放行 time/schedule（宿主导入的简写形）----
struct TriggerIR {
  std::string source = "time";               // 触发源：v1 只放行 "time"
  std::string kind = "schedule";             // timing.kind（简写形把 kind 提到 timing 下）
  std::string schedule;                      // 5 字段 cron，如 "0 8,20 * * *"
  std::optional<std::string> timezone;       // 简写形：由宿主时区补充，可缺省
  bool enabled = true;
  Json raw = Json::object();                 // 原样保留（元数据；校验只查 raw 的禁项）
};

enum class NodeKind { Tool, Expr, Label, Goto };

struct OnErrorIR {
  int retry = 0;                             // 重试次数（总尝试 = retry + 1）
  std::optional<std::string> fallback;       // 失败降级目标节点 id（可为 _turn_end）
};

struct NodeIR {
  std::string id;
  NodeKind kind = NodeKind::Tool;

  // Tool 节点
  std::string tool;                          // 必须与目录内工具完全同名（§8）
  Json parameters = Json::object();          // 只放该工具的 input 字段（可含 $ / @config 引用）
  Json output_schema = Json::object();       // 声明的字段即下游可引用集（§5）
  std::string next;                          // 成功出口（必填）
  std::optional<OnErrorIR> on_error;

  // Expr 节点（else 是 C++ 关键字 → 用 else_）
  std::string expr;
  std::string then;
  std::string else_;

  // Goto 节点
  std::string goto_target;                   // 指向一个 label 节点的 id（§4）
  int max = 0;                               // 循环有界（正整数）

  Json raw = Json::object();                 // 原始节点对象（校验回读用）
};

struct WorkflowIR {
  std::string id;
  TriggerIR trigger;
  std::vector<NodeIR> nodes;                 // nodes[0] = 入口（隐式，§3）

  // 从顶层 JSON 解析；失败返回人类可读错误（可直接回喂给生成器）。
  // 只做"看懂结构"，不做规则校验（规则在 validator.hpp，是另一道关）。
  static std::expected<WorkflowIR, std::string> parse(Json const& j);

  // 规范序列化（触发用简写形 {source, timing:{kind, schedule}}），用于 demo 落盘/展示。
  [[nodiscard]] Json to_json() const;

  // 按数组下标查节点；不存在 → nullopt。
  [[nodiscard]] std::optional<std::size_t> index_of(std::string const& id) const;

  // 节点类型判定（§3 优先级 + 歧义即错）。
  static std::expected<NodeKind, std::string> classify(Json const& node);
};

}  // namespace cppchain::workflow