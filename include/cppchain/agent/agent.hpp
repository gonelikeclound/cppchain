// cppchain —— agent/agent.hpp
// Agent 循环（M7）：create_agent 等价物 = model + tools + middleware。
// 同时是三个卡位问题的落地层：上下文压缩 / 超长工具返回 / 工具副作用。
#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/tool/tool_node.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::agent {

using cppchain::message::Message;
using cppchain::message::ToolCall;

// ---- 中间件：三个钩子（简版，够落实三个卡位问题）----
class Middleware {
 public:
  virtual ~Middleware() = default;

  // 模型调用前：可改写消息序列（压缩、裁剪在这里发生）
  virtual void before_model(std::vector<Message>& msgs) { (void)msgs; }

  // 工具执行前：返回 false → 拒绝执行；why 会作为 error ToolMessage 回喂（副作用审批）
  virtual bool approve_tool_call(ToolCall const& call, std::string& why) {
    (void)call;
    (void)why;
    return true;
  }

  // 工具执行后：可改写 ToolMessage（超长截断在这里发生）
  virtual void after_tool(Message& tool_message) { (void)tool_message; }
};

// ---- 预置中间件 ----

// 超长工具返回：超过 max_chars 截断，并标注被截断的字节数
class TruncateToolResults final : public Middleware {
 public:
  explicit TruncateToolResults(std::size_t max_chars = 2000) : max_chars_(max_chars) {}
  void after_tool(Message& tool_message) override;

 private:
  std::size_t max_chars_;
};

// 上下文压缩：消息数超过 max_messages 时折叠早期历史。
//   summarizer 为空 → 丢弃旧消息 + 一条占位说明；
//   summarizer 非空 → 用它把旧消息压成摘要（真实压缩，需多一次模型调用）
class TrimHistory final : public Middleware {
 public:
  using Summarizer =
      std::function<std::expected<std::string, model::ModelError>(std::vector<Message> const&)>;

  explicit TrimHistory(std::size_t max_messages = 20) : max_messages_(max_messages) {}
  TrimHistory(std::size_t max_messages, Summarizer summarizer)
      : max_messages_(max_messages), summarizer_(std::move(summarizer)) {}

  void before_model(std::vector<Message>& msgs) override;

  [[nodiscard]] std::size_t compressions() const { return compressions_; }

 private:
  std::size_t max_messages_;
  Summarizer summarizer_;
  std::size_t compressions_ = 0;
};

// 工具副作用：按名单拒绝（可扩展为回调/白名单）
class ToolApproval final : public Middleware {
 public:
  using Decider = std::function<bool(ToolCall const&, std::string& why)>;

  explicit ToolApproval(std::vector<std::string> denied_tools);
  explicit ToolApproval(Decider decider) : decider_(std::move(decider)) {}

  bool approve_tool_call(ToolCall const& call, std::string& why) override;

  [[nodiscard]] std::size_t denials() const { return denials_; }

 private:
  std::vector<std::string> denied_;
  Decider decider_;
  std::size_t denials_ = 0;
};

// ---- Agent ----

struct AgentOptions {
  std::string system_prompt;
  std::size_t max_iterations = 10;
  std::string thread_id;                        // 非空 + checkpointer → 每轮快照
  state::Checkpointer* checkpointer = nullptr;  // 非拥有
};

class Agent {
 public:
  // 生命周期：model 与 tools 都必须**比 Agent 活得久**（内部只存指针）。
  // 切勿传临时对象，例如 Agent(model, tool::ToolRegistry{}, opts) —— 会立即悬垂。
  Agent(model::ChatModel& model, tool::ToolRegistry const& tools, AgentOptions opts = {});

  void add_middleware(std::shared_ptr<Middleware> mw) { middlewares_.push_back(std::move(mw)); }

  // 跑一轮对话：输入 → （可能的多次工具往返）→ 最终 AI 消息
  std::expected<Message, model::ModelError> invoke(std::vector<Message> input);

  [[nodiscard]] std::vector<Message> const& transcript() const { return msgs_; }
  [[nodiscard]] std::size_t iterations() const { return iterations_; }
  [[nodiscard]] std::string const& last_checkpoint_id() const { return last_checkpoint_; }

 private:
  model::ChatModel* model_;
  tool::ToolRegistry const* tools_;
  tool::ToolNode node_;
  AgentOptions opts_;
  std::vector<std::shared_ptr<Middleware>> middlewares_;
  std::vector<Message> msgs_;
  std::size_t iterations_ = 0;
  std::string last_checkpoint_;
};

// 便捷构造（对应 langchain create_agent）
std::unique_ptr<Agent> create_agent(model::ChatModel& model, tool::ToolRegistry const& tools,
                                    AgentOptions opts = {});

}  // namespace cppchain::agent