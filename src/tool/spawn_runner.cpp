// cppchain —— src/tool/spawn_runner.cpp
// 共享子进程执行内核（wfr-㊸ @③）：从旧 PlainSpawnExecutor::run 原样抽出，
// 行为保持（fork/exec + 进程组超时强杀 + 输出封顶 + stdout/stderr 合流），
// 另加两项 provider 能力：cwd（chdir workspace）与 setrlimit 资源限额。
#include "cppchain/tool/spawn_runner.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

namespace cppchain::tool::detail {
namespace {

// 非阻塞读一段；返回 >0 实际读取、0 无数据可读（EAGAIN/EINTR）、-1 真错误
std::ptrdiff_t try_read(int fd, char* buf, std::size_t n) {
  ssize_t const r = ::read(fd, buf, n);
  if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
  return r;
}

// 子进程里应用资源限额（超限即 _exit，执行体不存在时 parent 见 exit_code=-1）
void apply_limits(SpawnLimits const& limits) {
  if (limits.fsize) {
    rlimit rl{256u << 20, 256u << 20};  // RLIMIT_FSIZE = 256MB
    ::setrlimit(RLIMIT_FSIZE, &rl);
  }
  if (limits.nofile) {
    rlimit rl{1024, 1024};  // RLIMIT_NOFILE = 1024
    ::setrlimit(RLIMIT_NOFILE, &rl);
  }
}

}  // namespace

ExecResult run_argv(std::vector<std::string> const& argv,
                    std::optional<std::filesystem::path> const& cwd,
                    SpawnLimits const& limits,
                    std::optional<std::chrono::milliseconds> timeout,
                    std::size_t max_output) {
  ExecResult res;
  if (argv.empty() || argv[0].empty()) {
    res.exit_code = -1;
    res.note = "空命令";
    return res;
  }

  int out[2];
  if (::pipe(out) != 0) {
    res.exit_code = -1;
    res.note = "pipe() 失败";
    return res;
  }
  int const devnull = ::open("/dev/null", O_RDONLY);
  if (devnull < 0) {
    ::close(out[0]);
    ::close(out[1]);
    res.exit_code = -1;
    res.note = "open(/dev/null) 失败";
    return res;
  }

  pid_t const pid = ::fork();
  if (pid < 0) {
    ::close(out[0]);
    ::close(out[1]);
    ::close(devnull);
    res.exit_code = -1;
    res.note = "fork() 失败";
    return res;
  }

  if (pid == 0) {
    // 子进程：独立进程组（便于对外整体强杀）；可选 chdir + 资源限额；stdin=/dev/null
    ::setpgid(0, 0);
    apply_limits(limits);
    if (cwd.has_value()) ::chdir(cwd->c_str());
    ::dup2(devnull, STDIN_FILENO);
    ::dup2(out[1], STDOUT_FILENO);
    ::dup2(out[1], STDERR_FILENO);
    ::close(devnull);
    ::close(out[0]);
    ::close(out[1]);

    std::vector<char*> av;
    av.reserve(argv.size() + 1);
    for (auto const& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    ::execv(av[0], av.data());
    ::_exit(127);  // 无法执行（argv[0] 不存在等）——被宿主兜住
  }

  // 父进程：非阻塞读 + 轮询回收 + 超时强杀（与旧实现逐行一致）
  ::close(out[1]);
  ::close(devnull);
  int const flags = ::fcntl(out[0], F_GETFL, 0);
  ::fcntl(out[0], F_SETFL, flags | O_NONBLOCK);

  std::string buf;
  bool truncated = false;
  auto const deadline = timeout.has_value()
                            ? std::chrono::steady_clock::now() + *timeout
                            : std::chrono::steady_clock::time_point::max();

  for (;;) {
    char chunk[4096];
    for (;;) {
      std::ptrdiff_t const r = try_read(out[0], chunk, sizeof chunk);
      if (r > 0) {
        if (buf.size() < max_output) {
          std::size_t const room = max_output - buf.size();
          buf.append(chunk, static_cast<std::size_t>(r) > room ? room : static_cast<std::size_t>(r));
        }
        truncated = truncated || buf.size() >= max_output;
      } else {
        break;  // 0 = 暂无可读
      }
    }

    int status = 0;
    pid_t const wr = ::waitpid(pid, &status, WNOHANG);
    if (wr == pid) {
      if (WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
      else if (WIFSIGNALED(status)) res.exit_code = 128 + WTERMSIG(status);
      else res.exit_code = -1;
      break;
    }
    if (wr < 0 && errno != EINTR) {
      res.exit_code = -1;
      res.note = std::string("waitpid: ") + std::strerror(errno);
      break;
    }

    auto const now = std::chrono::steady_clock::now();
    if (deadline <= now) {
      ::kill(-pid, SIGKILL);  // 强杀整个进程组（含孙进程）
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      res.timed_out = true;
      res.exit_code = 128 + SIGKILL;
      res.note = "超时，进程组已强杀";
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // 回收到子进程后，把管道里剩余内容读尽（非阻塞，读到 EAGAIN/EOF 为止）
  char tail[4096];
  for (;;) {
    if (try_read(out[0], tail, sizeof tail) <= 0) break;
  }
  ::close(out[0]);

  res.output = std::move(buf);
  if (truncated && res.note.empty()) res.note = "输出超过上限，已截断保留前段";
  return res;
}

}  // namespace cppchain::tool::detail