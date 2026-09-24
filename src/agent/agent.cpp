// cppchain —— src/agent/agent.cpp
#include "cppchain/agent/agent.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "cppchain/message/chunk.hpp"
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
  if (text.size() <= max_chars_) return;

  std::string const kept = truncate_utf8(text, max_chars_);
  std::size_t const dropped = text.size() - kept.size();
  tool_message.content.clear();
  tool_message.content.push_back(
      TextBlock{kept + "\n…[已截断 " + std::to_string(dropped) +
                    " 字节；如需完整内容请缩小查询范围]",
                std::nullopt});
}

// ---------------- TrimHistory ----------------

void TrimHistory::before_model(std::vector<Message>& msgs) {
  if (msgs.size() <= max_messages_) return;

  // 保留系统提示 + 最近 max_messages_ 条
  std::vector<Message> head;
  std::size_t keep_from = 0;
  if (!msgs.empty() && msgs.front().type == Role::System) {
    head.push_back(msgs.front());
    keep_from = 1;
  }
  std::size_t const drop_count = msgs.size() - max_messages_;
  if (drop_count <= keep_from) return;

  std::vector<Message> dropped(msgs.begin() + static_cast<std::ptrdiff_t>(keep_from),
                               msgs.begin() + static_cast<std::ptrdiff_t>(drop_count));

  std::string summary_text;
  if (summarizer_) {
    auto s = summarizer_(dropped);
    summary_text = s ? *s : ("（摘要失败：" + s.error().message + "）");
  } else {
    summary_text = "（此前 " + std::to_string(dropped.size()) + " 条消息因超长已省略）";
  }

  std::vector<Message> rebuilt = std::move(head);
  rebuilt.push_back(message::system_message("[历史摘要] " + summary_text));
  rebuilt.insert(rebuilt.end(), msgs.begin() + static_cast<std::ptrdiff_t>(drop_count),
                 msgs.end());

  msgs = std::move(rebuilt);
  ++compressions_;
}

// ---------------- ToolApproval ----------------

ToolApproval::ToolApproval(std::vector<std::string> denied_tools)
    : denied_(std::move(denied_tools)) {}

bool ToolApproval::approve_tool_call(ToolCall const& call, std::string& why) {
  bool allowed = true;
  if (decider_) {
    allowed = decider_(call, why);
  } else {
    allowed = std::find(denied_.begin(), denied_.end(), call.name) == denied_.end();
    if (!allowed) why = "工具 '" + call.name + "' 被策略禁止（副作用审批）";
  }
  if (!allowed) ++denials_;
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
                                                   std::move(values), std::vector<std::string>{},
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
                                                 std::move(values), std::vector<std::string>{"model"},
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