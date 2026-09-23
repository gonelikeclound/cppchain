// cppchain —— tool/tool_node.hpp
// ToolNode 等价物：把一条 AI 消息里的 tool_calls 执行掉，产出对应的 ToolMessage。
// 并行（有界）、结果顺序与 tool_call_id 严格对应、错误回喂模型。
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/tool/registry.hpp"

namespace cppchain::tool {

struct ToolNodeOptions {
  std::size_t max_parallel = 4;  // 同一批 tool_calls 的最大并发
  bool catch_errors = true;      // true: 错误 → status="error" 的 ToolMessage；false: 抛 ToolException
};

class ToolNode {
 public:
  explicit ToolNode(ToolRegistry const* registry, ToolNodeOptions opts = {})
      : registry_(registry), opts_(opts) {}

  // 执行 AI 消息里的全部 tool_calls（顺序与 tool_calls 一致）
  [[nodiscard]] std::vector<message::Message> run(message::Message const& ai_message) const {
    return run(std::span<message::ToolCall const>(ai_message.tool_calls));
  }

  [[nodiscard]] std::vector<message::Message> run(std::span<message::ToolCall const> calls) const;

  [[nodiscard]] ToolNodeOptions const& options() const { return opts_; }

 private:
  [[nodiscard]] message::Message execute_one(message::ToolCall const& call) const;

  ToolRegistry const* registry_;
  ToolNodeOptions opts_;
};

}  // namespace cppchain::tool