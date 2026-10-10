// cppchain —— src/agent/history.cpp
#include "cppchain/agent/history.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

#include "cppchain/agent/artifact.hpp"
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

// ---- wfr-㊵：规则引擎的内部件 ----

// 工具轮成组：AI(有 tool_calls/invalid_tool_calls) + 其后**连续**的 Tool 消息算一组。
// 与 TrimHistory 的组识别同一纪律（同去同留，绝不产生孤儿 ToolMessage —— 真端点会 400）。
struct Group {
  std::size_t begin = 0;
  std::size_t end = 0;      // 左闭右开
  bool tool_round = false;  // 是不是"工具轮"（否则是单条消息）
};

std::vector<Group> groups_of(std::vector<Message> const& msgs) {
  std::vector<Group> out;
  for (std::size_t i = 0; i < msgs.size();) {
    if (msgs[i].type == Role::Ai && has_tool_calls(msgs[i])) {
      std::size_t j = i + 1;
      while (j < msgs.size() && msgs[j].type == Role::Tool) ++j;
      out.push_back(Group{i, j, true});
      i = j;
    } else {
      out.push_back(Group{i, i + 1, false});
      ++i;
    }
  }
  return out;
}

// 组内每个 Tool 消息对应的工具名（按 tool_call_id join 回发起它的 AI 消息）。
// 名字查不到（历史里的调用方不在本次 AI 消息上 / id 对不上）→ 返回空串。
std::vector<std::string> group_tool_names(std::vector<Message> const& msgs, Group const& g) {
  std::vector<std::string> names;
  if (!g.tool_round) return names;
  for (std::size_t i = g.begin; i < g.end; ++i) {
    if (msgs[i].type != Role::Tool) continue;
    std::string name;
    if (msgs[i].tool_call_id)
      for (auto const& tc : msgs[g.begin].tool_calls)
        if (tc.id == *msgs[i].tool_call_id) {
          name = tc.name;
          break;
        }
    names.push_back(std::move(name));
  }
  return names;
}

std::string category_of(HistoryRuleParams const& p, std::string const& tool_name) {
  if (tool_name.empty()) return "general";
  auto const it = p.tool_category.find(tool_name);
  return it == p.tool_category.end() ? "general" : it->second;
}

