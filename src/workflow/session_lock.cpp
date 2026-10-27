// cppchain —— src/workflow/session_lock.cpp
// 会话单飞锁：临时文件写全内容 → link 原子落位；stale（pid 已死/内容损坏）自动回收。
#include "cppchain/workflow/session_lock.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>

#include <signal.h>
#include <unistd.h>

namespace cppchain::workflow {
namespace {

using Json = nlohmann::json;

bool pid_alive(long pid) {
  if (pid <= 0) return false;
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;  // 存在但无权限 → 活着
}

std::optional<SessionLockInfo> parse_info(std::string const& text) {
  Json j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("pid") || !j["pid"].is_number_integer())
    return std::nullopt;
  SessionLockInfo info;
  info.pid = j.value("pid", 0L);
  info.run = j.value("run", std::string{});
  info.started_epoch = j.value("started_epoch", 0L);
  if (info.pid <= 0) return std::nullopt;
  return info;
}

std::optional<SessionLockInfo> read_at(std::filesystem::path const& path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
  return parse_info(text);
}

}  // namespace

std::filesystem::path session_lock_path(std::filesystem::path const& workspace,
                                        std::string const& session) {
  std::string const name = session.empty() ? "session" : "session-" + session;
  return workspace / ".wfr" / "sessions" / (name + ".lock");
}

std::optional<SessionLockInfo> read_session_lock(std::filesystem::path const& workspace,
                                                 std::string const& session) {
  return read_at(session_lock_path(workspace, session));
}

SessionLockAcquire acquire_session_lock(std::filesystem::path const& workspace,
                                        std::string const& session, std::string const& run) {
  SessionLockAcquire out;
  std::filesystem::path const lock = session_lock_path(workspace, session);
  std::error_code ec;
  std::filesystem::create_directories(lock.parent_path(), ec);

  SessionLockInfo const self{static_cast<long>(::getpid()), run,
                             static_cast<long>(std::time(nullptr))};

  for (int attempt = 0; attempt < 3; ++attempt) {
    // 临时文件写全内容 → link(lock)；link 原子且已存在即失败（不会读到半截内容）
    std::filesystem::path const tmp =
        lock.string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(attempt);
    {
      std::ofstream o(tmp, std::ios::trunc);
      if (!o) {
        out.error = "无法写会话锁临时文件：" + tmp.string();
        return out;
      }
      o << Json{{"pid", self.pid}, {"run", self.run}, {"started_epoch", self.started_epoch}}
               .dump();
      o.close();
    }
    if (::link(tmp.c_str(), lock.c_str()) == 0) {
      ::unlink(tmp.c_str());
      out.acquired = true;
      return out;
    }
    int const err = errno;
    ::unlink(tmp.c_str());
    if (err != EEXIST) {
      out.error = "link 会话锁失败：" + std::string(std::strerror(err));
      return out;
    }

    // 已存在：读持有者；pid 已死或内容损坏 → stale 回收后重试
    auto const holder = read_at(lock);
    if (!holder.has_value() || !pid_alive(holder->pid)) {
      ::unlink(lock.c_str());
      out.stale_reaped = true;
      continue;
    }
    out.holder = holder;
    return out;  // Busy
  }
  out.error = "无法获取会话锁（并发竞争或锁文件异常）";
  return out;
}

void release_session_lock(std::filesystem::path const& workspace, std::string const& session) {
  auto const lock = session_lock_path(workspace, session);
  auto const info = read_at(lock);
  if (info.has_value() && info->pid == static_cast<long>(::getpid())) ::unlink(lock.c_str());
}

// ---- P1：取消标记 ----
std::filesystem::path session_cancel_path(std::filesystem::path const& workspace,
                                          std::string const& session) {
  std::string const name = session.empty() ? "session" : "session-" + session;
  return workspace / ".wfr" / "sessions" / (name + ".cancel");
}

bool write_session_cancel(std::filesystem::path const& workspace, std::string const& session,
                          long target_pid, std::string const& run) {
  auto const path = session_cancel_path(workspace, session);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  auto const tmp = path.string() + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream o(tmp, std::ios::trunc);
    if (!o) return false;
    o << Json{{"pid", target_pid},
              {"run", run},
              {"requested_epoch", static_cast<long>(std::time(nullptr))}}
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

std::optional<SessionCancelRequest> read_session_cancel(std::filesystem::path const& workspace,
                                                        std::string const& session) {
  std::ifstream in(session_cancel_path(workspace, session));
  if (!in) return std::nullopt;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
  Json j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("pid") || !j["pid"].is_number_integer())
    return std::nullopt;
  SessionCancelRequest req;
  req.pid = j.value("pid", 0L);
  req.run = j.value("run", std::string{});
  req.requested_epoch = j.value("requested_epoch", 0L);
  return req;
}

void clear_session_cancel(std::filesystem::path const& workspace, std::string const& session) {
  ::unlink(session_cancel_path(workspace, session).c_str());
}

}  // namespace cppchain::workflow
