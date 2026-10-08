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
#include <string>
#include <string_view>
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

}  // namespace cppchain::agent
