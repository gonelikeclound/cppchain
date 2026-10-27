// cppchain —— workflow/session_lock.hpp
// 会话单飞锁（problems/07 §8 P0）：同一 session 同一时刻只允许一轮 query。
//
// 背景：wfr 一次调用 = 一个进程同步跑完；此前同一会话并发两次会同时读写
// `<ws>/.wfr/sessions/session-<name>.log.jsonl` —— 两个进程的 seq 各自从旧文件
// 最大值续编，可能产生**重复 seq + 行交织**，replay 顺序不再确定。
//
// 语义：
//   · 锁文件 `<ws>/.wfr/sessions/session-<name>.lock`，内容 {pid, run, started_epoch}；
//   · **原子创建**（临时文件写全内容 → link，link 已存在即失败）——不会出现"空锁"半写；
//   · 持有者 pid 已死（kill(pid,0) = ESRCH）或锁内容损坏 → 视为 stale，自动回收重试；
//   · 释放只由**持有者**执行（pid 不匹配不动别人的锁）。
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace cppchain::workflow {

struct SessionLockInfo {
  long pid = 0;
  std::string run;             // 持有者自报的标识（wfr 用 "cli-<pid>"）
  long started_epoch = 0;      // 获取时间（epoch 秒）
};

struct SessionLockAcquire {
  bool acquired = false;
  bool stale_reaped = false;               // 回收过 stale 锁（观测/测试用）
  std::optional<SessionLockInfo> holder;   // Busy 时的持有者信息
  std::string error;                       // I/O 等硬错误（非 Busy）
};

[[nodiscard]] std::filesystem::path session_lock_path(std::filesystem::path const& workspace,
                                                      std::string const& session);

// 尝试获取（非阻塞）。Busy → acquired=false + holder；硬错误 → error 非空。
[[nodiscard]] SessionLockAcquire acquire_session_lock(std::filesystem::path const& workspace,
                                                      std::string const& session,
                                                      std::string const& run);

// 释放：仅当锁内容里的 pid == 本进程 pid；否则不动（防误删新持有者）。
void release_session_lock(std::filesystem::path const& workspace, std::string const& session);

// 读取当前锁（无锁/损坏 → nullopt）。
[[nodiscard]] std::optional<SessionLockInfo> read_session_lock(
    std::filesystem::path const& workspace, std::string const& session);

}  // namespace cppchain::workflow
