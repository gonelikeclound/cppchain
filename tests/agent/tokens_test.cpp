// M9 测试：token 估算（无 tokenizer 启发式）
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "cppchain/agent/tokens.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;

namespace {

std::size_t t(std::string_view s) { return tokens::estimate(s); }

}  // namespace

TEST(Tokens, empty_is_zero) { EXPECT_EQ(t(""), 0u); }

TEST(Tokens, ascii_roughly_four_chars_per_token) {
  std::string const s(20, 'a');             // 20 ascii → ≈5 tokens
  EXPECT_GE(t(s), 4u);
  EXPECT_LE(t(s), 7u);
}

TEST(Tokens, cjk_counts_one_per_char) {
  std::string const s = "天气查询天气查询";  // 8 个中文字
  EXPECT_GE(t(s), 7u);
  EXPECT_LE(t(s), 10u);
}

TEST(Tokens, mixed_roughly_sums) {
  std::string const en(16, 'x');            // ≈4 tokens
  std::string const zh = "中文字";            // 3 tokens
  EXPECT_LE(t(en + zh), t(en) + t(zh) + 2u);
  EXPECT_GE(t(en + zh), t(en) + t(zh));
}

// 截断二分回退的前提：estimate 对前缀长度非降（含切在多字节字符中部的脏前缀）
TEST(Tokens, monotonic_over_prefixes) {
  std::string const s = "你好 world 世界 abcdefghij";
  std::size_t prev = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    std::size_t const cur = t(std::string_view(s).substr(0, i));
    EXPECT_GE(cur, prev) << "前缀 " << i << " 处单调性被破坏";
    prev = cur;
  }
}

TEST(Tokens, messages_adds_per_message_overhead) {
  std::string const text = "一段文本";
  EXPECT_GE(tokens::estimate_msgs({message::human_message(text)}), t(text) + 4u);
  // 带 usage 的消息直接采用实测值（避免双计）
  Message m = message::ai_message("不管多长的文本，有 usage 就以 usage 为准");
  m.usage = message::UsageMetadata{10, 5, 15, 0};
  EXPECT_EQ(tokens::estimate_msgs({m}), 4u + 15u);
}