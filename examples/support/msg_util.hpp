// examples/support/msg_util.hpp
// 极简版 langchain pretty_repr / get_buffer_string：把 Message 打印成可读形态。
// 仅用于 demo，不进库。
#pragma once

#include <cppchain/message/content_block.hpp>
#include <cppchain/message/message.hpp>

#include <concepts>
#include <print>
#include <span>
#include <string>
#include <string_view>

namespace demo {

// 单条消息的"人读"输出
inline void show(std::string_view label, cppchain::message::Message const& m) {
  std::println("── {} ──", label);
  std::println("  role         : {}", cppchain::message::role_str(m.type));
  if (m.id) std::println("  id           : {}", *m.id);
  for (auto const& b : m.content) {
    std::visit(
        [](auto const& v) {
          if constexpr (std::same_as<std::decay_t<decltype(v)>, cppchain::message::TextBlock>)
            std::println("  content/text : {}", v.text);
          else if constexpr (std::same_as<std::decay_t<decltype(v)>,
                                          cppchain::message::ReasoningBlock>)
            std::println("  content/reason: {}", v.reasoning);
          else if constexpr (std::same_as<std::decay_t<decltype(v)>,
                                          cppchain::message::ToolCall>)
            std::println("  content/tool  : {}({}) args={}", v.name, v.id, v.args.dump());
          else
            std::println("  content/inv   : {} err={}", v.name, v.error);
        },
        b);
  }
  for (auto const& tc : m.tool_calls)
    std::println("  tool_calls    : {}({}) args={}", tc.name, tc.id, tc.args.dump());
  if (m.tool_call_id) std::println("  tool_call_id  : {}", *m.tool_call_id);
  if (m.usage)
    std::println("  usage         : in={} out={} total={}", m.usage->input_tokens,
                 m.usage->output_tokens, m.usage->total_tokens);
}

// 会话（消息序列）整体输出
inline void show_conversation(std::span<cppchain::message::Message const> msgs) {
  std::println("=== conversation ({} msgs) ===", msgs.size());
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    std::println("[{}] {} : {}", i, cppchain::message::role_str(msgs[i].type), msgs[i].text());
  }
}

}  // namespace demo