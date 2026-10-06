// wfr 查找约定测试（wfr_lookup）：
// settings.workflow_dirs → ws → 全局(~/.wfr) → 发布自带(extra) 的顺序；
// settings 相对路径按 ws 解析；settings 只追加、不能改 ws（ws 不可覆盖）。
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "cppchain/workflow/wfr.hpp"
#include "cppchain/workflow/wfr_lookup.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

struct TempBox {
  std::filesystem::path dir;
  std::string old_home;
  TempBox() {
    dir = std::filesystem::temp_directory_path() /
          ("cppchain-wfr-" + std::to_string(::getpid()) + "-" +
           std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", (dir / "home").c_str(), 1);
  }
  ~TempBox() {
    if (!old_home.empty()) setenv("HOME", old_home.c_str(), 1);
    std::filesystem::remove_all(dir);
  }
  [[nodiscard]] std::string ws() const { return (dir / "ws").string(); }
  [[nodiscard]] std::string home_wf() const { return (dir / "home" / ".wfr" / "workflows").string(); }
};

void write_file(std::string const& path, std::string content = "{}") {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream o(path, std::ios::trunc);
  o << content;
}

}  // namespace

// 默认（无 settings、无 ws、无 home）→ 发布自带兜底
TEST(WfrLookup, share_fallback) {
  TempBox b;
  write_file(b.dir / "share" / "chat.json");
  auto found = wfr_lookup_workflow(b.ws(), "chat", Json::object(), {b.dir / "share"});
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->ends_with("share/chat.json"));
}

// ws 命中在全局/发布之前
TEST(WfrLookup, workspace_wins_over_global_and_share) {
  TempBox b;
  write_file(b.ws() + "/.wfr/workflows/x.json");
  write_file(b.home_wf() + "/x.json");
  write_file(b.dir / "share" / "x.json");
  auto found = wfr_lookup_workflow(b.ws(), "x", Json::object(), {b.dir / "share"});
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->find("/ws/.wfr/workflows/x.json") != std::string::npos);
}

// 全局兜底（ws 无）
TEST(WfrLookup, global_fallback) {
  TempBox b;
  write_file(b.home_wf() + "/g.json");
  std::string cur;
  auto found = wfr_lookup_workflow(b.ws(), "g", Json::object(), {b.dir / "share"});
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->find("/home/.wfr/workflows/g.json") != std::string::npos);
}

// settings.workflow_dirs 排最前（相对路径按 ws 解析）
TEST(WfrLookup, settings_dirs_take_precedence) {
  TempBox b;
  write_file(b.dir / "extra" / "e.json");
  write_file(b.ws() + "/.wfr/workflows/e.json");   // ws 也有——settings 目录应赢
  write_file(b.home_wf() + "/e.json");
  Json settings = {{"workflow_dirs", Json::array({"../extra"})}};  // 相对 ws → b.dir/extra
  auto found = wfr_lookup_workflow(b.ws(), "e", settings, {b.dir / "share"});
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->find("/extra/e.json") != std::string::npos);
}

// settings 相对路径是相对于 workspace（不是 cwd）
TEST(WfrLookup, settings_relative_resolved_against_workspace) {
  TempBox b;
  write_file(b.ws() + "/layered/r.json");  // ws 下的相对《./layered》
  Json settings = {{"workflow_dirs", Json::array({"./layered"})}};
  auto found = wfr_lookup_workflow(b.ws(), "r", settings, {});
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->find("/ws/layered/r.json") != std::string::npos);
}

// 找不到 → nullopt（不误报）
TEST(WfrLookup, missing_returns_nullopt) {
  TempBox b;
  EXPECT_FALSE(wfr_lookup_workflow(b.ws(), "ghost", Json::object(), {b.dir / "share"}).has_value());
}

// 坏 settings.json → 空对象（不阻断，走默认 ws/global/share）
TEST(WfrLookup, corrupt_settings_is_fail_open) {
  TempBox b;
  write_file(b.ws() + "/.wfr/settings.json", "{{{ malformed");
  EXPECT_TRUE(wfr_read_settings(b.ws()).empty());
  write_file(b.home_wf() + "/ok.json");
  auto found = wfr_lookup_workflow(b.ws(), "ok", wfr_read_settings(b.ws()), {});
  ASSERT_TRUE(found);
}

// wfr_list_workflows_in 只收 *.json
TEST(WfrLookup, list_workflows_in_dir) {
  TempBox b;
  write_file(b.dir / "d" / "a.json");
  write_file(b.dir / "d" / "b.json");
  write_file(b.dir / "d" / "skip.txt");
  auto names = wfr_list_workflows_in((b.dir / "d").string());
  ASSERT_EQ(names.size(), 2u);
  EXPECT_EQ(names[0], "a");
  EXPECT_EQ(names[1], "b");
}