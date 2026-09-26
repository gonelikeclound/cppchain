// cppchain_message —— serializer.hpp
// M1 序列化契约：判别键 "type" 的规范 JSON（见 02-arch-design/01 §5）。
// round-trip 不变式：to_json(from_json(x)) 与 x 等价；未知 type 抛异常（不静默默认）。
#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

#include "cppchain/message/content_block.hpp"
#include "cppchain/message/message.hpp"

#include <optional>

// nlohmann 原生不支持 std::optional（会误走数组构造路径），这里补一个通用特化。
namespace nlohmann {
template <typename T>
struct adl_serializer<std::optional<T>> {
  static void to_json(json& j, std::optional<T> const& opt) {
    if (!opt) j = nullptr;
    else j = *opt;
  }
  static void from_json(json const& j, std::optional<T>& opt) {
    if (j.is_null()) opt.reset();
    else opt = j.get<T>();
  }
};
}  // namespace nlohmann

namespace cppchain::message {

namespace detail {
inline std::optional<std::string> opt_str(nlohmann::json const& j, char const* key) {
  if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
  return j.at(key).get<std::string>();
}
inline void put_opt(nlohmann::json& j, char const* key, std::optional<std::string> const& v) {
  if (v) j[key] = *v;
}
}  // namespace detail

// 公共助手：读可选字符串字段。
// 为什么需要它：规范 JSON 对可选字段**显式写 null**（如 "id": null），
// 而 nlohmann 的 `value(key, default)` 只在**键缺失**时用默认值 —— 值为 null 时会返回 null
// 并可能在转 string 时抛 type_error。凡读可选字段，请用本函数。
inline std::optional<std::string> opt_string(nlohmann::json const& j, std::string_view key) {
  auto const it = j.find(std::string(key));
  if (it == j.end() || it->is_null()) return std::nullopt;
  return it->get<std::string>();
}

// ---- UsageMetadata ----
inline void to_json(nlohmann::json& j, UsageMetadata const& u) {
  j = {{"input_tokens", u.input_tokens},
       {"output_tokens", u.output_tokens},
       {"total_tokens", u.total_tokens},
       {"reasoning_tokens", u.reasoning_tokens}};
}
inline void from_json(nlohmann::json const& j, UsageMetadata& u) {
  if (j.contains("input_tokens")) j.at("input_tokens").get_to(u.input_tokens);
  if (j.contains("output_tokens")) j.at("output_tokens").get_to(u.output_tokens);
  if (j.contains("total_tokens")) j.at("total_tokens").get_to(u.total_tokens);
  if (j.contains("reasoning_tokens")) j.at("reasoning_tokens").get_to(u.reasoning_tokens);
}

// ---- ToolCall / InvalidToolCall（独立槽位形态：无 type 键）----
inline void to_json(nlohmann::json& j, ToolCall const& tc) {
  j = {{"id", tc.id}, {"name", tc.name}, {"args", tc.args}};
}
inline void from_json(nlohmann::json const& j, ToolCall& tc) {
  j.at("id").get_to(tc.id);
  j.at("name").get_to(tc.name);
  if (j.contains("args")) j.at("args").get_to(tc.args);
}
inline void to_json(nlohmann::json& j, InvalidToolCall const& ic) {
  j = {{"id", ic.id}, {"name", ic.name}, {"error", ic.error}};
}
inline void from_json(nlohmann::json const& j, InvalidToolCall& ic) {
  j.at("id").get_to(ic.id);
  j.at("name").get_to(ic.name);
  if (j.contains("error")) j.at("error").get_to(ic.error);
}

// ---- ContentBlock（判别联合：type 键）----
inline void to_json(nlohmann::json& j, ContentBlock const& b) {
  std::visit(
      [&j](auto const& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::same_as<T, TextBlock>) {
          j = {{"type", "text"}, {"text", v.text}};
          detail::put_opt(j, "id", v.id);
        } else if constexpr (std::same_as<T, ReasoningBlock>) {
          j = {{"type", "reasoning"}, {"reasoning", v.reasoning}};
          detail::put_opt(j, "id", v.id);
        } else if constexpr (std::same_as<T, ToolCall>) {
          j = {{"type", "tool_call"}, {"id", v.id}, {"name", v.name}, {"args", v.args}};
        } else {
          j = {{"type", "invalid_tool_call"},
               {"id", v.id},
               {"name", v.name},
               {"error", v.error}};
        }
      },
      b);
}

inline void from_json(nlohmann::json const& j, ContentBlock& b) {
  auto type = j.at("type").get<std::string>();
  if (type == "text") {
    b = TextBlock{j.at("text").get<std::string>(), detail::opt_str(j, "id")};
  } else if (type == "reasoning") {
    b = ReasoningBlock{j.at("reasoning").get<std::string>(), detail::opt_str(j, "id")};
  } else if (type == "tool_call") {
    b = ToolCall{j.at("id").get<std::string>(), j.at("name").get<std::string>(),
                 j.value("args", nlohmann::json::object())};
  } else if (type == "invalid_tool_call") {
    b = InvalidToolCall{j.at("id").get<std::string>(), j.at("name").get<std::string>(),
                        detail::opt_str(j, "error").value_or("")};
  } else {
    throw std::invalid_argument("cppchain::message: unknown content block type: " + type);
  }
}

// ---- Message（判别键 type = Role 字符串）----
inline void to_json(nlohmann::json& j, Message const& m) {
  j = {{"type", role_str(m.type)},
       {"content", m.content},
       {"id", m.id ? nlohmann::json(*m.id) : nlohmann::json(nullptr)},
       {"name", m.name ? nlohmann::json(*m.name) : nlohmann::json(nullptr)},
       {"status", m.status},
       {"tool_call_id",
        m.tool_call_id ? nlohmann::json(*m.tool_call_id) : nlohmann::json(nullptr)},
       {"artifact", m.artifact ? nlohmann::json(*m.artifact) : nlohmann::json(nullptr)}};
  if (!m.tool_calls.empty()) j["tool_calls"] = m.tool_calls;
  if (!m.invalid_tool_calls.empty()) j["invalid_tool_calls"] = m.invalid_tool_calls;
  if (m.usage) j["usage"] = *m.usage;
  if (!m.response_metadata.empty()) j["response_metadata"] = m.response_metadata;
  if (!m.additional_kwargs.empty()) j["additional_kwargs"] = m.additional_kwargs;
  if (m.custom_role) j["custom_role"] = *m.custom_role;
}

inline void from_json(nlohmann::json const& j, Message& m) {
  m = Message{};
  m.type = role_from_str(j.at("type").get<std::string>());
  if (j.contains("content")) j.at("content").get_to(m.content);
  m.id = detail::opt_str(j, "id");
  m.name = detail::opt_str(j, "name");
  if (j.contains("status")) j.at("status").get_to(m.status);
  m.tool_call_id = detail::opt_str(j, "tool_call_id");
  m.artifact = detail::opt_str(j, "artifact");
  if (j.contains("tool_calls")) j.at("tool_calls").get_to(m.tool_calls);
  if (j.contains("invalid_tool_calls")) j.at("invalid_tool_calls").get_to(m.invalid_tool_calls);
  if (j.contains("usage")) j.at("usage").get_to(m.usage);
  if (j.contains("response_metadata")) j.at("response_metadata").get_to(m.response_metadata);
  if (j.contains("additional_kwargs")) j.at("additional_kwargs").get_to(m.additional_kwargs);
  m.custom_role = detail::opt_str(j, "custom_role");
}

}  // namespace cppchain::message