// cppchain_message —— content_block.hpp
// M1: ContentBlock 判别联合。参照 langchain_core/messages/content.py。
// 文本 / 推理 / 工具调用 四族 + ImageBlock（视觉/多模态：图片识别）。
// server_tool_* 仍延后。
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cppchain::message {

// ---- 块（content block）----

// 文本块
struct TextBlock {
  std::string text;
  std::optional<std::string> id = std::nullopt;

  bool operator==(TextBlock const&) const = default;
};

// 推理块（reasoning / thinking）
struct ReasoningBlock {
  std::string reasoning;
  std::optional<std::string> id = std::nullopt;

  bool operator==(ReasoningBlock const&) const = default;
};

// 工具调用块。与 Message::tool_calls 里的 ToolCall 是同一个类型：
// langchain 中 message.content_blocks 的 tool_call 与此为同一数据的不同视图。
struct ToolCall {
  std::string id;
  std::string name;
  nlohmann::json args = nlohmann::json::object();

  bool operator==(ToolCall const&) const = default;
};

// 非法工具调用（模型产出的 tool call 未通过校验）
struct InvalidToolCall {
  std::string id;
  std::string name;
  std::string error;

  bool operator==(InvalidToolCall const&) const = default;
};

// 图片块（视觉/多模态输入，OpenAI 兼容的 image_url 部件）。
// 两种来源二选一：
//   · data       —— base64 内联像素（wire 时拼成 data:<media_type>;base64,<data> 的 data URL）
//   · url        —— 外链 / 完整 data URL（端点自行拉取）
// media_type 形如 "image/png"；内联时给全，外链可缺省。
struct ImageBlock {
  std::optional<std::string> media_type = std::nullopt;
  std::optional<std::string> data = std::nullopt;
  std::optional<std::string> url = std::nullopt;
  std::optional<std::string> id = std::nullopt;

  bool operator==(ImageBlock const&) const = default;
};

// ContentBlock 判别联合。判别键 = variant index ↔ 字符串 "type"。
// ImageBlock 追加在末位 → 既有 index 0..3（text/reasoning/tool_call/invalid）语义不动。
using ContentBlock = std::variant<TextBlock, ReasoningBlock, ToolCall, InvalidToolCall, ImageBlock>;

// 块 → 判别字符串
inline std::string_view content_block_type(ContentBlock const& block) {
  return std::visit(
      []<typename T>(T const&) -> std::string_view {
        if constexpr (std::same_as<T, TextBlock>)
          return "text";
        else if constexpr (std::same_as<T, ReasoningBlock>)
          return "reasoning";
        else if constexpr (std::same_as<T, ToolCall>)
          return "tool_call";
        else if constexpr (std::same_as<T, InvalidToolCall>)
          return "invalid_tool_call";
        else
          return "image";
      },
      block);
}

}  // namespace cppchain::message