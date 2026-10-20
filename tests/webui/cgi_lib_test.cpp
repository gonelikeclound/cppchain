// wfr-🈟 CGI WebUI 增强 · 纯逻辑单测（cgi_lib.hpp 的 header-only 函数）。
// 覆盖：session 名解析（wfname.sessionid）/ workspace 名/目录校验 / workspaces.json 读写 /
// per-workflow --set 映射 / workflow 枚举与白名单。
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "wfr_cgi_lib.hpp"

using namespace cppchain::cgi;

namespace {

std::filesystem::path tmp_dir(std::string const& tag) {
  auto p = std::filesystem::temp_directory_path() /
           ("webui_" + tag + "_" + std::to_string(static_cast<unsigned>(std::random_device{}())));
  std::filesystem::create_directories(p);
  return p;
}

void write_json(std::filesystem::path const& p, std::string const& content) {
  std::ofstream(p) << content;
}

}  // namespace

// ---- parse_session_id ----
TEST(CgiLib, parse_session_id_ok) {
  auto s = parse_session_id("chat_cgi.s123");
  ASSERT_TRUE(s);
  EXPECT_EQ(s->workflow, "chat_cgi");
  EXPECT_EQ(s->id, "s123");
}
TEST(CgiLib, parse_session_id_legacy_no_dot_rejected) {
  EXPECT_FALSE(parse_session_id("s1").has_value());   // 遗留无名会话
  EXPECT_FALSE(parse_session_id("").has_value());
  EXPECT_FALSE(parse_session_id("chat_cgi.").has_value());  // 空 id
  EXPECT_FALSE(parse_session_id(".s1").has_value());        // 空 workflow
  EXPECT_FALSE(parse_session_id("a.b.c").has_value());      // id 含点
  EXPECT_FALSE(parse_session_id("a/b.s1").has_value());     // workflow 含斜杠
  EXPECT_FALSE(parse_session_id("a b.s1").has_value());     // 空格
}

// ---- is_registered_workflow ----
TEST(CgiLib, is_registered_workflow) {
  auto dir = tmp_dir("wf");
  write_json(dir / "a.json", "{}");
  write_json(dir / "b.json", "{}");
  std::filesystem::path const none = tmp_dir("none");
  EXPECT_TRUE(is_registered_workflow({dir}, "a"));
  EXPECT_TRUE(is_registered_workflow({dir}, "b"));
  EXPECT_FALSE(is_registered_workflow({dir}, "c"));
  EXPECT_FALSE(is_registered_workflow({none}, "a"));  // 目录不存在
  EXPECT_FALSE(is_registered_workflow({}, "a"));
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(none);
}

// ---- valid_workspace_name / sanitize_workspace_dir ----
TEST(CgiLib, valid_workspace_name) {
  EXPECT_TRUE(valid_workspace_name("demo"));
  EXPECT_TRUE(valid_workspace_name("demo-2"));
  EXPECT_TRUE(valid_workspace_name("A_b"));
  EXPECT_FALSE(valid_workspace_name(""));
  EXPECT_TRUE(valid_workspace_name("1x"));  // 字母数字开头均可
  EXPECT_FALSE(valid_workspace_name("-x"));  // 首字符须字母数字
  EXPECT_FALSE(valid_workspace_name("a b"));
  EXPECT_FALSE(valid_workspace_name("a/b"));
  EXPECT_FALSE(valid_workspace_name(std::string(33, 'a')));
}

TEST(CgiLib, sanitize_workspace_dir) {
  std::filesystem::path const root = tmp_dir("root");
  EXPECT_TRUE(sanitize_workspace_dir(root, "tmp/x").has_value());
  EXPECT_TRUE(sanitize_workspace_dir(root, "tmp/a/b").has_value());
  EXPECT_FALSE(sanitize_workspace_dir(root, "").has_value());
  EXPECT_FALSE(sanitize_workspace_dir(root, "/abs").has_value());  // 绝对路径
  EXPECT_FALSE(sanitize_workspace_dir(root, "../evil").has_value());
  EXPECT_FALSE(sanitize_workspace_dir(root, "a/../..").has_value());
  std::filesystem::remove_all(root);
}

