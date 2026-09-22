// M1 chunk 合并语义测试：text/reasoning 拼接、tool_call 按 index 累积、args 片段还原。
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cppchain/message/chunk.hpp"
#include "cppchain/message/content_block.hpp"

using namespace cppchain::message;

static MessageChunk ai_chunk() {
  MessageChunk c;
  c.type = Role::Ai;
  return c;
}

// 工具类小助手：避免 designated-init 的 -Wmissing-field-initializers 干扰
static ToolCallChunk chunk(int index) {
  ToolCallChunk c;
  c.index = index;
  return c;
}

TEST(ChunkMerge, text_concat) {
  auto a = ai_chunk();
  a.content = {TextBlock{"ab"}};
  auto b = ai_chunk();
  b.content = {TextBlock{"cd"}};
  auto c = a + b;
  ASSERT_EQ(c.content.size(), 1u);
  EXPECT_EQ(std::get<TextBlock>(c.content[0]).text, "abcd");
}

TEST(ChunkMerge, different_blocks_appended) {
  auto a = ai_chunk();
  a.content = {TextBlock{"abc"}};
  auto b = ai_chunk();
  b.content = {ReasoningBlock{"r1"}};
  auto c = a + b;
  ASSERT_EQ(c.content.size(), 2u);
  EXPECT_EQ(std::get<ReasoningBlock>(c.content[1]).reasoning, "r1");
}

TEST(ChunkMerge, tool_chunk_accumulates_then_finalizes) {
  auto a = ai_chunk();
  {
    auto t = chunk(0);
    t.id = "c1";
    t.has_id = true;
    a.tool_call_chunks.push_back(t);
  }
  auto b = ai_chunk();
  {
    auto t = chunk(0);
    t.name = "get_weather";
    t.args = "{\"cit";
    t.has_name = true;
    t.has_args = true;
    b.tool_call_chunks.push_back(t);
  }
  auto c2 = ai_chunk();
  {
    auto t = chunk(0);
    t.args = "y\":\"SF\"}";
    t.has_args = true;
    c2.tool_call_chunks.push_back(t);
  }

  auto m = finalize_chunk((a + b) + c2);
  ASSERT_EQ(m.tool_calls.size(), 1u);
  EXPECT_EQ(m.tool_calls[0].id, "c1");
  EXPECT_EQ(m.tool_calls[0].name, "get_weather");
  EXPECT_EQ(m.tool_calls[0].args.at("city").get<std::string>(), "SF");
}

TEST(ChunkMerge, name_missing_goes_invalid) {
  auto a = ai_chunk();
  auto t = chunk(0);
  t.args = "{}";
  t.has_args = true;
  a.tool_call_chunks.push_back(t);

  auto m = finalize_chunk(a);
  EXPECT_TRUE(m.tool_calls.empty());
  ASSERT_EQ(m.invalid_tool_calls.size(), 1u);
  EXPECT_NE(m.invalid_tool_calls[0].error.find("name"), std::string::npos);
}

TEST(ChunkMerge, bad_args_goes_invalid) {
  auto a = ai_chunk();
  auto t = chunk(0);
  t.name = "f";
  t.has_name = true;
  t.args = "{oops";
  t.has_args = true;
  a.tool_call_chunks.push_back(t);

  auto m = finalize_chunk(a);
  EXPECT_TRUE(m.tool_calls.empty());
  ASSERT_EQ(m.invalid_tool_calls.size(), 1u);
  EXPECT_NE(m.invalid_tool_calls[0].error.find("parse"), std::string::npos);
}

TEST(ChunkMerge, usage_sums) {
  auto a = ai_chunk();
  a.usage = UsageMetadata{10, 5, 15, 3};
  auto b = ai_chunk();
  b.usage = UsageMetadata{2, 3, 5, 1};
  auto c = a + b;
  ASSERT_TRUE(c.usage.has_value());
  EXPECT_EQ(c.usage->input_tokens, 12);
  EXPECT_EQ(c.usage->total_tokens, 20);
}

TEST(ChunkMerge, metadata_first_nonempty_wins) {
  auto a = ai_chunk();
  a.id = "m1";
  auto b = ai_chunk();
  b.id = "m2";
  auto c = a + b;
  EXPECT_EQ(c.id, "m1");

  auto d = ai_chunk();
  auto e = ai_chunk();
  e.id = "first-id";
  auto f = d + e;
  EXPECT_EQ(f.id, "first-id");
}