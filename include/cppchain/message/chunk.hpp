// cppchain_message —— chunk.hpp
// M1: 流式块（MessageChunk / ToolCallChunk）+ 合并语义。
// 参照 langchain_core BaseMessageChunk.__add__ + AIMessageChunk + _merge_tool_call_chunks。
// MessageChunk 只在流式运行期存在，不进入规范 JSON 序列化（serializer 只处理 Message）。
#pragma once

#include "cppchain/message/message.hpp"

#include <algorithm>
#include <vector>

namespace cppchain::message {

// 工具调用增量块：name/id 首见非空即取下；args 为增量 JSON 片段按序拼接，
// finalize 时一次性解析成完整 JSON。
struct ToolCallChunk {
  std::string id;
  std::string name;
  std::string args;      // 增量 JSON 片段（拼接前体）
  int index = 0;
  bool has_name = false;
  bool has_args = false;
  bool has_id = false;

  bool empty() const { return !has_name && !has_args && !has_id; }
};

// 流式消息块
struct MessageChunk {
  Role type = Role::Human;
  std::vector<ContentBlock> content{};
  std::vector<ToolCallChunk> tool_call_chunks{};
  std::vector<InvalidToolCall> invalid_calls{};
  std::optional<std::string> id;
  std::optional<std::string> name;
  std::string status = "success";
  std::optional<std::string> tool_call_id;
  std::optional<UsageMetadata> usage;
};

// ---- 合并（a 在前，b 在后，保持时间顺序）----

// 相邻同类型块合并：text+text 拼接；其余追加。
inline std::vector<ContentBlock> merge_content(std::vector<ContentBlock> a,
                                               std::vector<ContentBlock> const& b) {
  for (auto const& blk : b) {
    if (auto const* bt = std::get_if<TextBlock>(&blk);
        bt && !a.empty()) {
      if (auto* at = std::get_if<TextBlock>(&a.back())) {
        at->text += bt->text;
        continue;
      }
    }
    a.push_back(blk);
  }
  return a;
}

// 工具调用块按 index 累积合并
inline std::vector<ToolCallChunk> merge_tool_call_chunks(
    std::vector<ToolCallChunk> a, std::vector<ToolCallChunk> const& b) {
  for (auto const& r : b) {
    if (r.empty()) continue;
    auto it = std::ranges::find_if(a, [&](ToolCallChunk const& l) { return l.index == r.index; });
    if (it == a.end()) {
      a.push_back(r);
    } else {
      if (it->name.empty() && !r.name.empty()) { it->name = r.name; it->has_name = true; }
      if (it->id.empty() && !r.id.empty()) { it->id = r.id; it->has_id = true; }
      if (it->has_args && r.has_args) it->args += r.args;       // 增量片段按序拼接
      else if (r.has_args) { it->args += r.args; it->has_args = true; }
    }
  }
  std::ranges::sort(a, [](auto const& l, auto const& r) { return l.index < r.index; });
  return a;
}

inline MessageChunk operator+(MessageChunk a, MessageChunk const& b) {
  a.content = merge_content(std::move(a.content), b.content);
  a.tool_call_chunks = merge_tool_call_chunks(std::move(a.tool_call_chunks), b.tool_call_chunks);
  if (!a.id) a.id = b.id;
  if (!a.name) a.name = b.name;
  if (b.status != "success") a.status = b.status;
  if (!a.tool_call_id) a.tool_call_id = b.tool_call_id;
  if (a.usage && b.usage) {
    a.usage->input_tokens += b.usage->input_tokens;
    a.usage->output_tokens += b.usage->output_tokens;
    a.usage->total_tokens += b.usage->total_tokens;
    a.usage->reasoning_tokens += b.usage->reasoning_tokens;
  } else if (!a.usage && b.usage) {
    a.usage = b.usage;
  }
  a.invalid_calls.insert(a.invalid_calls.end(), b.invalid_calls.begin(), b.invalid_calls.end());
  return a;
}

// ---- 收敛：流式块 → 完整 Message ----
// 未拿到名字的 chunk 归入 invalid_tool_calls；args 解析失败同样归入 invalid。

inline Message finalize_chunk(MessageChunk const& c) {
  Message m;
  m.type = c.type;
  m.content = c.content;
  m.id = c.id;
  m.name = c.name;
  m.status = c.status;
  m.tool_call_id = c.tool_call_id;
  m.usage = c.usage;
  m.invalid_tool_calls = c.invalid_calls;

  for (auto const& chunk : c.tool_call_chunks) {
    if (chunk.name.empty()) {
      m.invalid_tool_calls.push_back(
          InvalidToolCall{chunk.id, chunk.name, "tool call missing name"});
      continue;
    }
    nlohmann::json args = nlohmann::json::object();
    if (chunk.has_args) {
      try {
        args = nlohmann::json::parse(chunk.args);
      } catch (nlohmann::json::exception const& e) {
        m.invalid_tool_calls.push_back(
            InvalidToolCall{chunk.id, chunk.name, std::string("args not parseable: ") + e.what()});
        continue;
      }
    }
    m.tool_calls.push_back(ToolCall{chunk.id, chunk.name, std::move(args)});
  }
  return m;
}

}  // namespace cppchain::message