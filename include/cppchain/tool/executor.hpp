// cppchain —— tool/executor.hpp
// 命令类工具的执行器抽象（wfr-㊸ @①，L4 沙箱的前置接口）。
// 铁律：bash 等命令工具**绝不直接 system() 跑命令**，一律经 ToolExecutor 注入后执行。
//   - 批① 提供 PlainSpawnExecutor（fork/exec 子进程 + 进程组超时强杀）：
//     只做进程级隔离，**不是沙箱** —— 显式定位为"受限子进程 provider"的底线形态。
//   - 批③ 由 SandboxManager（bwrap/容器 provider）替换，接口不变。
// 消费方：ExecutionGuard::run 把 ToolSpec::executor 注入 ToolContext::executor，
//         命令工具的 handler 只认 ctx.executor，无执行器时 fail-closed 拒跑。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace cppchain::tool {

// 执行结果（stdout 与 stderr 合并，顺序保留）
struct ExecResult {
  int exit_code = 0;      // -1 = 启动/等待级失败（见 note）
  std::string output;     // stdout+stderr 合并文本
  bool timed_out = false; // 超时被强杀（进程组 SIGKILL）
  std::string note;       // 失败/截断说明（人读；正常时空串）
};

class ToolExecutor {
 public:
  virtual ~ToolExecutor() = default;

  // 执行 command（经执行器后端）；timeout 为空 = 不限时（危险，建议调用方总给默认值）。
  [[nodiscard]] virtual ExecResult run(std::string const& command,
                                       std::optional<std::chrono::milliseconds> timeout) const = 0;
};

// 批① 的底线执行器工厂：fork/exec `/bin/sh -c` + 进程组超时强杀 + 输出封顶。
// 进程级隔离，**不是沙箱** —— 正式部署由批③ 的 SandboxManager provider 替换（接口不变）。
[[nodiscard]] std::shared_ptr<ToolExecutor> make_plain_spawn_executor(
    std::size_t max_output = 1u << 20);

// 批③ 的「受限子进程 provider」：chdir 到 cwd（工作区）+ setrlimit（FSIZE/NOFILE）。
// 仍是**非真沙箱**（文件系统/网络无约束），仅兜底；由 SandboxManager 在无更强 provider 时选用，
// describe() 会显式标注 "非真沙箱"，绝不静默冒充沙箱。
[[nodiscard]] std::shared_ptr<ToolExecutor> make_restricted_spawn_executor(
    std::filesystem::path const& cwd, std::size_t max_output = 1u << 20);

}  // namespace cppchain::tool