// cppchain —— agent/agent.hpp
// Agent 循环（M7）：create_agent 等价物 = model + tools + middleware。
// 同时是三个卡位问题的落地层：上下文压缩 / 超长工具返回 / 工具副作用。
#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
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

  // 工具执行**前**：返回有值则用它替代真实执行（副作用去重/重放）；
  // spec 为工具规格（可能为 null，表示未知工具），可用于跳过只读工具
  virtual std::optional<Message> lookup_tool_result(ToolCall const& call,
                                                    tool::ToolSpec const* spec) {
    (void)call;
    (void)spec;
    return std::nullopt;
  }

  // 工具执行后：可改写 ToolMessage（超长截断在这里发生）
  virtual void after_tool(Message& tool_message) { (void)tool_message; }
};

// ---- 预置中间件 ----

// 超长工具返回：超过上限截断，并标注被截断量。
//   max_chars  → 字节上限（原语义，缺省行为）；
//   max_tokens → 设置后改用 token 预算截断（M9，见 02-arch-design/10 A2②）
class TruncateToolResults final : public Middleware {
 public:
  struct Options {
    std::size_t max_chars = 2000;
    std::optional<std::size_t> max_tokens;  // 设置了 → token 预算模式（二分回退字符边界）
  };

  explicit TruncateToolResults(Options opts) : opts_(std::move(opts)) {}
  explicit TruncateToolResults(std::size_t max_chars) { opts_.max_chars = max_chars; }

  void after_tool(Message& tool_message) override;

 private:
  Options opts_;
};

// 上下文压缩：消息数超过 max_messages 时折叠早期历史。
//   summarizer 为空 → 丢弃旧消息 + 一条占位说明；
//   summarizer 非空 → 用它把旧消息压成摘要（真实压缩，需多一次模型调用）
//
// 两个关键保障（见 04-problems/04 与 06 的"压缩 × 时效"交叉）：
//   ① **边界安全**：折叠边界不得切断工具调用组（AI(tool_calls) 必须与它的 ToolMessage 同去同留），
//      否则会留下孤儿 ToolMessage → 真实端点直接 400。
//   ② **时效策略**：带时效元数据的易变结果（见 agent/freshness.hpp）单独处置：
//        Fold     —— 与普通消息一起折叠（默认；**时效信息会丢失**，失效变得不可见）
//        Preserve —— 整组保留原文 → StaleStateGuard 仍能识别过期并提示重取
//        Drop     —— 整组替换为占位说明 → 迫使模型重新获取
class TrimHistory final : public Middleware {
 public:
  using Summarizer =
      std::function<std::expected<std::string, model::ModelError>(std::vector<Message> const&)>;

  enum class VolatilePolicy { Fold, Preserve, Drop };

  struct Options {
    std::size_t max_messages = 20;
    // M9：token 占用触发（估算，见 tokens.hpp）。设了且估算超限 → 即使条数未超也压缩。
    std::optional<std::size_t> max_tokens;
    Summarizer summarizer;
    VolatilePolicy volatile_policy = VolatilePolicy::Fold;
  };

  explicit TrimHistory(Options opts) : opts_(std::move(opts)) {}
  explicit TrimHistory(std::size_t max_messages) { opts_.max_messages = max_messages; }
  TrimHistory(std::size_t max_messages, Summarizer summarizer) {
    opts_.max_messages = max_messages;
    opts_.summarizer = std::move(summarizer);
  }

  void before_model(std::vector<Message>& msgs) override;

  [[nodiscard]] std::size_t compressions() const { return compressions_; }
  [[nodiscard]] std::size_t preserved_groups() const { return preserved_groups_; }
  [[nodiscard]] std::size_t dropped_groups() const { return dropped_groups_; }
  // M9：LLM 摘要次数 / 累计估算省下的 token（= 被折叠区间 - 摘要文本）
  [[nodiscard]] std::size_t summaries() const { return summaries_; }
  [[nodiscard]] std::size_t estimated_tokens_saved() const { return tokens_saved_; }

  // 折叠边界的安全校正（public static：便于单测直接验证）
  [[nodiscard]] static std::size_t safe_boundary(std::vector<Message> const& msgs,
                                                 std::size_t keep_from, std::size_t desired);

 private:
  Options opts_;
  std::size_t compressions_ = 0;
  std::size_t preserved_groups_ = 0;
  std::size_t dropped_groups_ = 0;
  std::size_t summaries_ = 0;
  std::size_t tokens_saved_ = 0;
};

// 工具副作用：按名单/回调拒绝（M9 追加审批记忆化 + 只读免审，见 06 详细设计 §2.4）
class ToolApproval final : public Middleware {
 public:
  using Decider = std::function<bool(ToolCall const&, std::string& why)>;

  enum class MemoPolicy { Off, RememberDeny, RememberAll };

  struct Options {
    MemoPolicy memo = MemoPolicy::RememberDeny;  // 缺省=只记忆"拒绝"（放行每次仍审，副作用敞口）
    std::size_t max_entries = 128;               // FIFO 有界
    tool::ToolRegistry const* registry = nullptr;  // 非拥有；给定时按 read_only 免审
  };

  // 便捷重载：缺省 Options（默认 RememberDeny / 有界 / 无 registry）。
  // 注意：GCC 不允许"带 DMI 的嵌套聚合"作默认参数（会报 enclosing class 错误），
  // 故用委托构造 + Options{} 于 mem-init 列表。
  explicit ToolApproval(std::vector<std::string> denied_tools)
      : ToolApproval(std::move(denied_tools), Options{}) {}
  explicit ToolApproval(std::vector<std::string> denied_tools, Options opts);
  explicit ToolApproval(Decider decider) : ToolApproval(std::move(decider), Options{}) {}
  explicit ToolApproval(Decider decider, Options opts)
      : decider_(std::move(decider)), opts_(std::move(opts)) {}

  bool approve_tool_call(ToolCall const& call, std::string& why) override;

  [[nodiscard]] std::size_t denials() const { return denials_; }
  // M9：记忆命中的次数（证明"不重复问"路径真的被走到）
  [[nodiscard]] std::size_t memo_hits() const { return memo_hits_; }
  [[nodiscard]] std::size_t memo_size() const { return memo_.size(); }

 private:
  std::vector<std::string> denied_;
  Decider decider_;
  Options opts_;
  std::size_t denials_ = 0;
  std::size_t memo_hits_ = 0;
  // fingerprint → 已决结论（true=放行）。FIFO：满则逐出最旧。
  std::vector<std::pair<std::string, bool>> memo_;
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