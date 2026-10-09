// cppchain —— agent/history.hpp
// 历史压缩（wfr-㉙）：把「日志投影出来的 messages」按策略渲染成送给模型的历史。
//
// 三档（wfr-㉙ 的对照实验变量）：
//   full         恒等：逐条保留（含 ReasoningBlock）——基线，等于压缩能力引入前的行为；
//   drop_round   丢「ai(tool_calls) + 其后连续 tool」整组，并删所有 ai 的 ReasoningBlock
//                ——反例档：实测会让模型否认自己查过（"我上一条其实是编造的"）；
//   keep_evidence 不丢消息：删 ReasoningBlock；超预算的 tool 结果改写为「凭据行」
//                ——推荐档：保住"我确实调用过这个工具、拿到过这个结果"的最小凭据，
//                  只压体积（reasoning + 超长工具返回）。
//
// 纯函数、无 IO：压缩是**投影的渲染期变换**，结果绝不落盘（见 02-arch-design/15）。
// token 口径走 tokens::estimate_msgs（启发式，非端点 usage：相对趋势可信、绝对值不可引用）。
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/message/message.hpp"

namespace cppchain::agent {

using cppchain::message::Message;

enum class HistoryMode {
  Full,
  DropRound,
  KeepEvidence,
};

// 词表名（与 workflow JSON 里 mode 字段的取值一致）。
[[nodiscard]] std::string_view history_mode_name(HistoryMode mode);

// 词表解析。**不区分**"缺省"与"非法"——两者都返回 nullopt，由调用方决定语义：
//   缺省（未设 / 空 / "@config.X" 解析成 null）→ 回落 Full；
//   非空但拼错（如 --set history_mode=keep_evidnce）→ 调用方抛硬错（fail-closed）。
[[nodiscard]] std::optional<HistoryMode> parse_history_mode(std::string_view text);

struct HistoryStats {
  HistoryMode mode = HistoryMode::Full;
  std::size_t budget = 200;      // 仅 KeepEvidence 用（单条 tool 结果的内联 token 预算）
  std::size_t before_msgs = 0;
  std::size_t after_msgs = 0;
  std::size_t before_tokens = 0;  // tokens::estimate_msgs 口径
  std::size_t after_tokens = 0;
  std::size_t dropped_msgs = 0;    // DropRound 丢弃的消息数
  std::size_t rewritten_msgs = 0;  // KeepEvidence 改写的 tool 消息数
};

// 原地压缩 msgs。source_hint 非空时写进凭据行的来源标注（如会话日志路径）。
HistoryStats compress_history(std::vector<Message>& msgs, HistoryMode mode,
                              std::size_t budget_tokens = 200,
                              std::string_view source_hint = {});

// 供节点发布 result 用（字段名与 workflow 里的 $<id>.result.<f> 一致）。
[[nodiscard]] nlohmann::json history_stats_json(HistoryStats const& stats);

// ---- wfr-㊵：规则化的历史精简与长内容裁剪 ----
//
// 与上面三档的关系：三档是"固定的整体策略"（渲染期变换、不落盘）；这里是**可组合的规则
// 清单**，产出**计划**交给节点固化成 op 落进会话日志（见 02-arch-design/17 的一致性论证）。
// chat2 继续用三档，hist_rules / hist_clip 用规则。
//
// 纯函数：语料与工具类别都由调用方以**纯数据**传入 —— agent 层刻意不 include 任何 tool
// 头文件（依赖纪律；类别是"工具名 → 类别名"的 map 快照）。

struct HistoryRuleParams {
  std::size_t tool_budget = 200;  // clip_tool_results：单条工具结果的内联 token 预算
  std::size_t ai_budget = 400;    // clip_ai_text：单条**终态** AI 回答的 token 预算
  bool artifact_mode = false;     // true：超预算的工具结果落盘成引用行；false：就地截断
  std::string artifact_dir;       // artifact_mode 用（缺省不落盘）
  // 工具名 → 类别。由节点在**编译期**从 ToolRegistry 快照；查不到的名字视为 "general"。
  std::unordered_map<std::string, std::string> tool_category;
};

struct HistoryRuleStats {
  std::size_t before_msgs = 0, after_msgs = 0;
  std::size_t before_tokens = 0, after_tokens = 0;  // tokens::estimate_msgs 口径
  std::size_t dropped_msgs = 0;                     // 总删除条数
  std::size_t dropped_groups = 0;                   // 整组删除的工具轮数
  std::size_t dropped_error_groups = 0;             // 其中"工具结果全错"的
  std::size_t dropped_category_groups = 0;          // 其中"整组属于某类别"的
  std::size_t dropped_orphan_tools = 0;             // 孤儿 Tool 消息（安全网规则）
  std::size_t stripped_reasoning = 0;               // 被剥掉推理块的消息数
  std::size_t rewritten_tools = 0;                  // 被裁剪的工具结果数
  std::size_t rewritten_ai = 0;                     // 被裁剪的终态 AI 回答数
  std::size_t artifacts_written = 0;                // 落盘的 artifact 数
};

// 一段历史该做的删改（**计划**：下标 + 改后的消息）。节点把它固化成 transform op，
// reducer 只做机械应用 —— 于是重放同一条 op 必然得到同样结果。
struct HistoryRulePlan {
  std::vector<std::size_t> drop;                      // 升序下标
  std::vector<std::pair<std::size_t, Message>> rewrite;  // (下标, 改后的消息)
  HistoryRuleStats stats;
};

// 规则名是否被识别。`drop_category:<类别>` 只校验**形状**（前缀 + 非空后缀）——类别词表在
// tool 层，由 workflow 的 validator 用 tool::is_known_category 校（分层纪律）。
[[nodiscard]] bool is_known_history_rule(std::string_view rule);
// `drop_category:<c>` → <c>；不是该形状则 nullopt。
[[nodiscard]] std::optional<std::string_view> history_rule_category(std::string_view rule);

// preset 词表：minimal | lean | aggressive | clip（解析失败 → nullopt，调用方硬错）
[[nodiscard]] std::optional<std::string_view> parse_history_preset(std::string_view text);
// 把 preset + 静态规则清单 + clip 门控（tool_results | ai_text | both）合成最终规则集合：
// 去重、按固定优先级排序。clip 门控只作用于两条 clip 规则（便于"只裁 AI 长回答"这类档位）。
[[nodiscard]] std::vector<std::string> resolve_history_rules(std::string_view preset,
                                                             std::span<std::string const> extra,
                                                             std::string_view clip_scope = "both");

// 按规则集合算出计划（规则当**集合**看待，JSON 里的书写顺序不生效；组粒度规则见 §规则表）。
[[nodiscard]] HistoryRulePlan plan_history_rules(std::vector<Message> const& msgs,
                                                 std::span<std::string const> rules,
                                                 HistoryRuleParams const& params,
                                                 std::string_view source_hint = {});

[[nodiscard]] nlohmann::json history_rule_stats_json(HistoryRuleStats const& stats);

}  // namespace cppchain::agent
