// cppchain —— agent/journal.hpp
// 副作用执行日志（M7 收口补隐患）：防止"已执行的副作用被重放"。
//
// 为什么需要：M6 的恢复语义是"回到该步边界**重跑**"，M7 的循环与任何重试
// 都可能让同一个副作用执行两次（重复发邮件/扣款/删除）。幂等键就是 tool_call_id。
//
// 三态判定（安全默认）：
//   · 未见过           → 正常执行
//   · 已完成 Completed → **重放记录的结果**，不重新执行
//   · 只有意图 Intent  → 执行到一半就崩了，**结果未知** → 拒绝重跑并回喂说明
//   · 失败 Failed      → 默认也拒绝重跑（副作用可能已部分生效）；可配置为允许重试
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

#include "cppchain/agent/agent.hpp"

namespace cppchain::agent {

struct JournalEntry {
  enum class State { Intent, Completed, Failed };

  std::string key;        // 幂等键
  std::string tool_name;
  nlohmann::json args = nlohmann::json::object();
  State state = State::Intent;
  std::string content;    // 完成后记录的 ToolMessage 内容
  std::string status;     // "success" / "error"
};

nlohmann::json to_json(JournalEntry const& e);
JournalEntry entry_from_json(nlohmann::json const& j);

class ToolExecutionJournal {
 public:
  virtual ~ToolExecutionJournal() = default;

  [[nodiscard]] virtual std::optional<JournalEntry> find(std::string const& key) const = 0;
  virtual void record_intent(JournalEntry entry) = 0;
  virtual void record_result(std::string const& key, std::string content, std::string status) = 0;

  // 持久化（跨进程重启后仍能防重放）
  [[nodiscard]] virtual nlohmann::json dump() const = 0;
  virtual void load(nlohmann::json const& j) = 0;
};

class MemoryJournal final : public ToolExecutionJournal {
 public:
  [[nodiscard]] std::optional<JournalEntry> find(std::string const& key) const override;
  void record_intent(JournalEntry entry) override;
  void record_result(std::string const& key, std::string content, std::string status) override;

  [[nodiscard]] nlohmann::json dump() const override;
  void load(nlohmann::json const& j) override;

  [[nodiscard]] std::size_t size() const { return entries_.size(); }

 private:
  std::unordered_map<std::string, JournalEntry> entries_;
};

// 幂等键的取法
enum class JournalKeyMode {
  ToolCallId,       // 默认：同一次调用（模型生成的 id 稳定）——语义最明确
  ToolNameAndArgs,  // 更强：能抓住"模型重新规划后生成新 id 但参数相同"的重复
                    //   ⚠️ 代价：会误杀"合法的重复调用"（例如用户真的想查两次天气）
};

// 中间件：在工具执行前查日志、执行后写日志
class SideEffectGuard final : public Middleware {
 public:
  struct Options {
    JournalKeyMode key_mode = JournalKeyMode::ToolCallId;
    bool retry_on_failure = false;  // 失败记录是否允许重跑（默认不允许：副作用可能已部分生效）
  };

  explicit SideEffectGuard(ToolExecutionJournal& journal) : journal_(&journal) {}
  SideEffectGuard(ToolExecutionJournal& journal, Options opts)
      : journal_(&journal), opts_(opts) {}

  // 执行前：命中日志则返回替代结果（不执行）；否则登记"意图"
  std::optional<message::Message> lookup_tool_result(message::ToolCall const& call,
                                                     tool::ToolSpec const* spec) override;

  // 执行后：记录结果
  void after_tool(message::Message& tool_message) override;

  [[nodiscard]] std::size_t replays() const { return replays_; }
  [[nodiscard]] std::size_t blocked() const { return blocked_; }
  [[nodiscard]] std::size_t executions() const { return executions_; }

  [[nodiscard]] static std::string key_for(message::ToolCall const& call, JournalKeyMode mode);

 private:
  ToolExecutionJournal* journal_;
  Options opts_;
  std::unordered_map<std::string, std::string> pending_key_;  // tool_call_id → 幂等键
  std::size_t replays_ = 0;
  std::size_t blocked_ = 0;
  std::size_t executions_ = 0;
};

}  // namespace cppchain::agent