// 截断标注（与 keep_evidence 的凭据行同一风格：说清"被裁了多少、原文在哪"）
std::string clip_annotation(std::size_t orig_tokens, std::size_t orig_bytes,
                            std::string_view source_hint) {
  std::string s = "\n…[内容已裁剪：原文 " + std::to_string(orig_tokens) + " tokens / " +
                  std::to_string(orig_bytes) + " 字节；完整内容";
  s += source_hint.empty() ? std::string("仍在会话日志中") : ("在 " + std::string(source_hint) + " 里");
  return s + "]";
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

// ---- wfr-㊵：规则引擎 ----

bool is_known_history_rule(std::string_view rule) {
  if (rule == "drop_reasoning" || rule == "drop_error_tools" || rule == "drop_tool_results" ||
      rule == "clip_tool_results" || rule == "clip_ai_text")
    return true;
  auto const cat = history_rule_category(rule);
  return cat.has_value() && !cat->empty();
}

std::optional<std::string_view> history_rule_category(std::string_view rule) {
  constexpr std::string_view kPrefix = "drop_category:";
  if (rule.size() <= kPrefix.size() || rule.substr(0, kPrefix.size()) != kPrefix)
    return std::nullopt;
  return rule.substr(kPrefix.size());
}

std::optional<std::string_view> parse_history_preset(std::string_view text) {
  for (std::string_view const p : {"minimal", "lean", "aggressive", "clip"})
    if (text == p) return text;
  return std::nullopt;
}

std::vector<std::string> resolve_history_rules(std::string_view preset,
                                              std::span<std::string const> extra,
                                              std::string_view clip_scope) {
  std::vector<std::string> out;
  auto add = [&out](std::string const& r) {
    if (!r.empty() && std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
  };
  if (preset == "minimal") {
    add("drop_reasoning");
  } else if (preset == "lean") {
    add("drop_reasoning"); add("clip_tool_results"); add("clip_ai_text");
  } else if (preset == "clip") {
    add("clip_tool_results"); add("clip_ai_text");
  } else if (preset == "aggressive") {
    add("drop_reasoning"); add("drop_error_tools"); add("drop_category:retrieval");
    add("clip_tool_results"); add("clip_ai_text"); add("drop_tool_results");
  }
  for (std::string const& r : extra) add(r);

  // clip 门控：只作用于两条 clip 规则 —— 便于切出"只裁 AI 长回答"这类档位
  if (clip_scope == "tool_results")
    out.erase(std::remove(out.begin(), out.end(), "clip_ai_text"), out.end());
  else if (clip_scope == "ai_text")
    out.erase(std::remove(out.begin(), out.end(), "clip_tool_results"), out.end());

  // 固定优先级（与文档的规则表一致）：规则在 JSON 里的书写顺序不生效 → 计划确定、可测
  auto rank = [](std::string const& r) -> int {
    if (r == "drop_reasoning") return 0;
    if (r == "drop_error_tools") return 1;
    if (r.rfind("drop_category:", 0) == 0) return 2;
    if (r == "drop_tool_results") return 3;
    if (r == "clip_tool_results") return 4;
    if (r == "clip_ai_text") return 5;
    return 99;
  };
  std::stable_sort(out.begin(), out.end(),
                   [&rank](std::string const& a, std::string const& b) {
                     return rank(a) < rank(b);
                   });
  return out;
}

HistoryRulePlan plan_history_rules(std::vector<Message> const& msgs,
                                   std::span<std::string const> rules,
                                   HistoryRuleParams const& params,
                                   std::string_view source_hint) {
  HistoryRulePlan plan;
  HistoryRuleStats& st = plan.stats;
  st.before_msgs = msgs.size();
  st.before_tokens = tokens::estimate_msgs(msgs);

  auto has = [&rules](std::string_view r) {
    return std::find(rules.begin(), rules.end(), r) != rules.end();
  };
  std::vector<std::string> cats;
  for (std::string const& r : rules)
    if (auto const c = history_rule_category(r)) cats.emplace_back(*c);

  auto const groups = groups_of(msgs);
  std::vector<bool> dropped(msgs.size(), false);

  // ② 整组删（错误工具轮 / 指定类别的工具轮）—— AI 与它的 Tool 结果**同去**，
  //    只删 Tool 会留下 dangling tool_calls（真端点直接 400）
  if (has("drop_error_tools") || !cats.empty()) {
    for (Group const& g : groups) {
      if (!g.tool_round || g.end - g.begin < 2) continue;  // 没有工具结果 → 不动
      bool all_error = true;
      for (std::size_t i = g.begin + 1; i < g.end; ++i)
        if (msgs[i].status != "error") all_error = false;
      bool cat_match = false;
      if (!cats.empty()) {
        auto const names = group_tool_names(msgs, g);
        cat_match = !names.empty();
        for (std::string const& n : names)
          if (std::find(cats.begin(), cats.end(), category_of(params, n)) == cats.end())
            cat_match = false;   // 混类别 → 整组保留
      }
      bool const hit = (all_error && has("drop_error_tools")) || cat_match;
      if (!hit) continue;
      for (std::size_t i = g.begin; i < g.end; ++i) dropped[i] = true;
      ++st.dropped_groups;
      if (all_error && has("drop_error_tools")) ++st.dropped_error_groups;
      if (cat_match) ++st.dropped_category_groups;
    }
  }

  // ④ 孤儿 Tool（安全网：上游数据坏了也别把孤儿送给端点）
  if (has("drop_tool_results")) {
    for (std::size_t i = 0; i < msgs.size(); ++i) {
      if (dropped[i] || msgs[i].type != Role::Tool || !msgs[i].tool_call_id) continue;
      bool found = false;
      for (std::size_t j = 0; j < i && !found; ++j)
        for (auto const& tc : msgs[j].tool_calls)
          if (tc.id == *msgs[i].tool_call_id) found = true;
      if (!found) {
        dropped[i] = true;
        ++st.dropped_orphan_tools;
      }
    }
  }

  // ① 去思考过程（改而不是删：只剥 ReasoningBlock，tool_calls 与正文不动）
  if (has("drop_reasoning")) {
    for (std::size_t i = 0; i < msgs.size(); ++i) {
      if (dropped[i] || msgs[i].type != Role::Ai) continue;
      bool const any = std::any_of(msgs[i].content.begin(), msgs[i].content.end(),
                                   [](auto const& b) {
                                     return std::holds_alternative<ReasoningBlock>(b);
                                   });
      if (!any) continue;
      Message m = msgs[i];
      strip_reasoning(m);
      plan.rewrite.emplace_back(i, std::move(m));
      ++st.stripped_reasoning;
    }
  }

  // ⑤ 裁工具结果（超预算 → 截断，或落盘成引用行）
  if (has("clip_tool_results")) {
    std::size_t idx = 0;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
      if (dropped[i] || msgs[i].type != Role::Tool) continue;
      std::string const text = msgs[i].text();
      std::size_t const orig_tokens = tokens::estimate(text);
      if (text.empty() || orig_tokens <= params.tool_budget) continue;
      Message m = msgs[i];
      if (params.artifact_mode && !params.artifact_dir.empty()) {
        if (write_tool_artifact(m, params.artifact_dir, params.tool_budget, idx + 1,
                                params.artifact_display_root))
          ++st.artifacts_written;
        ++idx;
      } else {
        std::size_t const keep = prefix_within_tokens(text, params.tool_budget);
        m.content.clear();
        m.content.push_back(TextBlock{
            text.substr(0, keep) + clip_annotation(orig_tokens, text.size(), source_hint),
            std::nullopt});
      }
      plan.rewrite.emplace_back(i, std::move(m));
      ++st.rewritten_tools;
    }
  }

  // ⑥ 裁**终态** AI 回答。带 tool_calls 的是"过渡回答"：它的长来自 reasoning 与 args，
  //    正文通常极短，裁它无益、且绝不能碰 tool_calls（会断工具组）→ 直接跳过。
  if (has("clip_ai_text")) {
    for (std::size_t i = 0; i < msgs.size(); ++i) {
      if (dropped[i] || msgs[i].type != Role::Ai || has_tool_calls(msgs[i])) continue;
      std::string const text = msgs[i].text();
      std::size_t const orig_tokens = tokens::estimate(text);
      if (text.empty() || orig_tokens <= params.ai_budget) continue;
      Message m = msgs[i];
      std::vector<message::ContentBlock> kept;
      for (auto const& b : m.content)
        if (!std::holds_alternative<TextBlock>(b)) kept.push_back(b);  // ReasoningBlock 归 ①
      std::size_t const keep = prefix_within_tokens(text, params.ai_budget);
      kept.push_back(TextBlock{
          text.substr(0, keep) + clip_annotation(orig_tokens, text.size(), source_hint),
          std::nullopt});
      m.content = std::move(kept);
      plan.rewrite.emplace_back(i, std::move(m));
      ++st.rewritten_ai;
    }
  }

  for (std::size_t i = 0; i < msgs.size(); ++i)
    if (dropped[i]) plan.drop.push_back(i);
  st.dropped_msgs = plan.drop.size();
  std::sort(plan.rewrite.begin(), plan.rewrite.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });

  // after 口径：把计划应用到一个副本上算（与节点固化成 op 后的效果一致）
  std::vector<Message> after;
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    if (dropped[i]) continue;
    auto const it = std::find_if(plan.rewrite.begin(), plan.rewrite.end(),
                                 [i](auto const& p) { return p.first == i; });
    if (it != plan.rewrite.end()) {
      Message m = it->second;
      // 被重写的消息内容已变，原 usage 是**旧调用的实测值**，与 after 状态不符：
      // 丢弃它，让 estimate_msgs 按内容启发式重估（否则 after_tokens 反映不出裁剪）。
      m.usage.reset();
      after.push_back(std::move(m));
    } else {
      after.push_back(msgs[i]);
    }
  }
  st.after_msgs = after.size();
  st.after_tokens = tokens::estimate_msgs(after);
  return plan;
}

nlohmann::json history_rule_stats_json(HistoryRuleStats const& st) {
  return nlohmann::json{
      {"before_msgs", st.before_msgs},   {"after_msgs", st.after_msgs},
      {"before_tokens", st.before_tokens}, {"after_tokens", st.after_tokens},
      {"dropped_msgs", st.dropped_msgs}, {"dropped_groups", st.dropped_groups},
      {"dropped_error_groups", st.dropped_error_groups},
      {"dropped_category_groups", st.dropped_category_groups},
      {"dropped_orphan_tools", st.dropped_orphan_tools},
      {"stripped_reasoning", st.stripped_reasoning},
      {"rewritten_tools", st.rewritten_tools}, {"rewritten_ai", st.rewritten_ai},
      {"artifacts_written", st.artifacts_written}};
}

}  // namespace cppchain::agent
