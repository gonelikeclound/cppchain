// demo: messages_tool_flow —— 工具调用消息协议闭环（转译 langchain tool-calling demo）
//
// langchain 参照：
//   llm.bind_tools(tools) → 模型返回 AIMessage(tool_calls=...)
//   → ToolNode 执行工具 → ToolMessage(tool_call_id=...) 回填
//   → 后续把 ToolMessage 追加回对话（add_messages）
// 本 demo 不接真模型：用一个"假模型"产出 tool_calls，一个极简"工具执行器"执行并回填，
// 专注演示**消息协议本身**（M3 才做真正的 ToolNode）。
#include <cppchain/message/message.hpp>

#include <functional>
#include <print>
#include <string>
#include <vector>

#include "support/msg_util.hpp"

using namespace cppchain::message;

// 极简工具执行器：name → 结果文本（模拟真实工具返回值）
static std::string executeTool(std::string const& name, nlohmann::json const& args) {
  if (name == "get_weather" && args.contains("city"))
    return "Sunny, " + args["city"].get<std::string>() + ", 24C";
  return "unknown tool: " + name;
}

int main() {
  // 1. 假模型：对用户消息产出一个带 tool_calls 的 AIMessage
  Message user = human_message("weather in SF?");
  Message model = ai_message("I'll look it up.");
  model.tool_calls.push_back(ToolCall{"call_1", "get_weather", nlohmann::json{{"city", "SF"}}});

  demo::show_conversation({user, model});

  // 2. 执行工具并回填 ToolMessage（协议核心：tool_call_id 必须对应）
  Message tool_result;
  tool_result.type = Role::Tool;
  for (auto const& tc : model.tool_calls) {
    tool_result.tool_call_id = tc.id;  // 回填目标 tool call
    std::string out = executeTool(tc.name, tc.args);
    tool_result.content.push_back(TextBlock{out, std::nullopt});
  }
  Message final = ai_message("It's sunny in SF, 24C.");

  std::vector<Message> conv{user, model, tool_result, final};
  demo::show_conversation(conv);

  // 3. 协议不变式校验（validate_sequence）
  auto err = validate_sequence(conv);
  if (err) {
    std::println("!! 不变式被破坏: {}", *err);
    return 1;
  }
  std::println("✓ 协议完整：每个 tool_call 都有对应 ToolMessage 回填");

  // 4. 反例：若忘了回填，校验器会明确报错
  std::vector<Message> broken{user, model, ai_message("oops, forgot the tool")};
  auto err2 = validate_sequence(broken);
  std::println("反例（缺 ToolMessage）会被拦截: {}", err2 ? *err2 : "(未拦截!)");
  if (!err2) return 1;

  return 0;
}