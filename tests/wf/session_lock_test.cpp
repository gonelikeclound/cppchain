// tests/wf —— problems/07 §8 P0：会话单飞锁。
// 验证：第二次获取 Busy（带持有者信息）/ 释放后可再取 / stale（pid 已死或内容损坏）自动回收 /
// 不同 session 互不影响 / 只删自己 pid 的锁。
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <unistd.h>

#include "cppchain/workflow/session_lock.hpp"

using namespace cppchain::workflow;

namespace {

struct TempDir {
  std::filesystem::path dir;
  TempDir()
      : dir(std::filesystem::temp_directory_path() /
            ("wf_slock_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
};

void write_raw(std::filesystem::path const& p, std::string const& s) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream o(p, std::ios::trunc);
  o << s;
}

}  // namespace

TEST(WfSessionLock, second_acquire_is_busy) {
  TempDir tmp;
  auto a = acquire_session_lock(tmp.dir, "s1", "run-a");
  ASSERT_TRUE(a.acquired) << a.error;
  auto b = acquire_session_lock(tmp.dir, "s1", "run-b");
  EXPECT_FALSE(b.acquired);
  ASSERT_TRUE(b.holder.has_value());
  EXPECT_EQ(b.holder->pid, static_cast<long>(::getpid()));
  EXPECT_EQ(b.holder->run, "run-a");
  release_session_lock(tmp.dir, "s1");
}

TEST(WfSessionLock, release_allows_reacquire) {
  TempDir tmp;
  auto a = acquire_session_lock(tmp.dir, "s1", "run-a");
  ASSERT_TRUE(a.acquired) << a.error;
  release_session_lock(tmp.dir, "s1");
  EXPECT_FALSE(std::filesystem::exists(session_lock_path(tmp.dir, "s1")));
  auto b = acquire_session_lock(tmp.dir, "s1", "run-b");
  EXPECT_TRUE(b.acquired) << b.error;
  release_session_lock(tmp.dir, "s1");
}

TEST(WfSessionLock, dead_pid_is_reaped) {
  TempDir tmp;
  auto const lock = session_lock_path(tmp.dir, "s1");
  // 2147483647：Linux pid 上限远小于它 → kill(pid,0)=ESRCH
  write_raw(lock, R"({"pid":2147483647,"run":"ghost","started_epoch":1})");
  auto a = acquire_session_lock(tmp.dir, "s1", "run-new");
  EXPECT_TRUE(a.acquired) << a.error;
  EXPECT_TRUE(a.stale_reaped);
  auto const info = read_session_lock(tmp.dir, "s1");
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->run, "run-new");
  release_session_lock(tmp.dir, "s1");
}

TEST(WfSessionLock, malformed_lock_is_reaped) {
  TempDir tmp;
  write_raw(session_lock_path(tmp.dir, "s1"), "not-json");
  auto a = acquire_session_lock(tmp.dir, "s1", "run-new");
  EXPECT_TRUE(a.acquired) << a.error;
  EXPECT_TRUE(a.stale_reaped);
  release_session_lock(tmp.dir, "s1");
}

TEST(WfSessionLock, different_sessions_are_independent) {
  TempDir tmp;
  auto a = acquire_session_lock(tmp.dir, "s1", "run-a");
  auto b = acquire_session_lock(tmp.dir, "s2", "run-b");
  EXPECT_TRUE(a.acquired) << a.error;
  EXPECT_TRUE(b.acquired) << b.error;
  release_session_lock(tmp.dir, "s1");
  release_session_lock(tmp.dir, "s2");
}

TEST(WfSessionLock, release_only_removes_own_lock) {
  TempDir tmp;
  auto a = acquire_session_lock(tmp.dir, "s1", "run-a");
  ASSERT_TRUE(a.acquired) << a.error;
  // 模拟"锁被别人的新持有者接管"：把 pid 改成别的进程
  write_raw(session_lock_path(tmp.dir, "s1"),
            R"({"pid":2147483647,"run":"someone-else","started_epoch":1})");
  release_session_lock(tmp.dir, "s1");
  EXPECT_TRUE(std::filesystem::exists(session_lock_path(tmp.dir, "s1")));  // 不误删
  std::filesystem::remove(session_lock_path(tmp.dir, "s1"));
}

// P1：取消标记读写/清除
TEST(WfSessionLock, cancel_marker_roundtrip) {
  TempDir tmp;
  EXPECT_FALSE(read_session_cancel(tmp.dir, "s1").has_value());
  ASSERT_TRUE(write_session_cancel(tmp.dir, "s1", 12345, "run-x"));
  auto const req = read_session_cancel(tmp.dir, "s1");
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->pid, 12345);
  EXPECT_EQ(req->run, "run-x");
  clear_session_cancel(tmp.dir, "s1");
  EXPECT_FALSE(read_session_cancel(tmp.dir, "s1").has_value());
}
