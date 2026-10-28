// tests/wf —— problems/07 §13：人工审批通道 --approve（UNIX socket）+ 三层回退顺序。
//   ① --approve 通道（mock approver）
//   ② 无 --approve → 服务端转给客户端
//   ③ 客户端不可用 → 回退服务端 stdin
// 用 slow_tools 的 slow_sleep_guarded（只读 + NeedHuman）：批准 → 执行；拒绝 → error。
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/tool/spawn_runner.hpp"
#include "cppchain/workflow/session_channel.hpp"
#include "cppchain/workflow/session_lock.hpp"
#include "cppchain/workflow/session_log.hpp"

using namespace cppchain::workflow;
namespace message = cppchain::message;

namespace {

using Json = nlohmann::json;

struct TempDir {
  std::filesystem::path dir;
  TempDir()
      : dir(std::filesystem::temp_directory_path() /
            ("wf_appr_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
};

std::filesystem::path wfr_bin() {
  char buf[4096];
  ssize_t const n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  return std::filesystem::path(std::string(buf, static_cast<std::size_t>(n)))
             .parent_path()
             .parent_path() /
         "bin" / "wfr";
}
std::filesystem::path slow_lib() {
  return wfr_bin().parent_path().parent_path() / "tests" / "libslow_tools.so";
}

void write_workflow(std::filesystem::path const& ws, std::string const& tool) {
  auto const dir = ws / ".wfr" / "workflows";
  std::filesystem::create_directories(dir);
  Json ir{{"id", "wf_approve"},
          {"nodes", Json::array({
              Json{{"id", "treg"}, {"kind", "tool_registry"},
                   {"tools", Json::array({Json{{"from", "slow_tools"},
                                               {"type", "so"},
                                               {"load", Json::array({tool})}}})},
                   {"next", "recall"}},
              Json{{"id", "recall"}, {"kind", "load_history"},
                   {"path", "@config.workspace"}, {"session", "@config.session"},
                   {"next", "approve"}},
              Json{{"id", "approve"}, {"kind", "approve_tools"}, {"next", "act"}},
              Json{{"id", "act"}, {"kind", "run_tools"}, {"next", "_turn_end"}},
          })}};
  std::ofstream(dir / "wf_approve.json") << ir.dump(2);
}

void seed(std::filesystem::path const& ws, std::string const& tool) {
  message::Message ai = message::ai_message("");
  ai.tool_calls.push_back(message::ToolCall{"c1", tool, Json{{"ms", 0}}});
  SessionLog log(session_path(ws.string(), "s"));
  log.engine_enter("r-seed", "seed", "model");
  log.engine_exit("r-seed", "seed", "model", true, "ok", 0,
                  Json{{"messages", Json::array({Json(ai)})}});
}

Json last_tool_message(std::filesystem::path const& ws) {
  std::ifstream in(session_path(ws.string(), "s"));
  Json last;
  std::string line;
  while (std::getline(in, line)) {
    auto const j = Json::parse(line, nullptr, false);
    if (j.is_discarded()) continue;
    if (!j.contains("update") || !j["update"].is_object()) continue;
    if (!j["update"].contains("messages")) continue;
    for (Json const& m : j["update"]["messages"])
      if (m.value("type", "") == "tool") last = m;
  }
  return last;
}

bool send_control(std::filesystem::path const& ws, std::string const& session, Json const& msg) {
  int const fd = ::open(session_fifo_path(ws.string(), session).c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  bool const ok = write_fifo_line(fd, msg.dump());
  ::close(fd);
  return ok;
}

// mock 审批人：监听 socket，读一条 approve_request，回 {action}。
struct MockApprover {
  std::filesystem::path path;
  std::string action;
  Json got;
  std::thread th;
  int lfd = -1;
  explicit MockApprover(std::filesystem::path p, std::string act)
      : path(std::move(p)), action(std::move(act)) {}
  void start() {
    lfd = unix_listen(path.string());
    ASSERT_GE(lfd, 0);
    th = std::thread([this] {
      int const c = unix_accept(lfd, 10000);
      if (c < 0) return;
      LineChannel ch(c);
      std::string line;
      if (ch.read_line(line, 10000)) got = Json::parse(line, nullptr, false);
      (void)ch.write_line(Json{{"action", action}}.dump());
    });
  }
  void join() {
    if (th.joinable()) th.join();
    if (lfd >= 0) ::close(lfd);
    ::unlink(path.c_str());
  }
};

// 只收请求、不作答的审批人（用于"审批等待期被取消"用例）
struct HoldApprover {
  std::filesystem::path path;
  std::atomic<bool> got{false};
  std::atomic<bool> release{false};
  std::thread th;
  int lfd = -1;
  explicit HoldApprover(std::filesystem::path p) : path(std::move(p)) {}
  void start() {
    lfd = unix_listen(path.string());
    ASSERT_GE(lfd, 0);
    th = std::thread([this] {
      int const c = unix_accept(lfd, 10000);
      if (c < 0) return;
      LineChannel ch(c);
      std::string line;
      if (ch.read_line(line, 10000)) got = true;
      while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
  }
  void join() {
    release = true;
    if (th.joinable()) th.join();
    if (lfd >= 0) ::close(lfd);
    ::unlink(path.c_str());
  }
};

// 带 stdin 的 spawn（客户端交互审批用）：stdin 预写 input，stdout/stderr → /dev/null。
int run_with_stdin(std::vector<std::string> const& argv, std::string const& input) {
  int p[2];
  if (::pipe(p) != 0) return -1;
  pid_t const pid = ::fork();
  if (pid == 0) {
    ::dup2(p[0], STDIN_FILENO);
    ::close(p[0]);
    ::close(p[1]);
    int const devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
    }
    std::vector<char*> av;
    for (auto const& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    ::execvp(av[0], av.data());
    ::_exit(127);
  }
  ::close(p[0]);
  if (pid < 0) {
    ::close(p[1]);
    return -1;
  }
  (void)::write(p[1], input.data(), input.size());
  ::close(p[1]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
}

}  // namespace

TEST(WfApproveChannel, approve_over_socket_executes_tool) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP();
  TempDir tmp;
  write_workflow(tmp.dir, "slow_sleep_guarded");
  seed(tmp.dir, "slow_sleep_guarded");

  MockApprover mock(tmp.dir / "approve.sock", "approve");
  mock.start();
  auto const r = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
       "--approve", mock.path.string(), "go"},
      std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  mock.join();
  EXPECT_EQ(r.exit_code, 0) << r.output;
  EXPECT_EQ(mock.got.value("kind", ""), "approve_request");
  EXPECT_EQ(mock.got.value("node", ""), "approve");
  EXPECT_EQ(last_tool_message(tmp.dir).value("status", ""), "success");
}

TEST(WfApproveChannel, reject_over_socket_blocks_tool) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP();
  TempDir tmp;
  write_workflow(tmp.dir, "slow_sleep_guarded");
  seed(tmp.dir, "slow_sleep_guarded");

  MockApprover mock(tmp.dir / "approve.sock", "reject");
  mock.start();
  auto const r = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
       "--approve", mock.path.string(), "go"},
      std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  mock.join();
  EXPECT_EQ(r.exit_code, 0) << r.output;
  EXPECT_EQ(last_tool_message(tmp.dir).value("status", ""), "error");
}

// ② 服务端 --approve 透传给子进程（同一 mock 审批人）
TEST(WfApproveChannel, server_passes_approve_channel_to_child) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP();
  TempDir tmp;
  write_workflow(tmp.dir, "slow_sleep_guarded");
  seed(tmp.dir, "slow_sleep_guarded");

