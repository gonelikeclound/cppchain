// M9 测试：TruncateToolResults token 预算截断 + ArtifactToolResults 落盘 + Message.artifact round-trip
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "cppchain/agent/agent.hpp"
#include "cppchain/agent/artifact.hpp"
#include "cppchain/agent/tokens.hpp"
#include "cppchain/message/serializer.hpp"

using namespace cppchain;
using namespace cppchain::agent;
using cppchain::message::Message;

namespace {

std::string long_text() {
  std::string s;
  for (int i = 0; i < 120; ++i)
    s += "这是一段较长的中文工具结果，内容很多很占 token，编号 " + std::to_string(i) + "。";
  return s;
}

bool ends_at_utf8_boundary(std::string const& s) {
  if (s.empty()) return true;
  unsigned char const last = static_cast<unsigned char>(s.back());
  return (last & 0xC0) != 0x80;  // 末字节不是续字节 → 断在字符边界
}

std::string read_file(std::string const& path) {
  std::ifstream ifs(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(TruncateToken, budget_mode_cuts_to_budget_utf8_safe) {
  Message m = message::tool_message(long_text(), "c1");
  TruncateToolResults trunc(TruncateToolResults::Options{.max_tokens = 40});
  trunc.after_tool(m);
  std::string const out = m.text();
  EXPECT_LE(tokens::estimate(out), 40u);
  EXPECT_NE(out.find("[已按 token 预算截断"), std::string::npos);
  EXPECT_TRUE(ends_at_utf8_boundary(out));  // 不能切断中文
}

TEST(TruncateToken, exempts_short_results) {
  Message m = message::tool_message("短结果", "c1");
  TruncateToolResults trunc(TruncateToolResults::Options{.max_tokens = 40});
  trunc.after_tool(m);
  EXPECT_EQ(m.text(), "短结果");  // 预算内不动
}

TEST(Artifact, short_result_stays_inline) {
  Message m = message::tool_message("短结果", "c1");
  ArtifactToolResults art(ArtifactToolResults::Options{.dir = "./artifacts"});
  art.after_tool(m);
  EXPECT_EQ(art.artifacts_written(), 0u);
  EXPECT_FALSE(m.artifact.has_value());
  EXPECT_EQ(m.text(), "短结果");
}

TEST(Artifact, long_result_written_and_referenced) {
  auto const dir = std::filesystem::temp_directory_path() / "m9_artifact_test";
  std::filesystem::remove_all(dir);

  Message m = message::tool_message(long_text(), "call_9");
  ArtifactToolResults art(ArtifactToolResults::Options{.dir = dir.string(), .max_inline_tokens = 20});
  art.after_tool(m);

  EXPECT_EQ(art.artifacts_written(), 1u);
  ASSERT_TRUE(m.artifact.has_value());
  EXPECT_NE(m.artifact->find(dir.string()), std::string::npos);
  EXPECT_EQ(read_file(*m.artifact), long_text());            // 全文在文件里
  EXPECT_NE(m.text().find("[完整结果已落盘: "), std::string::npos);  // prompt 只放引用行
  EXPECT_LT(m.text().size(), long_text().size());

  std::filesystem::remove_all(dir);
}

TEST(Artifact, message_artifact_round_trip) {
  Message m = message::tool_message("内容", "c1");
  m.artifact = "artifacts/x.txt";
  nlohmann::json const j = m;
  Message const back = j.get<Message>();
  EXPECT_EQ(back.artifact, m.artifact);
  EXPECT_EQ(back.text(), "内容");

  // 缺省（无 artifact）序列化后仍为空
  Message m2 = message::tool_message("无落盘", "c2");
  EXPECT_FALSE(nlohmann::json(m2).get<Message>().artifact.has_value());
}