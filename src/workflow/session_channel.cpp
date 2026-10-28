// cppchain —— src/workflow/session_channel.cpp
#include "cppchain/workflow/session_channel.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace cppchain::workflow {
namespace {

using Json = nlohmann::json;

bool pid_alive(long pid) {
  if (pid <= 0) return false;
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
}

std::string session_stem(std::string const& session) {
  return session.empty() ? "session" : "session-" + session;
}

}  // namespace

std::filesystem::path session_fifo_path(std::filesystem::path const& workspace,
                                        std::string const& session) {
  return workspace / ".wfr" / "sessions" / (session_stem(session) + ".fifo");
}

std::filesystem::path session_server_path(std::filesystem::path const& workspace,
                                          std::string const& session) {
  return workspace / ".wfr" / "sessions" / (session_stem(session) + ".server.json");
}

std::filesystem::path session_reply_fifo_path(std::filesystem::path const& workspace,
                                              std::string const& session, long pid, int seq) {
  return workspace / ".wfr" / "sessions" /
         (session_stem(session) + "." + std::to_string(pid) + "." + std::to_string(seq) +
          ".reply");
}

bool ensure_session_fifo(std::filesystem::path const& fifo) {
  std::error_code ec;
  std::filesystem::create_directories(fifo.parent_path(), ec);
  struct stat st {};
  if (::stat(fifo.c_str(), &st) == 0) {
    if (S_ISFIFO(st.st_mode)) return true;
    ::unlink(fifo.c_str());  // 非 FIFO 占位 → 清掉重建
  }
  return ::mkfifo(fifo.c_str(), 0600) == 0 || errno == EEXIST;
}

void remove_session_fifo(std::filesystem::path const& fifo) { ::unlink(fifo.c_str()); }

std::optional<SessionServerInfo> read_server_info(std::filesystem::path const& workspace,
                                                  std::string const& session) {
  std::ifstream in(session_server_path(workspace, session));
  if (!in) return std::nullopt;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
  Json const j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("pid") || !j["pid"].is_number_integer())
    return std::nullopt;
  SessionServerInfo info;
  info.pid = j.value("pid", 0L);
  info.workflow = j.value("workflow", std::string{});
  info.started_epoch = j.value("started_epoch", 0L);
  if (info.pid <= 0) return std::nullopt;
  return info;
}

bool write_server_info(std::filesystem::path const& workspace, std::string const& session,
                       SessionServerInfo const& info) {
  auto const path = session_server_path(workspace, session);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  auto const tmp = path.string() + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream o(tmp, std::ios::trunc);
    if (!o) return false;
    o << Json{{"pid", info.pid},
              {"workflow", info.workflow},
              {"started_epoch",
               info.started_epoch != 0 ? info.started_epoch : static_cast<long>(std::time(nullptr))}}
             .dump();
    o.close();
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

void clear_server_info(std::filesystem::path const& workspace, std::string const& session) {
  ::unlink(session_server_path(workspace, session).c_str());
}

bool session_server_alive(std::filesystem::path const& workspace, std::string const& session) {
  auto const info = read_server_info(workspace, session);
  if (!info.has_value() || !pid_alive(info->pid)) return false;
  struct stat st {};
  return ::stat(session_fifo_path(workspace, session).c_str(), &st) == 0 && S_ISFIFO(st.st_mode);
}

bool FifoLineReader::open(std::filesystem::path const& fifo, bool keep_write_end) {
  close();
  (void)ensure_session_fifo(fifo);
  // keep_write_end=true（服务器）：O_RDWR 防止没有写者时 read 返回 EOF；
  // false（客户端回包）：O_RDONLY|O_NONBLOCK，先于请求发送打开。
  int const flags = keep_write_end ? (O_RDWR | O_NONBLOCK) : (O_RDONLY | O_NONBLOCK);
  fd_ = ::open(fifo.c_str(), flags | O_CLOEXEC);
  buf_.clear();
  return fd_ >= 0;
}

void FifoLineReader::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  buf_.clear();
}

bool FifoLineReader::read_line(std::string& out, int timeout_ms) {
  if (fd_ < 0) return false;
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    auto const nl = buf_.find('\n');
    if (nl != std::string::npos) {
      out = buf_.substr(0, nl);
      buf_.erase(0, nl + 1);
      return true;
    }
    auto const now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    auto const left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    struct pollfd p{fd_, POLLIN, 0};
    int const pr = ::poll(&p, 1, left > 0 ? left : 1);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (pr == 0) return false;
    char tmp[4096];
    ssize_t const n = ::read(fd_, tmp, sizeof(tmp));
    if (n > 0) {
      buf_.append(tmp, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
    return false;  // EOF / 错误
  }
}

bool write_fifo_line(int fd, std::string const& line) {
  std::string data = line;
  data.push_back('\n');
  std::size_t off = 0;
  while (off < data.size()) {
    ssize_t const n = ::write(fd, data.data() + off, data.size() - off);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, 1000) < 0 && errno != EINTR) return false;
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace cppchain::workflow

namespace cppchain::workflow {

// ---- P2：审批通道（UNIX domain socket） ----
namespace {

int read_line_fd(int fd, std::string& buf, std::string& out, int timeout_ms) {
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    auto const nl = buf.find('\n');
    if (nl != std::string::npos) {
      out = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      return 1;
    }
    auto const now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    auto const left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    struct pollfd p{fd, POLLIN, 0};
    int const pr = ::poll(&p, 1, left > 0 ? left : 1);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (pr == 0) return 0;
    char tmp[4096];
    ssize_t const n = ::read(fd, tmp, sizeof(tmp));
    if (n > 0) {
      buf.append(tmp, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
    return -1;
  }
}

}  // namespace

bool LineChannel::open_fd(int fd) {
  close();
  fd_ = fd;
  buf_.clear();
  return fd_ >= 0;
}

void LineChannel::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  buf_.clear();
}

bool LineChannel::read_line(std::string& out, int timeout_ms) {
  if (fd_ < 0) return false;
  return read_line_fd(fd_, buf_, out, timeout_ms) > 0;
}

bool LineChannel::write_line(std::string const& line) { return write_fifo_line(fd_, line); }

int unix_listen(std::string const& path) {
  ::unlink(path.c_str());
  int const fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 8) != 0) {
    ::close(fd);
    return -1;
  }
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

int unix_accept(int listen_fd, int timeout_ms) {
  if (listen_fd < 0) return -1;
  struct pollfd p{listen_fd, POLLIN, 0};
  int const pr = ::poll(&p, 1, timeout_ms);
  if (pr <= 0) return -1;
  int const fd = ::accept(listen_fd, nullptr, nullptr);
  if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

int unix_connect(std::string const& path) {
  int const fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

}  // namespace cppchain::workflow