  MockApprover mock(tmp.dir / "approve.sock", "approve");
  mock.start();
  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server", "--approve", mock.path.string()},
        std::nullopt, {}, std::chrono::seconds(40), 1u << 20);
  });
  // 等服务端登记
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (read_server_info(tmp.dir, "s").has_value()) break;
  }
  auto const c = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
       "--as-client", "go"},
      std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  EXPECT_EQ(c.exit_code, 0) << c.output;
  mock.join();
  auto const st = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "stop", "--workdir", tmp.dir.string(), "--session", "s"},
      std::nullopt, {}, std::chrono::seconds(15), 1u << 20);
  server.join();
  EXPECT_EQ(last_tool_message(tmp.dir).value("status", ""), "success");
}

// Web 审批路径：--stream 客户端把 approve 事件原样转发（等外部决定，不自动 fallback）。
static void run_web_approval_case(char const* decision, char const* expect_tool_status) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib())) return;
  TempDir tmp;
  write_workflow(tmp.dir, "slow_sleep_guarded");
  seed(tmp.dir, "slow_sleep_guarded");

  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server"},
        std::nullopt, {}, std::chrono::seconds(40), 1u << 20);
  });
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (read_server_info(tmp.dir, "s").has_value()) break;
  }

  cppchain::tool::ExecResult client;
  std::thread runner([&] {
    client = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
         "--as-client", "--stream", "--request-id", "w1", "go"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  ASSERT_TRUE(send_control(tmp.dir, "s",
                           Json{{"type", "approve_response"},
                                {"request", "w1"},
                                {"action", decision},
                                {"feedback", ""}}));
  runner.join();
  EXPECT_EQ(client.exit_code, 0) << client.output;
  EXPECT_NE(client.output.find("\"type\":\"approve\""), std::string::npos) << client.output;
  EXPECT_EQ(last_tool_message(tmp.dir).value("status", ""), expect_tool_status);

  (void)cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "stop", "--workdir", tmp.dir.string(), "--session", "s"},
      std::nullopt, {}, std::chrono::seconds(15), 1u << 20);
  server.join();
}

