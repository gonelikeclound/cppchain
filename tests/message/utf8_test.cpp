// tests/message —— utf8 容错（wfr-㉗）：非法 UTF-8 净化 + 安全 dump 不抛、坏字节换 U+FFFD
#include <gtest/gtest.h>

#include "cppchain/message/utf8.hpp"

using namespace cppchain::message;

TEST(Utf8, sanitize_replaces_invalid_bytes) {
  std::string s = "abc\xE2\x80";  // 0xE2 开头但只有 1 个续字节（需 2 个）
  sanitize_utf8(s);
  EXPECT_EQ(s.find('\xE2'), std::string::npos);
  EXPECT_NE(s.find("\xEF\xBF\xBD"), std::string::npos) << "应替换为 U+FFFD";
}

TEST(Utf8, sanitize_keeps_valid_cjk) {
  std::string s = "天气晴";  // 合法 3 字节 UTF-8
  sanitize_utf8(s);
  EXPECT_EQ(s, "天气晴");
  // 前导非法字节（0xFF 用于合法头字节）不误伤合法多字节：混入一个孤立续字节
  std::string m = "晴\x80晴";
  sanitize_utf8(m);
  EXPECT_EQ(m.size(), std::string("晴").size() * 2 + std::string("\xEF\xBF\xBD").size());
}

TEST(Utf8, safe_dump_never_throws) {
  nlohmann::json j;
  j["a"] = std::string("x\xE2");
  j["b"] = nlohmann::json::array({"好的", std::string("y\x80")});
  std::string const out = utf8_safe_dump(j);  // 不抛
  EXPECT_NE(out.find("\xEF\xBF\xBD"), std::string::npos) << "坏字节应替换为 U+FFFD";
}
