// demo: messages_serialize —— 序列化契约（转译 langchain messages_to_dict / .model_dump()）
//
// langchain 参照：
//   messages_to_dict(msgs) / json.dumps → 落盘 → messages_from_dict 恢复
//   以及更重要的契约："state（含消息）必须整体 JSON 可序列化"
//   —— 这正是 M5 Checkpointer / M7 上下文管理的底座。
#include <cppchain/message/message.hpp>
#include <cppchain/message/serializer.hpp>

#include <filesystem>
#include <fstream>
#include <print>
#include <string>

#include "support/msg_util.hpp"

using namespace cppchain::message;

int main() {
  // 构造一轮对话（含工具调用消息，覆盖全部角色）
  Message user = human_message("What's the weather in SF?", "u_1");
  Message ai;
  ai.type = Role::Ai;
  ai.id = "a_1";
  ai.content = {TextBlock{"Let me check."}};
  ai.tool_calls.push_back(ToolCall{"call_1", "get_weather", nlohmann::json{{"city", "SF"}}});
  ai.usage = UsageMetadata{12, 4, 16, 0};
  Message tool = tool_message("Sunny, 24C", "call_1");
  Message final = ai_message("It's sunny in SF, 24C.", "a_2");

  std::vector<Message> conv{user, ai, tool, final};
  demo::show_conversation(conv);

  // 1. 整条会话 → 规范 JSON → 落盘（这就是持久化/跨进程交换的形态）
  //    产物写到 demo-out/（.gitignore 忽略），避免污染仓库根目录
  std::filesystem::create_directories("demo-out");
  std::filesystem::path const out_path = "demo-out/conversation.json";
  nlohmann::json doc = conv;
  {
    std::ofstream out(out_path);
    out << doc.dump(2);
  }
  std::println("--- 已落盘: {} ---", out_path.string());

  // 2. 从文件读回 → 反序列化 → 与内存原件比对
  std::ifstream in(out_path);
  nlohmann::json back;
  in >> back;
  auto restored = back.get<std::vector<Message>>();

  bool same = (restored == conv);
  std::println("round-trip 恢复后与原件相等: {}", same);

  // 3. 单条消息可直接取规范 JSON（对应 langchain msg.model_dump()）
  auto const ai_json = nlohmann::json(ai);
  std::println("单条 model_dump() 形态: {}", ai_json.dump());

  return same ? 0 : 1;
}