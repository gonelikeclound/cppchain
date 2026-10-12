// cppchain —— src/tool/spawn_executor.cpp
// PlainSpawnExecutor：wfr-㊸ @① 的执行器底线形态，@③ 起委托共享内核 spawn_runner。
// ⚠️ 定位：只做**进程级**隔离（独立进程组、stdin=/dev/null、输出封顶、超时强杀），
// **不是沙箱**（文件系统/网络无约束）。批③ 由 SandboxManager 选择更强的 provider
// （bwrap），本类保留为「受限子进程 provider」的底线；两者共用同一个 fork/exec 内核。
#include "cppchain/tool/executor.hpp"
#include "cppchain/tool/spawn_runner.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace cppchain::tool {
namespace {

class PlainSpawnExecutor final : public ToolExecutor {
 public:
  PlainSpawnExecutor(std::optional<std::filesystem::path> cwd,
                     detail::SpawnLimits limits, std::size_t max_output)
      : cwd_(std::move(cwd)), limits_(limits), max_output_(max_output) {}

  ExecResult run(std::string const& command,
                 std::optional<std::chrono::milliseconds> timeout) const override {
    std::vector<std::string> const argv{"/bin/sh", "-c", command};
    return detail::run_argv(argv, cwd_, limits_, timeout, max_output_);
  }

 private:
  std::optional<std::filesystem::path> cwd_;
  detail::SpawnLimits limits_;
  std::size_t max_output_;
};

}  // namespace

// 批① 的默认执行器：进程级隔离（非沙箱），工作目录与限额保持旧行为不变
[[nodiscard]] std::shared_ptr<ToolExecutor> make_plain_spawn_executor(std::size_t max_output) {
  return std::make_shared<PlainSpawnExecutor>(std::nullopt, detail::SpawnLimits{}, max_output);
}

// 批③ 的「受限子进程 provider」：沙箱不可用时的底线 —— chdir 到工作区 + 资源限额，
// 显式标注为非真沙箱（SandboxManager::describe 会原样告诉宿主/模型）。
[[nodiscard]] std::shared_ptr<ToolExecutor> make_restricted_spawn_executor(
    std::filesystem::path const& cwd, std::size_t max_output) {
  detail::SpawnLimits limits;
  limits.fsize = true;
  limits.nofile = true;
  return std::make_shared<PlainSpawnExecutor>(cwd, limits, max_output);
}

}  // namespace cppchain::tool