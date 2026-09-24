// cppchain —— agent/stale.hpp
// 状态时效性（M7 后新增的问题域，见 04-problems/06）：
// 有些状态必须**实时获取**，不能凭上下文里的历史结果推断。
//
// 本中间件做两件事：
//   after_tool   → 给工具结果打**观测时间戳**（+ 工具声明的 TTL），随序列化/快照存活
//   before_model → 扫描已过期结果，**改写为"已过期"标注**（保留 tool_call_id，维持协议闭环）
//
// 为什么是"改写"而不是"删除"：M1 的协议不变式要求 tool_calls 必须有对应 ToolMessage，
// 删掉会破坏闭环；而且**把旧值换成显式警示**比留着旧值更安全（模型不会再拿它当现状）。
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "cppchain/agent/agent.hpp"

namespace cppchain::agent {

class StaleStateGuard final : public Middleware {
 public:
  // 可注入时钟：测试用假时钟保证确定性
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  explicit StaleStateGuard(Clock clock = [] { return std::chrono::system_clock::now(); })
      : clock_(std::move(clock)) {}

  // 执行前：登记该调用的 TTL（来自工具声明）
  std::optional<message::Message> lookup_tool_result(message::ToolCall const& call,
                                                     tool::ToolSpec const* spec) override;

  // 执行后：打观测时间戳
  void after_tool(message::Message& tool_message) override;

  // 模型调用前：把过期结果改写为警示
  void before_model(std::vector<message::Message>& msgs) override;

  [[nodiscard]] std::size_t invalidations() const { return invalidations_; }

  // additional_kwargs 里用的键（供测试与外部检查）
  static constexpr char kObservedAt[] = "observed_at";    // epoch 秒
  static constexpr char kTtlSeconds[] = "ttl_seconds";    // 工具声明的 TTL
  static constexpr char kStale[] = "stale";               // 已被标记过期

 private:
  Clock clock_;
  std::unordered_map<std::string, std::int64_t> pending_ttl_;  // tool_call_id → ttl 秒
  std::size_t invalidations_ = 0;
};

}  // namespace cppchain::agent