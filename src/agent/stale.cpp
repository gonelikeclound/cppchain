// cppchain —— src/agent/stale.cpp
#include "cppchain/agent/stale.hpp"

#include <utility>

namespace cppchain::agent {

using cppchain::message::Message;
using cppchain::message::Role;
using cppchain::message::TextBlock;

namespace {

std::int64_t epoch_seconds(std::chrono::system_clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

}  // namespace

std::optional<Message> StaleStateGuard::lookup_tool_result(message::ToolCall const& call,
                                                           tool::ToolSpec const* spec) {
  // 未声明 TTL 的工具不需要时效管理
  if (spec == nullptr || !spec->ttl.has_value()) {
    pending_ttl_.erase(call.id);
    return std::nullopt;  // 不干预执行
  }
  pending_ttl_[call.id] = spec->ttl->count();
  return std::nullopt;
}

void StaleStateGuard::after_tool(Message& tool_message) {
  if (!tool_message.tool_call_id) return;
  auto const it = pending_ttl_.find(*tool_message.tool_call_id);
  if (it == pending_ttl_.end()) return;

  tool_message.additional_kwargs[freshness::kObservedAt] = epoch_seconds(clock_());
  tool_message.additional_kwargs[freshness::kTtlSeconds] = it->second;
  pending_ttl_.erase(it);
}

void StaleStateGuard::before_model(std::vector<Message>& msgs) {
  std::int64_t const now = epoch_seconds(clock_());

  for (auto& m : msgs) {
    if (m.type != Role::Tool) continue;
    if (!freshness::has_metadata(m)) continue;
    if (m.additional_kwargs.value(freshness::kStale, false)) continue;  // 已标记过，不重复改写

    auto const observed = m.additional_kwargs[freshness::kObservedAt].get<std::int64_t>();
    auto const ttl = m.additional_kwargs[freshness::kTtlSeconds].get<std::int64_t>();
    std::int64_t const age = now - observed;
    if (age <= ttl) continue;  // 仍新鲜

    std::string const original = m.text();
    m.content.clear();
    m.content.push_back(TextBlock{
        "（此结果观测于 " + std::to_string(age) + " 秒前，已超过时效 " + std::to_string(ttl) +
            " 秒，**不能作为现状使用**。原值仅供参考：\"" +
            (original.size() > 120 ? original.substr(0, 120) + "…" : original) +
            "\"。若要基于该状态回答或决策，请重新调用工具获取最新值。）",
        std::nullopt});
    m.additional_kwargs[freshness::kStale] = true;
    ++invalidations_;
  }
}

}  // namespace cppchain::agent