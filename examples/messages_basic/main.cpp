// demo: messages_basic —— 消息类型族（转译 langchain "Messages" 概念 demo）
//
// langchain 参照：
//   from langchain_core.messages import HumanMessage, SystemMessage, AIMessage
//   msg.content / msg.text / content 里的 content-block（text/reasoning/tool_call）
#include <cppchain/message/message.hpp>

#include <print>

#include "support/msg_util.hpp"

using namespace cppchain::message;

int main() {
  // 1. langchain: SystemMessage("...") / HumanMessage("...") / AIMessage("...")
  Message sys  = system_message("You are a helpful assistant.");
  Message user = human_message("What's the weather in San Francisco?");
  Message ai   = ai_message("I'll check the weather for you.");

  demo::show("system (SystemMessage)", sys);
  demo::show("user   (HumanMessage)", user);
  demo::show("ai     (AIMessage)", ai);

  // 2. 内容块：正文 + 推理块（langchain: AI 回复里带 reasoning/thinking）
  Message thinker;
  thinker.type = Role::Ai;
  thinker.content = {TextBlock{"Final: it is sunny."}, ReasoningBlock{"Looked up current temp."}};
  demo::show("ai with reasoning block", thinker);

  // 3. chat 角色可以自定义（langchain: ChatMessage(role="developer")）
  Message dev;
  dev.type = Role::Chat;
  dev.custom_role = "developer";
  dev.content = {TextBlock{"Prefer concise answers."}};
  demo::show("chat (ChatMessage role=developer)", dev);

  // 4. Message::text() 拼接所有文本块（langchain: msg.text）
  Message multi;
  multi.type = Role::Ai;
  multi.content = {TextBlock{"alpha"}, TextBlock{" beta"}, ReasoningBlock{"(hidden)"}};
  std::println("text() 只取文本块, 顺序拼接: '{}' (reasoning 不在其中)", multi.text());
  std::println("role 判别: {} == 'ai' ? {}", role_str(multi.type), multi.type == Role::Ai);

  return 0;
}