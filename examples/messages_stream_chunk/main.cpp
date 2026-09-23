// demo: messages_stream_chunk —— 流式 chunk 合并（转译 langchain streaming / chunk 概念）
//
// langchain 参照：
//   for chunk in llm.stream(...):       # 每片是 AIMessageChunk
//       acc = acc + chunk               # __add__ 按规则合并（文本逐段拼、工具 args 累积）
//   msg = acc 收敛成完整消息
// 本 demo 模拟"模型流式输出"：正文逐段到达，同时工具调用参数分多片到达。
#include <cppchain/message/chunk.hpp>
#include <cppchain/message/message.hpp>

#include <print>
#include <string>
#include <vector>

#include "support/msg_util.hpp"

using namespace cppchain::message;

int main() {
  // 模拟网络到达的流式帧（每帧一个 MessageChunk）
  std::vector<MessageChunk> frames;
  auto push_text = [&frames](std::string const& text) {
    MessageChunk c;
    c.type = Role::Ai;
    c.content = {TextBlock{text}};
    frames.push_back(c);
  };
  auto push_tool_name = [&frames](std::string const& name) {
    MessageChunk c;
    c.type = Role::Ai;
    ToolCallChunk t;
    t.name = name;
    t.has_name = true;
    c.tool_call_chunks.push_back(t);
    frames.push_back(c);
  };
  auto push_tool_args = [&frames](std::string const& args_frag) {
    MessageChunk c;
    c.type = Role::Ai;
    ToolCallChunk t;
    t.args = args_frag;
    t.has_args = true;
    c.tool_call_chunks.push_back(t);
    frames.push_back(c);
  };

  push_text("Let me look that ");
  push_text("up in SF. ");
  push_tool_name("get_weather");
  push_tool_args("{\"city\":\"");   // 工具 args 参数分片：第 1 片
  push_text("The forecast: ");
  push_tool_args("SF\"}");          // 第 2 片 —— 拼起来才是完整 JSON
  push_text("sunny, 24C.");

  std::println("共 {} 帧到达, 开始按序合并...", frames.size());

  // 逐帧 operator+ 合并（对应 langchain 的 acc = acc + chunk）
  MessageChunk acc;
  std::size_t step = 0;
  auto text_so_far = [&acc] {
    std::string out;
    for (auto const& b : acc.content)
      if (auto const* t = std::get_if<TextBlock>(&b)) out += t->text;
    return out;
  };
  for (auto const& f : frames) {
    acc = acc + f;
    std::println("  帧[{}] 内容块={} 工具chunk={} | 文本已拼='{}'", step++,
                 acc.content.size(), acc.tool_call_chunks.size(), text_so_far());
  }

  // 收敛为完整消息
  Message m = finalize_chunk(acc);
  demo::show("final (AIMessageChunk → AIMessage)", m);

  bool ok = m.text() == "Let me look that up in SF. The forecast: sunny, 24C."
         && m.tool_calls.size() == 1
         && m.tool_calls[0].name == "get_weather"
         && m.tool_calls[0].args.at("city").get<std::string>() == "SF";
  std::println("流式合并→收敛 验证: {}", ok);
  return ok ? 0 : 1;
}