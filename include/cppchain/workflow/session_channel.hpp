// cppchain —— workflow/session_channel.hpp
// 会话 C/S 通道（problems/07 §8 P2b）：FIFO + 服务端登记文件。
//
// 约定：
//   FIFO        <ws>/.wfr/sessions/session-<name>.fifo       （mkfifo 0600）
//   服务端登记  <ws>/.wfr/sessions/session-<name>.server.json {pid, workflow, started_epoch}
//   每请求回包  <ws>/.wfr/sessions/session-<name>.<pid>.<seq>.reply （客户端创建的 FIFO）
//
// 生命周期：服务端持会话锁；客户端不取锁（它在服务端的串行 FIFO 里排队）。
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace cppchain::workflow {

struct SessionServerInfo {
  long pid = 0;
  std::string workflow;
  long started_epoch = 0;
};

[[nodiscard]] std::filesystem::path session_fifo_path(std::filesystem::path const& workspace,
                                                      std::string const& session);
[[nodiscard]] std::filesystem::path session_server_path(std::filesystem::path const& workspace,
                                                        std::string const& session);
[[nodiscard]] std::filesystem::path session_reply_fifo_path(std::filesystem::path const& workspace,
                                                            std::string const& session, long pid,
                                                            int seq);

// mkfifo 0600；已存在且是 FIFO → 视为成功。返回 false 表示创建失败/非 FIFO。
[[nodiscard]] bool ensure_session_fifo(std::filesystem::path const& fifo);
void remove_session_fifo(std::filesystem::path const& fifo);

[[nodiscard]] std::optional<SessionServerInfo> read_server_info(
    std::filesystem::path const& workspace, std::string const& session);
[[nodiscard]] bool write_server_info(std::filesystem::path const& workspace,
                                     std::string const& session, SessionServerInfo const& info);
void clear_server_info(std::filesystem::path const& workspace, std::string const& session);

// 服务端是否仍活着（登记存在 + pid 存活 + FIFO 存在）。
[[nodiscard]] bool session_server_alive(std::filesystem::path const& workspace,
                                        std::string const& session);

// FIFO 行读器（服务器用 O_RDWR|O_NONBLOCK 打开：没有写者时也不会 EOF，
// 读用 poll 超时，便于同时检查取消/信号）。
class FifoLineReader {
 public:
  FifoLineReader() = default;
  ~FifoLineReader() { close(); }
  FifoLineReader(FifoLineReader const&) = delete;
  FifoLineReader& operator=(FifoLineReader const&) = delete;

  [[nodiscard]] bool open(std::filesystem::path const& fifo, bool keep_write_end);
  void close();
  [[nodiscard]] bool is_open() const { return fd_ >= 0; }
  // 读一行（不含换行）；超时/暂无完整行 → false（可重试）。
  [[nodiscard]] bool read_line(std::string& out, int timeout_ms);
  [[nodiscard]] int fd() const { return fd_; }

 private:
  int fd_ = -1;
  std::string buf_;
};

// 向 fd 写一行（阻塞 until 写完；失败返回 false）。
[[nodiscard]] bool write_fifo_line(int fd, std::string const& line);

}  // namespace cppchain::workflow
