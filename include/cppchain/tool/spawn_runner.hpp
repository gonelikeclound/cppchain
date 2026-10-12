// cppchain —— tool/spawn_runner.hpp
// 命令子进程执行的**共享内核**（wfr-㊸ @③ L4/L5）：fork/exec 任意 argv + 新进程组 +
// 非阻塞读 + 轮询回收 + 超时进程组 SIGKILL + 输出封顶（截断标记）。
// PlainSpawnExecutor（受限子进程 provider）与 BwrapExecutor（沙箱 provider）两个实现共用。
// ⚠️ 内部实现细节，非用户 API（公开头只因两个 .cpp 都要用）。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cppchain/tool/executor.hpp"  // ExecResult / ToolExecutor

namespace cppchain::tool::detail {

// 受限子进程 provider 的资源限额（setrlimit 子集；默认全关 = 与旧 PlainSpawn 行为一致）
struct SpawnLimits {
  bool fsize = false;    // RLIMIT_FSIZE = 256MB（限单次写，防爆盘）
  bool nofile = false;   // RLIMIT_NOFILE = 1024（限 fd，防资源耗尽）
};

// fork/exec argv（子进程新进程组；cwd 非空则 chdir；limits 在子进程 setrlimit）。
// argv[0] 必须可解析（execl 语义）；返回与 ToolExecutor::run 同形：
//   exit_code=-1 = 启动/等待级失败；timed_out = 超时进程组 SIGKILL；
//   output 超出 max_output 时保留前缀并置 note（截断标记）。
[[nodiscard]] ExecResult run_argv(std::vector<std::string> const& argv,
                                  std::optional<std::filesystem::path> const& cwd,
                                  SpawnLimits const& limits,
                                  std::optional<std::chrono::milliseconds> timeout,
                                  std::size_t max_output);

}  // namespace cppchain::tool::detail