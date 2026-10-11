// cppchain —— src/tool/spawn_executor.cpp
// PlainSpawnExecutor：fork/exec `/bin/sh -c` + 进程组超时强杀（wfr-㊸ @① 的执行器底线形态）。
// ⚠️ 定位：只做**进程级**隔离（子进程独立地址空间、进程组可整体强杀、stdin=/dev/null、
// 输出容量封顶），**不是沙箱**（文件系统/网络无约束）。批③ 由 SandboxManager
// （bwrap/容器 provider）替换，接口不变（01-discuss/06 §5 L4）。
#include "cppchain/tool/executor.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace cppchain::tool {
namespace {

// 非阻塞读一段；返回 >0 实际读取、0 无数据可读（EAGAIN/EINTR）、-1 真错误
std::ptrdiff_t try_read(int fd, char* buf, std::size_t n) {
  ssize_t r = ::read(fd, buf, n);
  if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
  return r;
}

}  // namespace

class PlainSpawnExecutor final : public ToolExecutor {
 public:
  explicit PlainSpawnExecutor(std::size_t max_output = 1u << 20) : max_output_(max_output) {}

  ExecResult run(std::string const& command,
                 std::optional<std::chrono::milliseconds> timeout) const override {
    ExecResult res;
    if (command.empty()) { res.exit_code = -1; res.note = "空命令"; return res; }

    int out[2];
    if (::pipe(out) != 0) { res.exit_code = -1; res.note = "pipe() 失败"; return res; }
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
      ::close(out[0]); ::close(out[1]); ::close(devnull);
      res.exit_code = -1;
      res.note = "fork() 失败";
      return res;
    }

    if (pid == 0) {
      // 子进程：独立进程组（便于对外整体强杀）；stdin=/dev/null；stdout+stderr 合流
      ::setpgid(0, 0);
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(out[1], STDOUT_FILENO);
      ::dup2(out[1], STDERR_FILENO);
      ::close(devnull);
      ::close(out[0]);
      ::close(out[1]);
      ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);  // shell 不存在（本机缺 /bin/sh）——被分类器与宿主环境兜住
    }

    // 父进程：非阻塞读 + 轮询回收 + 超时强杀
    ::close(out[1]);
    ::close(devnull);
    int flags = ::fcntl(out[0], F_GETFL, 0);
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
          if (buf.size() < max_output_) {
            std::size_t const room = max_output_ - buf.size();
            buf.append(chunk, static_cast<std::size_t>(r) > room ? room
                                                                  : static_cast<std::size_t>(r));
          }
          bool const full = buf.size() >= max_output_;
          truncated = truncated || full;
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
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
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
      std::ptrdiff_t const r = try_read(out[0], tail, sizeof tail);
      if (r > 0) continue;          // 只清空，不再累计（防截断条件下反复冲洗）
      break;
    }
    ::close(out[0]);

    res.output = std::move(buf);
    if (truncated && res.note.empty()) res.note = "输出超过上限，已截断保留前段";
    return res;
  }

 private:
  std::size_t max_output_;
};

// 工厂：批① 的默认执行器（进程级隔离，非沙箱——见头文件定位说明）
[[nodiscard]] std::shared_ptr<ToolExecutor> make_plain_spawn_executor(std::size_t max_output) {
  return std::make_shared<PlainSpawnExecutor>(max_output);
}

}  // namespace cppchain::tool