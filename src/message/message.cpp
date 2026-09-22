// cppchain_message —— message.cpp
// Role 转换 / 构造助手 / 协议不变式校验（非模板实现集中在此）。
#include "cppchain/message/message.hpp"

#include <stdexcept>
#include <string>

namespace cppchain::message {

std::string_view role_str(Role r) {
  switch (r) {
    case Role::System: return "system";
    case Role::Human:  return "human";
    case Role::Ai:     return "ai";
    case Role::Tool:   return "tool";
    case Role::Chat:   return "chat";
  }
  return "unknown";
}

Role role_from_str(std::string_view s) {
  if (s == "system") return Role::System;
  if (s == "human") return Role::Human;
  if (s == "ai") return Role::Ai;
  if (s == "tool") return Role::Tool;
  if (s == "chat") return Role::Chat;
  throw std::invalid_argument("cppchain::message: unknown role type: " + std::string(s));
}

namespace {
Message with_text(Role r, std::string text, std::optional<std::string> id) {
  Message m;
  m.type = r;
  m.id = std::move(id);
  if (!text.empty()) m.content.push_back(TextBlock{std::move(text), std::nullopt});
  return m;
}
}  // namespace

Message system_message(std::string text, std::optional<std::string> id) {
  return with_text(Role::System, std::move(text), std::move(id));
}
Message human_message(std::string text, std::optional<std::string> id) {
  return with_text(Role::Human, std::move(text), std::move(id));
}
Message ai_message(std::string text, std::optional<std::string> id) {
  return with_text(Role::Ai, std::move(text), std::move(id));
}

Message tool_message(std::string content, std::string tool_call_id, std::string status) {
  Message m;
  m.type = Role::Tool;
  m.tool_call_id = std::move(tool_call_id);
  m.status = std::move(status);
  if (!content.empty()) m.content.push_back(TextBlock{std::move(content), std::nullopt});
  return m;
}

std::optional<std::string> validate_sequence(std::span<Message const> msgs) {
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    auto const& ai = msgs[i];
    if (!ai.has_tool_calls()) continue;

    std::vector<std::string> missing;
    for (auto const& tc : ai.tool_calls) {
      if (!tc.id.empty()) missing.push_back(tc.id);
    }
    if (missing.empty()) continue;  // 无 id 的 tool call 不做回填追踪

    for (std::size_t j = i + 1; j < msgs.size() && !missing.empty(); ++j) {
      auto const& m = msgs[j];
      if (m.is_tool() && m.tool_call_id) {
        for (auto it = missing.begin(); it != missing.end();) {
          if (*it == *m.tool_call_id) it = missing.erase(it);
          else ++it;
        }
      }
    }
    if (!missing.empty()) {
      std::string ids;
      for (auto const& id : missing) ids += " " + id;
      return "message #" + std::to_string(i) + " has tool calls without matching ToolMessage:" +
             ids;
    }
  }
  return std::nullopt;
}

}  // namespace cppchain::message