// ---- read_workspaces_file / append_workspace ----
TEST(CgiLib, read_workspaces_file_bad_json_is_empty) {
  auto dir = tmp_dir("wsfile");
  auto cfg = dir / "workspaces.json";
  EXPECT_TRUE(read_workspaces_file(cfg).empty());       // 缺文件
  write_json(cfg, "not json{{");
  EXPECT_TRUE(read_workspaces_file(cfg).empty());       // 坏 JSON 不抛
  write_json(cfg, R"({"workspaces":[{"name":"a","dir":"d1"}]})");
  auto r = read_workspaces_file(cfg);
  ASSERT_TRUE(r.is_array());
  EXPECT_EQ(r.size(), 1);
  std::filesystem::remove_all(dir);
}

TEST(CgiLib, append_workspace_dedup_and_persist) {
  auto dir = tmp_dir("append");
  auto cfg = dir / "workspaces.json";
  EXPECT_TRUE(append_workspace(cfg, "demo", "tmp/cgi_ws/demo"));
  EXPECT_TRUE(append_workspace(cfg, "demo2", "tmp/cgi_ws/demo2"));
  // 重名 / 重目录 → 拒绝
  EXPECT_FALSE(append_workspace(cfg, "demo", "other"));
  EXPECT_FALSE(append_workspace(cfg, "other", "tmp/cgi_ws/demo"));
  auto r = read_workspaces_file(cfg);
  ASSERT_TRUE(r.is_array());
  EXPECT_EQ(r.size(), 2);
  EXPECT_EQ(r[0].value("name", ""), "demo");
  EXPECT_EQ(r[1].value("dir", ""), "tmp/cgi_ws/demo2");
  std::filesystem::remove_all(dir);
}

// ---- default_sets_for ----
TEST(CgiLib, default_sets_for) {
  auto chat = default_sets_for("chat_cgi", "hi");
  EXPECT_EQ(chat.size(), 3u);  // compress 三件
  bool has_when = false;
  for (auto const& [k, v] : chat) if (k == "compress_when") has_when = (v == "messages");
  EXPECT_TRUE(has_when);

  auto mch = default_sets_for("mch", "查天气出报告");
  ASSERT_EQ(mch.size(), 2u);
  EXPECT_EQ(mch[0].first, "goal"); EXPECT_EQ(mch[0].second, "查天气出报告");  // from_query
  auto mch_par = default_sets_for("mch_par", "x");
  EXPECT_EQ(mch_par.size(), 2u);

  auto lot = default_sets_for("lot", "daily");
  ASSERT_EQ(lot.size(), 2u);
  EXPECT_EQ(lot[0].first, "task"); EXPECT_EQ(lot[0].second, "daily");

  EXPECT_TRUE(default_sets_for("unknown_wf", "q").empty());  // 安全默认
  EXPECT_TRUE(default_sets_for("chat", "q").empty());        // 内置 chat 无 @config 键
}

// ---- enumerate_workflow_names ----
TEST(CgiLib, enumerate_workflow_names_dedup_and_sort) {
  auto d1 = tmp_dir("enum1");
  auto d2 = tmp_dir("enum2");
  write_json(d1 / "b.json", "{}");
  write_json(d1 / "a.json", "{}");
  write_json(d1 / "b.json", "{}");  // 重复文件不影响（同名目录条目去重）
  write_json(d2 / "a.json", "{}");  // 与 d1 同名 → 去重（首个 win）
  write_json(d2 / "c.json", "{}");
  auto names = enumerate_workflow_names({d1, d2});
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[0], "a");  // 有序
  EXPECT_EQ(names[1], "b");
  EXPECT_EQ(names[2], "c");
  std::filesystem::remove_all(d1);
  std::filesystem::remove_all(d2);
}
