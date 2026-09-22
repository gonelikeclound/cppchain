// M1 message 序列化契约测试：round-trip、判别、不变式、非法输入。
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <stdexcept>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"

using namespace cppchain::message;

static Message with_blocks(std::vector<ContentBlock> blocks) {
  Message m;
  m.type = Role::Ai;
  m.content = std::move(blocks);
  return m;
}

TEST(MessageRoundTrip, simple_human) {
  auto m = human_message("hello");
  auto const j = nlohmann::json(m);
  EXPECT_EQ(j.at("type").get<std::string>(), "human");
  auto back = j.get<Message>();
  EXPECT_EQ(back, m);
}

TEST(MessageRoundTrip, ai_with_tool_calls_and_usage) {
  Message m = ai_message("calling weather");
  m.tool_calls.push_back(ToolCall{"call_1", "get_weather", nlohmann::json{{"city", "SF"}}});
  m.usage = UsageMetadata{10, 5, 15, 0};
  m.response_metadata = {{"model", "gpt-x"}};

  auto const j = nlohmann::json(m);
  EXPECT_EQ(j.at("type").get<std::string>(), "ai");
  EXPECT_EQ(j.at("tool_calls")[0].at("name").get<std::string>(), "get_weather");
  EXPECT_EQ(j.at("usage").at("total_tokens").get<long>(), 15);

  auto back = j.get<Message>();
  EXPECT_EQ(back, m);
}

TEST(MessageRoundTrip, content_block_discriminator) {
  Message m = with_blocks({TextBlock{"think"}, ReasoningBlock{"hmm"}});
  auto const j = nlohmann::json(m);
  EXPECT_EQ(j.at("content")[0].at("type").get<std::string>(), "text");
  EXPECT_EQ(j.at("content")[1].at("type").get<std::string>(), "reasoning");
  auto back = j.get<Message>();
  EXPECT_EQ(back, m);

  // tool_call 块判别
  auto m2 = with_blocks(
      {ToolCall{"c1", "lookup", nlohmann::json{{"k", "v"}}}});
  auto const j2 = nlohmann::json(m2);
  EXPECT_EQ(j2.at("content")[0].at("type").get<std::string>(), "tool_call");
  auto back2 = j2.get<Message>();
  EXPECT_EQ(back2, m2);
}

TEST(MessageRoundTrip, tool_message) {
  auto m = tool_message("sunny", "call_1");
  auto const j = nlohmann::json(m);
  EXPECT_EQ(j.at("type").get<std::string>(), "tool");
  EXPECT_EQ(j.at("tool_call_id").get<std::string>(), "call_1");
  EXPECT_EQ(j.get<Message>(), m);
}

TEST(MessageSerde, unknown_content_block_type_throws) {
  nlohmann::json j = {{"type", "ai"}, {"content", {{{"type", "ELF"}}}}};
  EXPECT_THROW(j.get<Message>(), std::invalid_argument);
}

TEST(MessageSerde, unknown_role_throws) {
  nlohmann::json j = {{"type", "elf"}};
  EXPECT_THROW(j.get<Message>(), std::invalid_argument);
}

TEST(MessageInvariant, tool_tool_calls_require_tool_message) {
  Message ai = ai_message("calling");
  ai.tool_calls.push_back(ToolCall{"call_9", "f", nlohmann::json::object()});
  auto err = validate_sequence(std::vector<Message>{ai});
  ASSERT_TRUE(err.has_value());
  EXPECT_NE(err->find("call_9"), std::string::npos);

  std::vector<Message> ok{ai, tool_message("done", "call_9")};
  EXPECT_FALSE(validate_sequence(ok).has_value());
}