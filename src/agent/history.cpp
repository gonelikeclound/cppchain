// cppchain —— src/agent/history.cpp
#include "cppchain/agent/history.hpp"

#include <string>
#include <unordered_map>
#include <utility>

#include "cppchain/agent/tokens.hpp"

namespace cppchain::agent {

using cppchain::message::ReasoningBlock;
using cppchain::message::Role;
using cppchain::message::TextBlock;

namespace {

// UTF-8 安全的字符边界回退（与 agent.cpp / artifact.cpp 同一口径：绝不切断多字节字符）。
std::size_t char_boundary(std::string_view s, std::size_t cut) {
  while (cut > 0 && cut < s.size() && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return cut;
}

// 取"估算 token 数 ≤ max_tokens"的最长前缀长度（字节）。tokens::estimate 对前缀非降 → 可二分。
std::size_t prefix_within_tokens(std::string_view s, std::size_t max_tokens) {
  namespace tok = cppchain::agent::tokens;
  if (tok::estimate(s) <= max_tokens) return s.size();
  std::size_t lo = 0;
  std::size_t hi = s.size();
  while (lo < hi) {
    std::size_t const mid = lo + (hi - lo + 1) / 2;
    if (tok::estimate(s.substr(0, mid)) <= max_tokens)
      lo = mid;
    else
      hi = mid - 1;
  }
  return char_boundary(s, lo);
}

// 删掉该消息里的所有推理块；返回是否有改动。
void strip_reasoning(Message& m) {
  std::erase_if(m.content,
                [](auto const& b) { return std::holds_alternative<ReasoningBlock>(b); });
}

// 该 ai 消息是否"发起过工具调用"（合法或非法都算：非法调用同样有后续 ToolMessage）。
bool has_tool_calls(Message const& m) {
  return !m.tool_calls.empty() || !m.invalid_tool_calls.empty();
}

}  // namespace

std::string_view history_mode_name(HistoryMode mode) {
  switch (mode) {
    case HistoryMode::Full:
      return "full";
    case HistoryMode::DropRound:
      return "drop_round";
    case HistoryMode::KeepEvidence:
      return "keep_evidence";
  }
  return "full";
}

std::optional<HistoryMode> parse_history_mode(std::string_view text) {
  if (text == "full") return HistoryMode::Full;
  if (text == "drop_round") return HistoryMode::DropRound;
  if (text == "keep_evidence") return HistoryMode::KeepEvidence;
  return std::nullopt;
}

nlohmann::json history_stats_json(HistoryStats const& stats) {
  return nlohmann::json{
      {"mode", std::string(history_mode_name(stats.mode))},
      {"budget", stats.budget},
      {"before_msgs", stats.before_msgs},
      {"after_msgs", stats.after_msgs},
      {"before_tokens", stats.before_tokens},
      {"after_tokens", stats.after_tokens},
      {"dropped_msgs", stats.dropped_msgs},
      {"rewritten_msgs", stats.rewritten_msgs},
  };
}

HistoryStats compress_history(std::vector<Message>& msgs, HistoryMode mode,
                             std::size_t budget_tokens, std::string_view source_hint) {
  HistoryStats stats;
  stats.mode = mode;
  stats.budget = budget_tokens;
  stats.before_msgs = msgs.size();
  stats.before_tokens = tokens::estimate_msgs(msgs);

  switch (mode) {
    case HistoryMode::Full:
      break;  // 恒等：一行不改

    case HistoryMode::DropRound: {
      // 丢「ai(tool_calls) + 其后**连续的** tool」整组（同去同留，绝不产生孤儿 ToolMessage）；
      // 其余消息保留，但所有 ai 的 ReasoningBlock 一律删掉。
      std::vector<Message> kept;
      kept.reserve(msgs.size());
      for (std::size_t i = 0; i < msgs.size();) {
        if (msgs[i].type == Role::Ai && has_tool_calls(msgs[i])) {
          ++i;  // 丢掉发起调用的这条 ai
          ++stats.dropped_msgs;
          while (i < msgs.size() && msgs[i].type == Role::Tool) {
            ++i;  // 连同它的工具结果一起丢
            ++stats.dropped_msgs;
          }
          continue;
        }
        Message m = std::move(msgs[i]);
        if (m.type == Role::Ai) strip_reasoning(m);
        kept.push_back(std::move(m));
        ++i;
      }
      msgs = std::move(kept);
      break;
    }

    case HistoryMode::KeepEvidence: {
      // 先建 tool_call_id → (工具名, 参数) 索引：凭据行里的工具名与参数必须是真的，
      // 从发起调用的那条 ai 消息 join 回来（与 run_tools 的 by_id 手法一致）。
      struct CallInfo {
        std::string name;
        std::string args;
      };
      std::unordered_map<std::string, CallInfo> by_id;
      for (Message const& m : msgs) {
        if (m.type != Role::Ai) continue;
        for (auto const& tc : m.tool_calls) by_id.emplace(tc.id, CallInfo{tc.name, tc.args.dump()});
      }

      for (Message& m : msgs) {
        if (m.type == Role::Ai) {
          strip_reasoning(m);
          continue;
        }
        if (m.type != Role::Tool) continue;

        std::string const text = m.text();
        if (text.empty() || tokens::estimate(text) <= budget_tokens)
          continue;  // 小结果即最强证据：逐字保留，不动它

        auto const it = m.tool_call_id ? by_id.find(*m.tool_call_id) : by_id.end();
        std::string const label = (it != by_id.end())
                                      ? (it->second.name + "(" + it->second.args + ")")
                                      : std::string("工具结果");
        std::size_t const keep_bytes = prefix_within_tokens(text, budget_tokens);

        std::string line = label + " → " + text.substr(0, keep_bytes);
        line += "\n…[历史工具结果已压缩：原文 " + std::to_string(tokens::estimate(text)) +
                " tokens / " + std::to_string(text.size()) + " 字节（保留前 " +
                std::to_string(tokens::estimate(text.substr(0, keep_bytes))) + " tokens）；完整内容";
        line += source_hint.empty() ? std::string("仍在会话日志中")
                                    : ("在 " + std::string(source_hint) + " 里");
        line += "，如需细节请重新调用该工具]";

        m.content.clear();
        m.content.push_back(TextBlock{std::move(line), std::nullopt});
        ++stats.rewritten_msgs;
      }
      break;
    }
  }

  stats.after_msgs = msgs.size();
  stats.after_tokens = tokens::estimate_msgs(msgs);
  return stats;
}

}  // namespace cppchain::agent
