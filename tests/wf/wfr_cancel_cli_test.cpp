// tests/wf —— problems/07 §9 P1：`wfr cancel` CLI 的最小端到端（不依赖 LLM）。
// 覆盖：没有进行中的查询 → 明确提示且 exit 0；有锁 → 写 <session>.cancel（pid/run 对齐）。
// 真实"取消运行中的 wfr"需要长任务，见 problems/07 §9.2 的手工验证（CLI/SIGINT/CGI）。
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
            ("wfr_cancel_cli_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
};

std::filesystem::path wfr_bin() {
  char buf[4096];
  ssize_t const n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  std::filesystem::path exe(std::string(buf, static_cast<std::size_t>(n)));
  return exe.parent_path().parent_path() / "bin" / "wfr";  // tests/ → dev/bin/wfr
}

cppchain::tool::ExecResult run_cancel(std::filesystem::path const& ws,
                                              std::string const& session) {
  return cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "cancel", "--workdir", ws.string(), "--session", session, "--no-wait"},
      std::nullopt, {}, std::chrono::seconds(10), 1u << 20);
}

}  // namespace

TEST(WfrCancelCli, no_running_query_is_clear_noop) {
  if (!std::filesystem::exists(wfr_bin())) GTEST_SKIP() << "wfr 未构建：" << wfr_bin();
  TempDir tmp;
  auto const r = run_cancel(tmp.dir, "s1");
  ASSERT_EQ(r.exit_code, 0) << r.output;
  EXPECT_NE(r.output.find("没有进行中的查询"), std::string::npos) << r.output;
}

TEST(WfrCancelCli, writes_cancel_marker_for_lock_holder) {
  if (!std::filesystem::exists(wfr_bin())) GTEST_SKIP() << "wfr 未构建：" << wfr_bin();
  TempDir tmp;
  auto const a = acquire_session_lock(tmp.dir, "s1", "test-run");
  ASSERT_TRUE(a.acquired) << a.error;

  auto const r = run_cancel(tmp.dir, "s1");
  ASSERT_EQ(r.exit_code, 0) << r.output;
  EXPECT_NE(r.output.find("已请求取消"), std::string::npos) << r.output;

  auto const req = read_session_cancel(tmp.dir, "s1");
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->pid, static_cast<long>(::getpid()));  // 标记目标 = 锁持有者
  EXPECT_EQ(req->run, "test-run");

  clear_session_cancel(tmp.dir, "s1");
  release_session_lock(tmp.dir, "s1");
}
