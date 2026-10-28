// tests/wf —— problems/07 §9 P1：Web(CGI) 的取消入口（不需浏览器/LLM）。
// 直接以 CGI 方式调用 ?action=cancel：无锁 → cancelled:false；有锁 → 写 <session>.cancel。
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

#include "cppchain/tool/spawn_runner.hpp"
#include "cppchain/workflow/session_lock.hpp"

using namespace cppchain::workflow;

namespace {

struct TempDir {
  std::filesystem::path dir;
  TempDir()
      : dir(std::filesystem::temp_directory_path() /
            ("wfr_cgi_cancel_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
};

std::filesystem::path self_exe() {
  char buf[4096];
  ssize_t const n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  return std::filesystem::path(std::string(buf, static_cast<std::size_t>(n)));
}
std::filesystem::path wfr_cgi_bin() { return self_exe().parent_path().parent_path() / "bin" / "wfr_cgi"; }
std::filesystem::path repo_root() {
  return self_exe().parent_path().parent_path().parent_path().parent_path();
}

cppchain::tool::ExecResult run_cgi_cancel(std::filesystem::path const& ws,
                                          std::string const& session) {
  std::string const qs = "QUERY_STRING=action=cancel&ws=" + ws.string() + "&s=" + session;
  return cppchain::tool::detail::run_argv({"/usr/bin/env", qs, wfr_cgi_bin().string()},
                                          repo_root(), {}, std::chrono::seconds(10), 1u << 20);
}

}  // namespace

TEST(WfrCgiCancel, no_running_query_reports_not_cancelled) {
  if (!std::filesystem::exists(wfr_cgi_bin())) GTEST_SKIP() << "wfr_cgi 未构建";
  TempDir tmp;
  auto const r = run_cgi_cancel(tmp.dir, "s1");
  ASSERT_EQ(r.exit_code, 0) << r.output;
  EXPECT_NE(r.output.find("\"cancelled\":false"), std::string::npos) << r.output;
  EXPECT_NE(r.output.find("没有进行中的查询"), std::string::npos) << r.output;
}

TEST(WfrCgiCancel, writes_marker_for_lock_holder) {
  if (!std::filesystem::exists(wfr_cgi_bin())) GTEST_SKIP() << "wfr_cgi 未构建";
  TempDir tmp;
  auto const a = acquire_session_lock(tmp.dir, "s1", "cgi-test-run");
  ASSERT_TRUE(a.acquired) << a.error;

  auto const r = run_cgi_cancel(tmp.dir, "s1");
  ASSERT_EQ(r.exit_code, 0) << r.output;
  EXPECT_NE(r.output.find("\"cancelled\":true"), std::string::npos) << r.output;

  auto const req = read_session_cancel(tmp.dir, "s1");
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->pid, static_cast<long>(::getpid()));
  EXPECT_EQ(req->run, "cgi-test-run");

  clear_session_cancel(tmp.dir, "s1");
  release_session_lock(tmp.dir, "s1");
}
