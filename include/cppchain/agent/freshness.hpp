// cppchain —— agent/freshness.hpp
// 时效元数据契约（跨中间件共享）：观测时间 / TTL / 已失效标记写在 ToolMessage 的
// additional_kwargs 里，随序列化与快照存活。
//
// 为什么单独一个头文件：写入方是 StaleStateGuard（stale.hpp），读取方还有
// TrimHistory（agent.hpp）——若直接互相 include 会成环，故把契约抽到这里。
#pragma once

#include <cstdint>
#include <optional>

#include "cppchain/message/message.hpp"

namespace cppchain::agent::freshness {

inline constexpr char kObservedAt[] = "observed_at";  // epoch 秒
inline constexpr char kTtlSeconds[] = "ttl_seconds";  // 工具声明的 TTL
inline constexpr char kStale[] = "stale";             // 已被标记为过期

// 是否携带时效元数据（即"易变状态的观测结果"）
[[nodiscard]] inline bool has_metadata(message::Message const& m) {
  return m.additional_kwargs.is_object() && m.additional_kwargs.contains(kObservedAt) &&
         m.additional_kwargs.contains(kTtlSeconds);
}

// 该结果是否已过期（now_epoch 与观测时间之差 > TTL）
[[nodiscard]] inline std::optional<bool> is_expired(message::Message const& m,
                                                    std::int64_t now_epoch) {
  if (!has_metadata(m)) return std::nullopt;
  auto const observed = m.additional_kwargs[kObservedAt].get<std::int64_t>();
  auto const ttl = m.additional_kwargs[kTtlSeconds].get<std::int64_t>();
  return (now_epoch - observed) > ttl;
}

}  // namespace cppchain::agent::freshness