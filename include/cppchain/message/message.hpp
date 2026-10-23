// cppchain_message —— message.hpp
// M1: Message 主类型。参照 langchain_core/messages/base.py + ai.py + tool.py。
// 设计：单一 struct + Role 判别（不搞继承树），tool/chat 相关字段为可选槽，
//      JSON 化零成本，判别键为 "type"。
#pragma once

#include <nlohmann/json.hpp>

#include "cppchain/message/content_block.hpp"

#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <vector>

namespace cppchain::message {

// ---- Role（消息判别式，对应 type 字面量）----

enum class Role { System, Human, Ai, Tool, Chat };

// Role ↔ 字符串（"system"/"human"/"ai"/"tool"/"chat"）；未知字符串抛异常（定义于 message.cpp）
std::string_view role_str(Role r);
Role role_from_str(std::string_view s);

// ---- 用量元数据（简化版：只留基本计数 + reasoning）----
// 参照 langchain UsageMetadata；v1 砍掉 audio/cache_* 细节。

struct UsageMetadata {
  long input_tokens = 0;
  long output_tokens = 0;
  long total_tokens = 0;
  long reasoning_tokens = 0;

  auto operator<=>(UsageMetadata const&) const = default;
};

// ---- Message ----

// 历史摘要消息的标记（wfr-㊵）。放在 additional_kwargs 里，序列化会原样往返。
// **单一来源**：state 层的 fold 归约靠它剥掉头部旧摘要（世代交替而不是堆叠），
// summarize 节点靠它判断"已经折叠过、还需不需要再折"。
inline constexpr char kSummaryMarker[] = "cppchain_summary";

struct Message {
  Role type = Role::Human;
  std::vector<ContentBlock> content{};   // 纯文本也归一化为 TextBlock（见 serializer normalize）
  std::optional<std::string> id;
  std::optional<std::string> name;

  nlohmann::json response_metadata = nlohmann::json::object();  // 原生响应元数据（透传）
  nlohmann::json additional_kwargs = nlohmann::json::object();

  // Role::Tool 专用
  std::optional<std::string> tool_call_id;  // 回填 target tool call（不变式，见 validate_sequence）
  std::string status = "success";           // "success" | "error"
  // 完整结果落盘后的引用（M9 artifact 落盘，见 02-arch-design/10）。缺省为空 → 正常内联内容。
  std::optional<std::string> artifact;

  // Role::Ai 专用
  std::vector<ToolCall> tool_calls;
  std::vector<InvalidToolCall> invalid_tool_calls;
  std::optional<UsageMetadata> usage;

  // Role::Chat 专用（自定义 role 字符串兜底）
  std::optional<std::string> custom_role;

  // 相等性：成员含 nlohmann::json（无 <=>），故显式默认化 ==；不做排序。
  bool operator==(Message const&) const = default;

  // ---- 便利访问 ----

  bool has_tool_calls() const { return !tool_calls.empty(); }
  bool is_tool() const { return type == Role::Tool; }
  // 是否含图片块（视觉输入）——wire 据此把 content 升为数组（见 model/openai/wire.hpp）。
  bool has_images() const {
    for (auto const& b : content)
      if (std::holds_alternative<ImageBlock>(b)) return true;
    return false;
  }

  // 拼接文本内容（text 块的连接）；无文本 → 空串
  std::string text() const {
    std::string out;
    for (auto const& b : content) {
      if (auto const* t = std::get_if<TextBlock>(&b)) {
        if (!out.empty()) out += '\n';
        out += t->text;
      }
    }
    return out;
  }
};

// 该消息是不是历史摘要（见 kSummaryMarker）
[[nodiscard]] inline bool is_summary_message(Message const& m) {
  auto const it = m.additional_kwargs.find(kSummaryMarker);
  return it != m.additional_kwargs.end() && it->is_boolean() && it->get<bool>();
}

// ---- 构造助手（让调用点更表达力） ----

Message system_message(std::string text, std::optional<std::string> id = std::nullopt);
Message human_message(std::string text, std::optional<std::string> id = std::nullopt);
Message ai_message(std::string text, std::optional<std::string> id = std::nullopt);
Message tool_message(std::string content, std::string tool_call_id,
                     std::string status = "success");

// ---- 协议不变式 ----

// 校验消息序列的一条核心不变式：
//   出现 tool_calls 的 Ai 消息之后，必须紧跟对应的 ToolMessage（按 id 一一对应）。
// 返回 nullopt = 通过；否则返回说明。
// （M1 提供校验，M3 的工具闭环对它强制执行。）
std::optional<std::string> validate_sequence(std::span<Message const> msgs);

}  // namespace cppchain::message