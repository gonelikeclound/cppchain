// cppchain —— src/tool/sandbox.cpp
// SandboxManager（wfr-㊸ @③）：provider 选择器 + bwrap 沙箱执行器。
// 探针纪律：bwrap 可用 ≠ 二进制存在 —— 必须能**实际建 namespace**（无 user namespace
// 的宿主（如受限容器内）bwrap 会死在 "setting up uid map"）；不可用即自隐退，
// 绝不让带沙箱标志的命令裸跑失败还装作成功。无任何可用 provider → nullptr（fail-closed）。
#include "cppchain/tool/sandbox.hpp"

#include <cstdlib>
#include <string>

#include "cppchain/tool/spawn_runner.hpp"

namespace cppchain::tool {
namespace {

std::string const kPathDelimiter =
#ifdef _WIN32
    ";";
#else
    ":";
#endif

// 在 PATH + 常用目录里找二进制；找不到返回空串
std::string find_binary(std::string const& name) {
  std::vector<std::string> candidates;
  if (char const* p = std::getenv("PATH")) {
    std::string const path(p);
    std::size_t pos = 0;
    while (pos <= path.size()) {
      std::size_t const end = path.find(kPathDelimiter, pos);
      std::string const dir = path.substr(pos, end == std::string::npos ? std::string::npos
                                                                        : end - pos);
      if (!dir.empty()) candidates.push_back(dir + "/" + name);
      if (end == std::string::npos) break;
      pos = end + 1;
    }
  }
  candidates.push_back("/usr/bin/" + name);
  candidates.push_back("/usr/local/bin/" + name);
  candidates.push_back("/bin/" + name);
  for (auto const& c : candidates) {
    if (std::filesystem::exists(c)) return c;
  }
  return {};
}

// bwrap 沙箱执行器：把命令包进 bwrap 的 namespace 里跑。读/超时/截断复用共享内核。
class BwrapExecutor final : public ToolExecutor {
 public:
  explicit BwrapExecutor(std::string bwrap, SandboxConfig const& cfg)
      : bwrap_(std::move(bwrap)), cfg_(cfg) {}

  ExecResult run(std::string const& command,
                 std::optional<std::chrono::milliseconds> timeout) const override {
    std::vector<std::string> argv = SandboxManager::bwrap_argv(cfg_, command);
    argv.insert(argv.begin(), bwrap_);
    // cwd/限额交给 bwrap（--chdir/--bind/tmpfs/禁网）；外层子进程只做进程组+管道
    return detail::run_argv(argv, std::nullopt, detail::SpawnLimits{}, timeout, cfg_.max_output);
  }

 private:
  std::string bwrap_;
  SandboxConfig cfg_;
};

}  // namespace

bool SandboxManager::bwrap_usable() {
  std::string const bwrap = find_binary("bwrap");
  if (bwrap.empty()) return false;
  SandboxConfig probe_cfg;
  // 只需要一次能建 namespace 的探针：跑 true；超时 10s 封顶
  std::vector<std::string> argv = bwrap_argv(probe_cfg, "true");
  argv.insert(argv.begin(), bwrap);
  ExecResult const r = detail::run_argv(argv, std::nullopt, detail::SpawnLimits{},
                                        std::chrono::seconds(10), 64);
  return r.exit_code == 0;
}

std::vector<std::string> SandboxManager::bwrap_argv(SandboxConfig const& cfg,
                                                    std::string const& command) {
  std::string const ws = cfg.workspace.empty() ? "/" : cfg.workspace.string();
  return {"--unshare-all",             // 禁网 + 新 pid/ipc/uts + 尽力 user ns
          "--die-with-parent",         // 父进程死 → 沙箱整体退出（防孤儿悬留）
          "--ro-bind", "/", "/",       // 根只读
          "--dev", "/dev",             // 设备（壳要 /dev/null 等）
          "--proc", "/proc",           // 新 pid ns 的 proc
          "--tmpfs", "/tmp",           // /tmp 独立 tmpfs
          "--bind", ws, ws,            // workspace 可写（顺序在 ro-bind 之后 → 覆盖）
          "--clearenv",                // 不泄漏宿主环境
          "--setenv", "PATH",
          "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
          "--setenv", "HOME", "/tmp", "--setenv", "TMPDIR", "/tmp",
          "--chdir", ws,               // 工作目录 = workspace
          "/bin/sh", "-c", command};
}

std::shared_ptr<ToolExecutor> SandboxManager::make_subprocess(
    std::filesystem::path const& ws, std::size_t max_output) {
  return make_restricted_spawn_executor(ws, max_output);
}

std::shared_ptr<ToolExecutor> SandboxManager::resolve(SandboxConfig const& cfg) {
  std::vector<std::string> const order =
      cfg.providers.empty() ? std::vector<std::string>{"bwrap", "subprocess"} : cfg.providers;
  for (std::string const& p : order) {
    if (p == "bwrap") {
      if (bwrap_usable()) {
        provider_ = "bwrap";
        return std::make_shared<BwrapExecutor>(find_binary("bwrap"), cfg);
      }
      continue;  // 二进制缺失或 namespace 建不了 → 自隐退，试下一个
    }
    if (p == "subprocess") {
      provider_ = "subprocess";
      return make_subprocess(cfg.workspace, cfg.max_output);
    }
    if (p == "container") {
      // 预留：容器 provider 需 container_image + 运行时（docker/podman），未实现前跳过。
      continue;
    }
    // 未知 provider 名 → 跳过（fail-closed 的含义是"没解析到就拒绝"，不是宇面上拒绝）
  }
  return nullptr;
}

// 初始化顺序陷阱：`provider_` 必须在 `exec_` 之前已构造，resolve() 里才写得到它。
// 因此不能放初始值列表（那里先构造 exec_、provider_ 还没构造，赋值是 UB）—— 放函数体。
SandboxManager::SandboxManager(SandboxConfig cfg) {
  exec_ = resolve(std::move(cfg));
}

std::string SandboxManager::describe() const {
  if (provider_ == "bwrap")
    return "bwrap 沙箱（unshare-all 禁网 · 根只读 · workspace 可写 · /tmp tmpfs 隔离）";
  if (provider_ == "subprocess")
    return "受限子进程（**非真沙箱**：文件系统/网络无约束，仅 chdir workspace + setrlimit + 超时/输出封顶）";
  return "无可用 sandbox provider";
}

}  // namespace cppchain::tool