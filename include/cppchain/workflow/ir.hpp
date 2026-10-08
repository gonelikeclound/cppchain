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
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cppchain::workflow {

using Json = nlohmann::json;

// ---- M12：保留目标名（编译期映射，不是节点）----
inline constexpr char kTurnEndName[] = "_turn_end";  // 正常终止
inline constexpr char kFailName[] = "_fail";         // 以错误终止（route 的 fail-closed 档）

// ---- M12：route 的判别式与 outcome（**封闭词表**）----
// 判别式用 cppchain 自己归一化过的 outcome，不用端点给的自由字符串（如 finish_reason）——
// 分层上"端点差异由适配层吃住"，且端点字符串有实测差异（见 02-arch-design/11 §5.2.3）。
inline constexpr char kOnLastAiOutcome[] = "last_ai_outcome";
inline constexpr char kOutcomeToolCalls[] = "tool_calls";
inline constexpr char kOutcomeInvalidToolCalls[] = "invalid_tool_calls";
inline constexpr char kOutcomeText[] = "text";

// 校验与编译共用同一份词表，避免两处漂移
[[nodiscard]] inline bool is_known_discriminant(std::string_view s) {
  return s == kOnLastAiOutcome;
}
[[nodiscard]] inline bool is_known_outcome(std::string_view s) {
  return s == kOutcomeToolCalls || s == kOutcomeInvalidToolCalls || s == kOutcomeText;
}

// ---- trigger：v1 只放行 time/schedule（宿主导入的简写形）----
struct TriggerIR {
  std::string source = "time";               // 触发源：v1 只放行 "time"
  std::string kind = "schedule";             // timing.kind（简写形把 kind 提到 timing 下）
  std::string schedule;                      // 5 字段 cron，如 "0 8,20 * * *"
  std::optional<std::string> timezone;       // 简写形：由宿主时区补充，可缺省
  bool enabled = true;
  Json raw = Json::object();                 // 原样保留（元数据；校验只查 raw 的禁项）
};

enum class NodeKind {
  Tool,
  Expr,
  Label,
  Goto,
  // ---- M12（组件化与命令行 agent，见 02-arch-design/11）----
  Model,     // 一次 LLM 调用：读 state.messages → 追加回复（不循环）
  Route,     // match 分派：按 last_ai_outcome 选下一跳
  RunTools,  // 执行最后一条 AI 消息的 tool_calls → ToolMessage 追加进 messages
  LoadHistory,   // 工作路径的历史消息 → 追加进 state.messages（④：日志的 replay 投影）
  // （save_history 已随 ④ 删除：history = 日志的投影，不再单独落盘）
  // ---- wfr-㉗：回显节点（屏之道回显 thinking/工具摘要，纯副作用不改 state）----
  Echo,
  // ---- ⑤ 动态库导入 ----
  Dynamic,   // 内含 kind 名匹配`导入的动态节点`（见 cppchain/workflow/plugin_node.hpp）；
             // 注意：**框架分发一律按名字走**（node_type_of_name），Dynamic 仅供
             // label/goto/on_error 等结构性判定时区分"不是内置节点"。
};

// ---- ⑤ 动态库导入声明（docs/02-arch-design/12 §5.3）----
struct ImportIR {
  std::string lib;                 // 库路径（**信任单元**，永远显式列出）
  std::vector<std::string> nodes;  // 显式节点名（A1 枚举落地前省略即报错）；取不到 → 校验期拒
};

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

  // Model 节点（M12）：一次模型调用。model/toolset 都是**组件名**（宿主注册，见 components.hpp）
  std::string model;                         // 必填：模型组件名
  std::string toolset;                       // 可选：工具集组件名（空 = 不带工具）
  std::string system_prompt;                 // 可选：本节点追加的 system 提示
  // wfr-㉙：厂商私有参数，原样并入请求体（→ CallOptions::extra）。值是运行期参数，
  // 可含 `@config.*`；解析为 null 的**顶层键会被丢弃**（不把 "k":null 发给端点）。
  Json model_params = Json::object();
  // wfr-㉙：出站是否回传 reasoning_content（drop / tool_calls / always，可写 `@config.*`）。
  std::string reasoning_echo;

  // Route 节点（M12）：match 分派。判别式与 outcome 都是**封闭词表**
  std::string on;                            // 判别式名，初版只有 "last_ai_outcome"
  std::map<std::string, std::string> cases;  // outcome → 目标节点 id（或保留名）
  std::string default_;                      // 未命中目标；**必须显式给出**（禁止漏档静默）

  // LoadHistory 节点（M12 / ④）：workspace/session 是运行期取值的**参数**
  // （可写 `@config.workspace` 这类引用，与该层其它参数同一套解析；历史即日志的投影）
  std::string path;
  std::string session;
  // wfr-㉙：读出来的历史按策略渲染（full / drop_round / keep_evidence，可写 `@config.*`）。
  // 压缩是**投影的渲染期变换**：结果绝不落盘（回声抑制见 compiler.cpp），故不影响 replay。
  std::string history_mode;
  std::string history_budget;                // keep_evidence 的单条工具结果内联 token 预算（可选）

  Json raw = Json::object();                 // 原始节点对象（校验回读用）
};

struct WorkflowIR {
  std::string id;
  // 可选：无 trigger 即「宿主手工触发」（wfr 直接拉起），调度语义由宿主决定（D1 无 OS 调度）
  std::optional<TriggerIR> trigger;
  std::vector<NodeIR> nodes;                 // nodes[0] = 入口（隐式，§3）
  std::vector<ImportIR> imports;             // ⑤：动态库导入声明（顶层"imports"键）

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