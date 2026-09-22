// M1 demo：构造 → 序列化 → 恢复 → 不变式校验 的消息流水线。
#include <cppchain/message/chunk.hpp>
#include <cppchain/message/message.hpp>
#include <cppchain/message/serializer.hpp>

#include <print>
#include <span>

using namespace cppchain::message;
using nlohmann::json;

int main() {
  // 1. 构造一轮"agent 对话"：user → ai(带 tool_call) → tool → ai
  Message u = human_message("What's the weather in SF?");

  Message ai = ai_message("Let me check.");
  ai.tool_calls.push_back(ToolCall{"call_1", "get_weather", json{{"city", "SF"}}});
  ai.usage = UsageMetadata{12, 4, 16, 0};

  Message t = tool_message("Sunny, 24C", "call_1");
  Message final = ai_message("It's sunny in SF, 24C.");

  std::vector<Message> conv{u, ai, t, final};

  // 2. 不变式校验
  auto err = validate_sequence(conv);
  if (err) {
    std::println("INVARIANT FAILED: {}", *err);
    return 1;
  }

  // 3. 整体序列化（规范 JSON）→ 恢复
  json doc = conv;
  std::vector<Message> restored = doc.get<std::vector<Message>>();
  bool same = (restored == conv);
  std::println("round-trip equal: {}", same);

  // 4. 流式演示：两段文本块 + 三段 args 片段 → 收敛成完整 tool call
  MessageChunk c1, c_rest;
  c1.type = c_rest.type = Role::Ai;
  c1.content = {TextBlock{"The weather "}};
  c_rest.content = {TextBlock{"is sunny."}};
  {
    ToolCallChunk t;
    t.name = "get_weather";
    t.args = "{\"cit";
    t.index = 0;
    t.has_name = true;
    t.has_args = true;
    c1.tool_call_chunks.push_back(t);
  }
  {
    ToolCallChunk t;
    t.args = "y\":\"SF\"}";
    t.index = 0;
    t.has_args = true;
    c_rest.tool_call_chunks.push_back(t);
  }
  Message streamed = finalize_chunk(c1 + c_rest);

  std::println("streamed text : {}", streamed.text());
  for (auto const& tc : streamed.tool_calls) {
    std::println("streamed tool : {}({}) args={}", tc.name, tc.id, tc.args.dump());
  }

  // 5. 打印规范 JSON 便于肉眼检查
  std::println("--- canonical json ---");
  std::println("{}", doc.dump(2));
  return same && !streamed.tool_calls.empty() ? 0 : 1;
}