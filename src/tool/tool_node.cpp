// cppchain —— src/tool/tool_node.cpp
#include "cppchain/tool/tool_node.hpp"

#include <algorithm>
#include <exception>
#include <future>
#include <string>

#include "cppchain/message/utf8.hpp"

namespace cppchain::tool {

namespace {

using cppchain::message::Message;
using cppchain::message::TextBlock;
using cppchain::message::ToolCall;

// 错误回喂：status="error" 的 ToolMessage，内容是可读错误（供模型自愈）
Message error_tool_message(std::string const& tool_call_id, std::string const& text) {
  Message m = message::tool_message("", tool_call_id, "error");
  m.content.clear();
  m.content.push_back(TextBlock{text, std::nullopt});
  return m;
}

}  // namespace

Message ToolNode::execute_one(ToolCall const& call) const {
  ToolContext ctx;
  ctx.tool_call_id = call.id;

  ToolSpec const* spec = (registry_ != nullptr) ? registry_->find(call.name) : nullptr;
  if (spec == nullptr) {
    if (!opts_.catch_errors) throw ToolException("unknown tool: " + call.name);
    return error_tool_message(call.id, "unknown tool: " + call.name);
  }
  if (!spec->handler) {
    if (!opts_.catch_errors) throw ToolException("tool has no handler: " + call.name);
    return error_tool_message(call.id, "tool has no handler: " + call.name);
  }

  // wfr-㊸ @②：执行器按工具规格注入（不依赖 ExecutionGuard 路径；
  // 与 exec_guard.cpp 的 run 同契约 —— bash 等命令工具靠 ctx.executor 才能执行）。
  ctx.executor = spec->executor;

  std::expected<nlohmann::json, ToolError> result;
  try {
    result = spec->handler(call.args, ctx);
  } catch (std::exception const& e) {
    if (!opts_.catch_errors) throw;
    return error_tool_message(call.id, std::string("tool threw exception: ") + e.what());
  }

  if (!result) {
    if (!opts_.catch_errors) throw ToolException(result.error().message);
    return error_tool_message(call.id, result.error().message);
  }

  std::string content = result->is_string() ? result->get<std::string>() : message::utf8_safe_dump(*result);
  message::sanitize_utf8(content);   // 双保险：字符串型结果也可能带非法字节
  return message::tool_message(std::move(content), call.id);
}

std::vector<Message> ToolNode::run(std::span<ToolCall const> calls) const {
  std::vector<Message> out(calls.size());
  std::size_t const width = std::max<std::size_t>(1, opts_.max_parallel);

  // 分批并行：批内并发 ≤ max_parallel，批间串行 → 有界；结果按下标回填 → 顺序确定
  for (std::size_t base = 0; base < calls.size(); base += width) {
    std::size_t const end = std::min(base + width, calls.size());
    std::vector<std::future<Message>> batch;
    batch.reserve(end - base);
    for (std::size_t i = base; i < end; ++i) {
      batch.push_back(
          std::async(std::launch::async, [this, &calls, i] { return execute_one(calls[i]); }));
    }
    for (std::size_t i = base; i < end; ++i) out[i] = batch[i - base].get();
  }
  return out;
}

}  // namespace cppchain::tool