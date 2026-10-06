// cppchain —— src/agent/agent.cpp
#include "cppchain/agent/agent.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "cppchain/message/chunk.hpp"
#include "cppchain/agent/freshness.hpp"
#include "cppchain/agent/tokens.hpp"
#include "cppchain/message/serializer.hpp"   // Message ↔ JSON（快照需要）
#include "cppchain/state/state.hpp"

namespace cppchain::agent {

using Json = state::Json;
using cppchain::message::ReasoningBlock;
using cppchain::message::Role;
using cppchain::message::TextBlock;

// ---------------- TruncateToolResults ----------------

namespace {

// 按字节上限截断，但**不切断 UTF-8 字符**：
// 直接按字节切会把中文切开 → 非法 UTF-8 → 序列化成 JSON 时抛 type_error.316。
std::string truncate_utf8(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;  // 回退到字符边界
  return s.substr(0, end);
}

}  // namespace

void TruncateToolResults::after_tool(Message& tool_message) {
  std::string const text = tool_message.text();
  if (text.empty()) return;

  if (opts_.max_tokens) {  // M9：token 预算模式（标注本身也算 token，用有效预算线性扫描）
    namespace tok = cppchain::agent::tokens;
    std::size_t const budget = *opts_.max_tokens;
    if (tok::estimate(text) <= budget) return;

    // 标注文案（含真实数字；kept 变化仅影响末几位数字，先以 kept=0 估算其固定的成本）
    auto annotation = [&](std::size_t kept) {
      return std::string("\n…[已按 token 预算截断：原文约 ") + std::to_string(tok::estimate(text)) +
             " tokens，保留前 " + std::to_string(tok::estimate(text.substr(0, kept))) +
             " tokens（" + std::to_string(text.size()) + "→" + std::to_string(kept) + " 字节）]";
    };

    // 与 estimate() 同口径增量累计：wide(多字节字符)+ceil(ascii/4)，天然停在字符边界。
    // 有效预算 = 总预算 - 标注成本。
    std::size_t const ann_cost = tok::estimate(annotation(0));
    std::size_t const eff = budget > ann_cost ? budget - ann_cost : 0;
    std::size_t kept = 0, wide = 0, ascii = 0;
    for (std::size_t i = 0; i < text.size();) {
      unsigned char const b = static_cast<unsigned char>(text[i]);
      if (b < 0x80) { ++ascii; ++i; }
      else if (b < 0xC0) { ++i; continue; }
      else {
        ++wide; ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
      }
      if (wide + (ascii + 3) / 4 <= eff) kept = i;  // 刚消耗完一个完整字符 → i 是字符边界
      else break;
    }

    // 校正循环：标注用真实数字后若仍超预算，逐字符回退（ann_cost 用 kept=0，数字变大只差 1~2 token）
    auto prev_char_start = [&](std::size_t p) {
      std::size_t q = p - 1;
      while (q > 0 && (static_cast<unsigned char>(text[q]) & 0xC0) == 0x80) --q;
      return q;
    };
    while (kept > 0 && tok::estimate(text.substr(0, kept) + annotation(kept)) > budget)
      kept = prev_char_start(kept);

    std::string const kept_text = text.substr(0, kept);
    tool_message.content.clear();
    tool_message.content.push_back(TextBlock{kept_text + annotation(kept), std::nullopt});
    return;
  }

  // 字节模式（原语义，回归路径）
  if (text.size() <= opts_.max_chars) return;
  std::string const kept = truncate_utf8(text, opts_.max_chars);
  std::size_t const dropped = text.size() - kept.size();
  tool_message.content.clear();
  tool_message.content.push_back(
      TextBlock{kept + "\n…[已截断 " + std::to_string(dropped) +
                    " 字节；如需完整内容请缩小查询范围]",
                std::nullopt});
}

// ---------------- TrimHistory ----------------

std::size_t TrimHistory::safe_boundary(std::vector<Message> const& msgs, std::size_t keep_from,
                                        std::size_t desired) {
  // 折叠边界必须落在"安全点"：保留区不能以孤儿 ToolMessage 开头，
  // 也不能出现"ToolMessage 在保留区、其 AI(tool_calls) 被折掉"的撕裂。
  //
  // 边界等于末尾（全部折叠，如 max_messages=0）时没有保留区，也就不存在撕裂 ——
  // 直接返回；且 msgs[b] 会越界，必须先挡住。
  if (desired >= msgs.size()) return msgs.size();
  std::size_t b = desired;
  while (b > keep_from) {
    if (msgs[b].type == Role::Tool) {  // 边界正好落在工具结果上 → 左移，把它的上下文也留下
      --b;
      continue;
    }
    bool split = false;
    for (std::size_t i = b; i < msgs.size() && !split; ++i) {
      if (msgs[i].type != Role::Tool || !msgs[i].tool_call_id) continue;
      for (std::size_t j = keep_from; j < b && !split; ++j)
        for (auto const& tc : msgs[j].tool_calls)
          if (tc.id == *msgs[i].tool_call_id) split = true;
    }
    if (!split) break;
    --b;
  }
  return b;
}

void TrimHistory::before_model(std::vector<Message>& msgs) {
  namespace tok = cppchain::agent::tokens;
  bool const over_token =
      opts_.max_tokens.has_value() && tok::estimate_msgs(msgs) > *opts_.max_tokens;
  if (msgs.size() <= opts_.max_messages && !over_token) return;

  // 保留系统提示
  std::vector<Message> head;
  std::size_t keep_from = 0;
  if (!msgs.empty() && msgs.front().type == Role::System) {
    head.push_back(msgs.front());
    keep_from = 1;
  }

  std::size_t drop_count = msgs.size() > opts_.max_messages ? msgs.size() - opts_.max_messages : 0;

  // M9：token 触发 → 加大折叠量，直到存活区（head + 尾部）估算不超限；保底留一条非系统消息。
  if (over_token) {
    std::size_t const budget = *opts_.max_tokens;
    std::size_t const head_tokens = tok::estimate_msgs(head);
    while (drop_count < msgs.size() - 1 &&
           head_tokens +
                   tok::estimate_msgs(std::span<Message const>(msgs).subspan(drop_count)) >
               budget)
      ++drop_count;
  }

  // ① 边界安全校正
  drop_count = safe_boundary(msgs, keep_from, drop_count);
  if (drop_count <= keep_from) return;

  // ② 扫描被折叠区间，把"工具调用组"（AI(tool_calls) + 随后的 ToolMessages）识别出来
  struct Group {
    std::size_t begin;
    std::size_t end;  // 左闭右开
    bool volatile_state;  // 含带时效元数据的工具结果
  };
  std::vector<Group> groups;
  std::vector<std::size_t> folded_indices;  // 需要交给摘要器的消息下标

  for (std::size_t i = keep_from; i < drop_count;) {
    if (msgs[i].type == Role::Ai && !msgs[i].tool_calls.empty()) {
      std::size_t j = i + 1;
      bool vol = false;
      while (j < drop_count && msgs[j].type == Role::Tool) {
        if (freshness::has_metadata(msgs[j])) vol = true;
        ++j;
      }
      groups.push_back(Group{i, j, vol});
      i = j;
    } else {
      groups.push_back(Group{i, i + 1, false});
      ++i;
    }
  }

  // ③ 按时效策略分流
  std::vector<Message> preserved;
  std::vector<Message> to_summarize;
  std::size_t dropped = 0;
  for (auto const& g : groups) {
    if (g.volatile_state && opts_.volatile_policy == VolatilePolicy::Preserve) {
      for (std::size_t i = g.begin; i < g.end; ++i) preserved.push_back(msgs[i]);
      ++preserved_groups_;
      continue;
    }
    if (g.volatile_state && opts_.volatile_policy == VolatilePolicy::Drop) {
      ++dropped;
      ++dropped_groups_;
      continue;  // 整组丢弃（连同 AI 消息 → 不留孤儿 ToolMessage）
    }
    for (std::size_t i = g.begin; i < g.end; ++i) to_summarize.push_back(msgs[i]);
  }

  // ④ 生成摘要
  std::string summary_text;
  if (opts_.summarizer && !to_summarize.empty()) {
    auto s = opts_.summarizer(to_summarize);
    summary_text = s ? *s : ("（摘要失败：" + s.error().message + "）");
    if (s) ++summaries_;  // M9：记录了真实的 LLM 摘要次数（成本观测）
  } else {
    summary_text = "（此前 " + std::to_string(to_summarize.size()) + " 条消息因超长已省略）";
  }
  if (dropped > 0)
    summary_text += "；另有 " + std::to_string(dropped) +
                    " 次易变状态的查询结果因时效策略被丢弃，**需要时请重新查询**";

  // ⑤ 重建：head → 保留的易变组 → 摘要 → 保留区
  std::vector<Message> rebuilt = std::move(head);
  rebuilt.insert(rebuilt.end(), preserved.begin(), preserved.end());
  rebuilt.push_back(message::system_message("[历史摘要] " + summary_text));
  rebuilt.insert(rebuilt.end(), msgs.begin() + static_cast<std::ptrdiff_t>(drop_count), msgs.end());

  // M9：估算省下的 token = 被折叠区间 - 摘要文本（供横向表"成本 vs 收益"列）
  namespace tok = cppchain::agent::tokens;
  long long const saved =
      static_cast<long long>(tok::estimate_msgs(to_summarize)) -
      static_cast<long long>(tok::estimate(summary_text));
  if (saved > 0) tokens_saved_ += static_cast<std::size_t>(saved);

  msgs = std::move(rebuilt);
  ++compressions_;
}

// ---------------- ToolApproval ----------------

namespace {

// 参数指纹：递归按键排序 → 稳定 dump，保证 {"a":1,"b":2} 与 {"b":2,"a":1} 同一指纹
// （nlohmann 对象键无序，直接 dump 不稳定 → 会误伤"同一参数不同写法"的记忆化命中）。
std::string canonical_json(nlohmann::json const& v) {
  if (!v.is_object()) return v.is_string() ? v.get<std::string>() : v.dump();
  std::vector<std::string> keys;
  keys.reserve(v.size());
  for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(it.key());
  std::sort(keys.begin(), keys.end());
  std::string out{'{'};
  bool first = true;
  for (auto const& k : keys) {
    if (!first) out += ',';
    first = false;
    out += k;
    out += ':';
    out += canonical_json(v.at(k));
  }
  out += '}';
  return out;
}

}  // namespace

ToolApproval::ToolApproval(std::vector<std::string> denied_tools, Options opts)
    : denied_(std::move(denied_tools)), opts_(std::move(opts)) {}

bool ToolApproval::approve_tool_call(ToolCall const& call, std::string& why) {
  // M9：收到 registry 的只读工具 → 免审（缺省 registry==nullptr → 行为与旧版一致）
  if (opts_.registry) {
    if (auto const* spec = opts_.registry->find(call.name); spec != nullptr && spec->read_only)
      return true;
  }

  std::string const fp = call.name + "|" + canonical_json(call.args);
  auto const cached = std::find_if(memo_.begin(), memo_.end(),
                                   [&](auto const& e) { return e.first == fp; });
  if (cached != memo_.end()) {
    ++memo_hits_;
    if (!cached->second) why = "工具 '" + call.name + "' 本会话已被拒绝（记忆化，不再重复询问）";
    return cached->second;
  }

  bool allowed = true;
  if (decider_) {
    allowed = decider_(call, why);
  } else {
    allowed = std::find(denied_.begin(), denied_.end(), call.name) == denied_.end();
    if (!allowed) why = "工具 '" + call.name + "' 被策略禁止（副作用审批）";
  }
  if (!allowed) ++denials_;

  // 有界记忆：拒绝永远记（RememberDeny/All 都记）；放行仅 RememberAll 记（放行是副作用敞口，默认每次仍审）
  bool const remember = (!allowed && opts_.memo != MemoPolicy::Off) ||
                        (allowed && opts_.memo == MemoPolicy::RememberAll);
  if (remember) {
    if (memo_.size() >= opts_.max_entries) memo_.erase(memo_.begin());  // FIFO 逐出最旧
    memo_.emplace_back(fp, allowed);
  }
  return allowed;
}

// ---------------- Agent ----------------

Agent::Agent(model::ChatModel& model, tool::ToolRegistry const& tools, AgentOptions opts)
    : model_(&model), tools_(&tools), node_(&tools), opts_(std::move(opts)) {}

std::expected<Message, model::ModelError> Agent::invoke(std::vector<Message> input) {
  iterations_ = 0;
  last_checkpoint_.clear();
  msgs_.clear();

  if (!opts_.system_prompt.empty())
    msgs_.push_back(message::system_message(opts_.system_prompt));

  // 有 thread 且有快照 → 从快照恢复（续跑同一会话）
  if (opts_.checkpointer != nullptr && !opts_.thread_id.empty()) {
    if (auto snap = opts_.checkpointer->get_tuple(opts_.thread_id, "", std::nullopt);
        snap.has_value()) {
      auto parsed = state::StateSchema::messages_state().apply(Json::object(), snap->values);
      if (parsed.contains("messages")) {
        msgs_.clear();
        for (auto const& raw : parsed["messages"]) msgs_.push_back(raw.get<Message>());
      }
    }
  }
  msgs_.insert(msgs_.end(), input.begin(), input.end());

  for (std::size_t iter = 0; iter < opts_.max_iterations; ++iter) {
    iterations_ = iter + 1;

    // 1) 模型调用前：中间件可改写消息（压缩/裁剪）
    for (auto const& mw : middlewares_) mw->before_model(msgs_);

    // 2) 调模型（带工具）
    model::CallOptions call_opts;
    call_opts.tools = tools_->to_openai_tools();
    auto reply = model_->invoke(msgs_, call_opts);
    if (!reply) return std::unexpected(reply.error());

    msgs_.push_back(*reply);

    // 3) 无工具调用 → 结束（先把终态落一次快照，否则最后一条 AI 回复不会进历史）
    if (reply->tool_calls.empty()) {
      if (opts_.checkpointer != nullptr && !opts_.thread_id.empty()) {
        Json values = {{"messages", msgs_}};
        last_checkpoint_ = opts_.checkpointer->put(opts_.thread_id, "", std::nullopt,
                                                   std::move(values), std::vector<state::Packet>{},
                                                   Json{{"step", iter}, {"phase", "final"}});
      }
      return *reply;
    }

    // 4) 执行工具（含审批与结果后处理）
    std::vector<Message> tool_msgs;
    for (auto const& call : reply->tool_calls) {
      bool approved = true;
      std::string why;
      for (auto const& mw : middlewares_) {
        if (!mw->approve_tool_call(call, why)) {
          approved = false;
          break;
        }
      }

      Message result;
      if (!approved) {
        result = message::tool_message("rejected: " + why, call.id, "error");
      } else {
        // 执行前：中间件可返回缓存结果（副作用去重/重放），命中则不执行
        std::optional<Message> cached;
        for (auto const& mw : middlewares_) {
          if (auto r = mw->lookup_tool_result(call, tools_->find(call.name))) {
            cached = std::move(r);
            break;
          }
        }
        if (cached) {
          result = std::move(*cached);
        } else {
          auto executed = node_.run(std::vector<ToolCall>{call});
          result = executed.empty() ? message::tool_message("no result", call.id, "error")
                                    : std::move(executed.front());
        }
      }
      for (auto const& mw : middlewares_) mw->after_tool(result);
      msgs_.push_back(result);
      tool_msgs.push_back(std::move(result));
    }

    // 5) 落快照（每轮一次）
    if (opts_.checkpointer != nullptr && !opts_.thread_id.empty()) {
      Json values = {{"messages", msgs_}};
      last_checkpoint_ = opts_.checkpointer->put(opts_.thread_id, "", std::nullopt,
                                                 std::move(values),
                                                 std::vector<state::Packet>{{"model"}},
                                                 Json{{"step", iter}, {"phase", "after_tools"}});
    }
  }

  return std::unexpected(
      model::ModelError{model::ModelError::Kind::Unknown, 0,
                        "agent exceeded max_iterations=" + std::to_string(opts_.max_iterations), {}});
}

std::unique_ptr<Agent> create_agent(model::ChatModel& model, tool::ToolRegistry const& tools,
                                    AgentOptions opts) {
  return std::make_unique<Agent>(model, tools, std::move(opts));
}

}  // namespace cppchain::agent