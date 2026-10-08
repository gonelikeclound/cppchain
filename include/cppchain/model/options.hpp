// cppchain —— model/options.hpp
// 调用参数与模型配置。未建模的参数一律走 extra 透传。
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cppchain::model {

// 出站是否把 assistant 的 ReasoningBlock 作为 `reasoning_content` 回传给端点（wfr-㉙）。
//   Drop      永不回传 —— 引入前的行为，默认值（既有调用点零变化）；
//   ToolCalls 仅当该 assistant 消息带 tool_calls 时回传 —— DeepSeek V4 家族
//             "思考模式下带 tool_calls 的 assistant 必须回传 reasoning"的最小实现；
//   Always    只要该消息有 ReasoningBlock 就回传。
// 注意：ToolCalls 只对"本来就有 reasoning 的消息"补字段。若一段历史里某轮是用**非思考
// 模型**产生的（根本没有 ReasoningBlock），它在严格端点上仍无法满足回传要求 —— 即
// ToolCalls 档仅适用于**全程使用思考模型**的会话（见 docs/02-arch-design/15）。
enum class ReasoningEcho { Drop, ToolCalls, Always };

struct CallOptions {
  std::optional<std::string> model;        // 覆盖 ModelConfig.model
  std::optional<double> temperature;
  std::optional<int> max_tokens;
  std::vector<std::string> stop;
  // "auto"/"none"/"required"（字符串），或强制某函数：{"type":"function","function":{"name":…}}
  nlohmann::json tool_choice = nullptr;
  // 结构化输出用（如 {"type":"json_object"}）；非 null 时并入请求体
  nlohmann::json response_format = nullptr;
  nlohmann::json tools = nlohmann::json::array();  // M2 仅 wire 通道（M3 引入 Tool 抽象）
  nlohmann::json extra = nlohmann::json::object(); // 原样并入请求体（厂商私有参数）
  ReasoningEcho reasoning_echo = ReasoningEcho::Drop;  // 出站 reasoning_content 策略
};

struct ModelConfig {
  std::string api_key;
  std::string base_url = "https://api.openai.com/v1";
  std::string model;                       // 如 "deepseek-chat" / "gpt-4o-mini"
  long timeout_ms = 60000;
  std::vector<std::string> extra_headers;  // 形如 "X-Foo: bar"
  // 企业代理/自签证书环境的 CA bundle 路径（留空则用系统默认 CA）
  std::string ca_info;
};

}  // namespace cppchain::model