TEST(WfApproveChannel, web_stream_forwards_event_and_approve_decision) {
  run_web_approval_case("approve", "success");
}

TEST(WfApproveChannel, web_stream_fallback_goes_to_server_stdin) {
  run_web_approval_case("fallback", "error");  // 服务端 stdin=/dev/null → reject
}

// 审批等待期取消：子进程卡在 --approve 等待，SIGTERM 应立即取消（exit 130 + __cancelled__）
TEST(WfApproveChannel, cancel_during_approval_wait) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP();
  TempDir tmp;
  write_workflow(tmp.dir, "slow_sleep_guarded");
  seed(tmp.dir, "slow_sleep_guarded");

  HoldApprover mock(tmp.dir / "approve.sock");
  mock.start();
  cppchain::tool::ExecResult res;
  std::thread runner([&] {
    res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
         "--approve", mock.path.string(), "go"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });
  for (int i = 0; i < 100 && !mock.got.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(mock.got.load()) << "审批请求未到达 mock approver";
  auto const lock = read_session_lock(tmp.dir, "s");
  ASSERT_TRUE(lock.has_value());
  ASSERT_EQ(::kill(static_cast<pid_t>(lock->pid), SIGTERM), 0);
  runner.join();
  mock.join();
  EXPECT_EQ(res.exit_code, 130) << res.output;
  Json const tm = last_tool_message(tmp.dir);
  EXPECT_TRUE(tm.is_null() || tm.value("status", "").empty());  // 工具未执行
  bool cancelled = false;
  std::ifstream in(session_path(tmp.dir.string(), "s"));
  std::string line;
  while (std::getline(in, line)) {
    auto const j = Json::parse(line, nullptr, false);
    if (!j.is_discarded() && j.value("node", "") == "__cancelled__") cancelled = true;
  }
  EXPECT_TRUE(cancelled);
}

// 服务端审批等待期被 wfr cancel：子进程取消、客户端 exit 130、服务端存活
TEST(WfApproveChannel, server_cancel_during_approval_wait) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP();
  TempDir tmp;
  write_workflow(tmp.dir, "slow_sleep_guarded");
  seed(tmp.dir, "slow_sleep_guarded");

  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server"},
        std::nullopt, {}, std::chrono::seconds(40), 1u << 20);
  });
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (read_server_info(tmp.dir, "s").has_value()) break;
  }

  cppchain::tool::ExecResult client;
  std::thread runner([&] {
    client = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
         "--as-client", "--stream", "go"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(800));  // 进入审批等待

  auto const cc = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "cancel", "--workdir", tmp.dir.string(), "--session", "s",
       "--no-wait"},
      std::nullopt, {}, std::chrono::seconds(10), 1u << 20);
  EXPECT_EQ(cc.exit_code, 0) << cc.output;

  runner.join();
  EXPECT_EQ(client.exit_code, 130) << client.output;
  EXPECT_TRUE(read_server_info(tmp.dir, "s").has_value());  // 服务端仍存活

  (void)cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "stop", "--workdir", tmp.dir.string(), "--session", "s"},
      std::nullopt, {}, std::chrono::seconds(15), 1u << 20);
  server.join();
}

// 客户端交互审批：非 stream 客户端从 stdin 读 "approve" → approve_response → 工具执行
TEST(WfApproveChannel, client_interactive_approval_via_stdin) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP();
  TempDir tmp;
  write_workflow(tmp.dir, "slow_sleep_guarded");
  seed(tmp.dir, "slow_sleep_guarded");

  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server"},
        std::nullopt, {}, std::chrono::seconds(40), 1u << 20);
  });
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (read_server_info(tmp.dir, "s").has_value()) break;
  }

  int const rc = run_with_stdin(
      {wfr_bin().string(), "wf_approve", "--workdir", tmp.dir.string(), "--session", "s",
       "--as-client", "go"},
      "approve\n");
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(last_tool_message(tmp.dir).value("status", ""), "success");

  (void)cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "stop", "--workdir", tmp.dir.string(), "--session", "s"},
      std::nullopt, {}, std::chrono::seconds(15), 1u << 20);
  server.join();
}
