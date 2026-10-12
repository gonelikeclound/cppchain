// cppchain —— tool/sandbox.hpp
// SandboxManager（wfr-㊸ @③，L4）：命令类工具的沙箱执行器选择器。
// provider 可插拔，按配置顺序解析第一个**可用**的执行器；全不可用 → nullptr
// （fail-closed：调用方必须拒绝执行，绝不静默裸奔 —— Codex 同纪律）。
//
// providers 词表：
//   "bwrap"     —— bubblewrap（Linux，不需要 root），`--unshare-all` 禁网 + `/` 只读 +
//                  workspace 可写 + /tmp tmpfs + `--die-with-parent`。usable 判据：
//                  二进制存在 **且** 能实际建 namespace（本机无 user namespace 时自隐退）。
//   "subprocess"—— 受限子进程 provider（chdir workspace + setrlimit + 超时强杀），
//                  显式标注「非真沙箱」，仅兜底。
//   "container" —— docker/podman 预留（需 container_image 配置；未配 = 跳过）。
// providers 为空 = 自动（bwrap → subprocess）。
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cppchain/tool/executor.hpp"

namespace cppchain::tool {

struct SandboxConfig {
  std::filesystem::path workspace;               // 沙箱内可写的根（子进程 cwd / bwrap rw-bind）
  std::vector<std::string> providers;            // 候选名（见头注释）；空 = 自动
  std::string container_image;                   // container provider 用（缺省空 = 该 provider 跳过）
  std::size_t max_output = 1u << 20;             // 透传给执行器的输出封顶
};

class SandboxManager {
 public:
  // 解析第一个可用 provider；结果可能为空（无可用 provider → 调用方 fail-closed）。
  explicit SandboxManager(SandboxConfig cfg);

  [[nodiscard]] bool available() const { return exec_ != nullptr; }
  [[nodiscard]] ToolExecutor const* executor() const { return exec_.get(); }
  // 选中 provider 的人读描述（"bwrap 沙箱（…）" / "受限子进程（非真沙箱）" / "无可用 provider"）
  [[nodiscard]] std::string describe() const;
  // 当前选中 provider 名（"bwrap" / "subprocess" / ""）
  [[nodiscard]] std::string provider_name() const { return provider_; }

  // ---- 测试友好件 ----
  // bwrap 是否可用：二进制存在 + 能实际建 namespace（快速探针）。
  [[nodiscard]] static bool bwrap_usable();
  // bwrap 的完整 argv（flags + 命令）。纯函数，测试断言沙箱声明。
  [[nodiscard]] static std::vector<std::string> bwrap_argv(SandboxConfig const& cfg,
                                                           std::string const& command);
  // 子进程 provider 的执行器（供 wfr 直接构造；SandboxManager 内部经 make_restricted_spawn_executor）
  [[nodiscard]] static std::shared_ptr<ToolExecutor> make_subprocess(std::filesystem::path const& ws,
                                                                     std::size_t max_output);

 private:
  [[nodiscard]] std::shared_ptr<ToolExecutor> resolve(SandboxConfig const& cfg);

  std::shared_ptr<ToolExecutor> exec_;
  std::string provider_;  // 选中名（"" = 无）
};

}  // namespace cppchain::tool