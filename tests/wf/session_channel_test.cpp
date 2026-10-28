// tests/wf —— problems/07 §8 P2b：会话 C/S 通道（FIFO 服务端/客户端）。
// 单测：路径/登记/FIFO 行读写；E2E：--lock-session --as-server + --as-client 往返、
// --post 投递不等待、--stream 事件透传、SIGTERM 收尾清理。
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <unistd.h>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/tool/spawn_runner.hpp"
#include "cppchain/workflow/session_channel.hpp"
#include "cppchain/workflow/session_lock.hpp"
#include "cppchain/workflow/session_log.hpp"

using namespace cppchain::workflow;

namespace {

struct TempDir {
  std::filesystem::path dir;
  TempDir()
      : dir(std::filesystem::temp_directory_path() /
            ("wf_chan_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
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

void write_quick_workflow(std::filesystem::path const& ws) {
  auto const dir = ws / ".wfr" / "workflows";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "wf_quick.json")
      << R"({"id":"wf_quick","nodes":[{"id":"e","kind":"echo","text":"hello from child","next":"_turn_end"}]})";
}

std::filesystem::path slow_lib() {
  return wfr_bin().parent_path().parent_path() / "tests" / "libslow_tools.so";
}

void write_slow_workflow(std::filesystem::path const& ws) {
  auto const dir = ws / ".wfr" / "workflows";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "wf_slow.json") << R"({"id":"wf_slow","nodes":[
    {"id":"treg","kind":"tool_registry","tools":[{"from":"slow_tools","type":"so","load":["slow_sleep"]}],"next":"recall"},
    {"id":"recall","kind":"load_history","path":"@config.workspace","session":"@config.session","next":"act"},
    {"id":"act","kind":"run_tools","next":"after"},
    {"id":"after","kind":"echo","text":"done","next":"_turn_end"}]})";
}

void seed_tool_call(std::filesystem::path const& ws, std::string const& session) {
  namespace msg = cppchain::message;
  msg::Message ai = msg::ai_message("");
  ai.tool_calls.push_back(msg::ToolCall{"c1", "slow_sleep", nlohmann::json{{"ms", 3000}}});
  SessionLog log(session_path(ws.string(), session));
  log.engine_enter("r-seed", "seed", "model");
  log.engine_exit("r-seed", "seed", "model", true, "ok", 0,
                  nlohmann::json{{"messages", nlohmann::json::array({nlohmann::json(ai)})}});
}

int count_node_enters(std::filesystem::path const& ws, std::string const& session,
                      std::string const& node) {
  std::ifstream in(session_path(ws.string(), session));
  int n = 0;
  std::string line;
  while (std::getline(in, line)) {
    auto const j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded()) continue;
    if (j.value("source", "") == "engine" && j.value("phase", "") == "enter" &&
        j.value("node", "") == node)
      ++n;
  }
  return n;
}

int count_echo_enters(std::filesystem::path const& ws, std::string const& session) {
  std::ifstream in(session_path(ws.string(), session));
  int n = 0;
  std::string line;
  while (std::getline(in, line)) {
    auto const j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded()) continue;
    if (j.value("source", "") == "engine" && j.value("phase", "") == "enter" &&
        j.value("node", "") == "e")
      ++n;
  }
  return n;
}

}  // namespace

TEST(WfSessionChannel, paths_and_server_info_roundtrip) {
  TempDir tmp;
  EXPECT_EQ(session_fifo_path(tmp.dir, "s").filename(), "session-s.fifo");
  EXPECT_EQ(session_server_path(tmp.dir, "s").filename(), "session-s.server.json");
  EXPECT_EQ(session_reply_fifo_path(tmp.dir, "s", 42, 3).filename(),
            "session-s.42.3.reply");

  EXPECT_FALSE(read_server_info(tmp.dir, "s").has_value());
  ASSERT_TRUE(write_server_info(tmp.dir, "s", SessionServerInfo{12345, "wf", 99}));
  auto const info = read_server_info(tmp.dir, "s");
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->pid, 12345);
  EXPECT_EQ(info->workflow, "wf");
  EXPECT_EQ(info->started_epoch, 99);
  clear_server_info(tmp.dir, "s");
  EXPECT_FALSE(read_server_info(tmp.dir, "s").has_value());
}

TEST(WfSessionChannel, fifo_line_roundtrip) {
  TempDir tmp;
  auto const fifo = tmp.dir / "x.fifo";
  ASSERT_TRUE(ensure_session_fifo(fifo));
  FifoLineReader reader;
  ASSERT_TRUE(reader.open(fifo, /*keep_write_end=*/true));
  EXPECT_TRUE(write_fifo_line(reader.fd(), R"({"a":1})"));
  EXPECT_TRUE(write_fifo_line(reader.fd(), "second"));
  std::string line;
  ASSERT_TRUE(reader.read_line(line, 1000));
  EXPECT_EQ(line, R"({"a":1})");
  ASSERT_TRUE(reader.read_line(line, 1000));
  EXPECT_EQ(line, "second");
  EXPECT_FALSE(reader.read_line(line, 50));  // 无更多数据
  remove_session_fifo(fifo);
}

TEST(WfSessionChannel, server_client_roundtrip_and_post) {
  if (!std::filesystem::exists(wfr_bin())) GTEST_SKIP() << "wfr 未构建";
  TempDir tmp;
  write_quick_workflow(tmp.dir);

  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });

  std::optional<SessionServerInfo> info;
  for (int i = 0; i < 100 && !info; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    info = read_server_info(tmp.dir, "s");
  }
  ASSERT_TRUE(info.has_value()) << "服务端未启动";

  // 同步客户端：拿输出
  auto const c1 = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
       "--as-client", "hello"},
      std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  EXPECT_EQ(c1.exit_code, 0) << c1.output;
  EXPECT_NE(c1.output.find("hello from child"), std::string::npos) << c1.output;

  // 流式客户端：事件行透传（NDJSON 里有 node_end）
  auto const c2 = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
       "--as-client", "--stream", "hello2"},
      std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  EXPECT_EQ(c2.exit_code, 0) << c2.output;
  EXPECT_NE(c2.output.find("\"type\":\"node_end\""), std::string::npos) << c2.output;

  // --post：不等结果
  auto const c3 = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
       "--as-client", "--post", "hello3"},
      std::nullopt, {}, std::chrono::seconds(10), 1u << 20);
  EXPECT_EQ(c3.exit_code, 0) << c3.output;
  EXPECT_NE(c3.output.find("已投递"), std::string::npos) << c3.output;

  // post 之后服务端仍会执行（轮询日志到 3 次 echo）
  int enters = 0;
  for (int i = 0; i < 100 && enters < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    enters = count_echo_enters(tmp.dir, "s");
  }
  EXPECT_EQ(enters, 3);

  // wfr stop 收尾：清 FIFO/登记/锁
  auto const st = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "stop", "--workdir", tmp.dir.string(), "--session", "s"},
      std::nullopt, {}, std::chrono::seconds(15), 1u << 20);
  EXPECT_EQ(st.exit_code, 0) << st.output;
  EXPECT_NE(st.output.find("已停止"), std::string::npos) << st.output;
  server.join();
  EXPECT_FALSE(std::filesystem::exists(session_fifo_path(tmp.dir, "s")));
  EXPECT_FALSE(std::filesystem::exists(session_server_path(tmp.dir, "s")));
  EXPECT_FALSE(std::filesystem::exists(session_lock_path(tmp.dir, "s")));
}

// P2c：请求级 workflow + 队列位置回执（慢请求占住服务端，后来的快请求看到"排队第 N 位"）。
TEST(WfSessionChannel, queue_position_and_per_request_workflow) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP() << "wfr / libslow_tools.so 未构建";
  TempDir tmp;
  write_quick_workflow(tmp.dir);
  write_slow_workflow(tmp.dir);
  seed_tool_call(tmp.dir, "s");

  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server"},
        std::nullopt, {}, std::chrono::seconds(40), 1u << 20);
  });
  std::optional<SessionServerInfo> info;
  for (int i = 0; i < 100 && !info; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    info = read_server_info(tmp.dir, "s");
  }
  ASSERT_TRUE(info.has_value());

  // 慢请求（客户端指定 wf_slow）：3s
  cppchain::tool::ExecResult slow;
  std::thread slow_client([&] {
    slow = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_slow", "--workdir", tmp.dir.string(), "--session", "s",
         "--as-client", "slow"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });
  bool act_started = false;
  for (int i = 0; i < 100 && !act_started; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    act_started = count_node_enters(tmp.dir, "s", "act") > 0;
  }
  ASSERT_TRUE(act_started) << "慢请求未开始执行";

  // 快请求在慢请求进行中到达 → 应收到 queued{position}
  auto const quick = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
       "--as-client", "quick"},
      std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  EXPECT_EQ(quick.exit_code, 0) << quick.output;
  EXPECT_NE(quick.output.find("排队第"), std::string::npos) << quick.output;
  EXPECT_NE(quick.output.find("hello from child"), std::string::npos) << quick.output;

  slow_client.join();
  EXPECT_EQ(slow.exit_code, 0) << slow.output;

  auto const st = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "stop", "--workdir", tmp.dir.string(), "--session", "s"},
      std::nullopt, {}, std::chrono::seconds(15), 1u << 20);
  EXPECT_EQ(st.exit_code, 0) << st.output;
  server.join();
}

// P2c：cancel --request —— 取消排队中的请求
TEST(WfSessionChannel, cancel_queued_request_by_id) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP();
  TempDir tmp;
  write_quick_workflow(tmp.dir);
  write_slow_workflow(tmp.dir);
  seed_tool_call(tmp.dir, "s");

  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server"},
        std::nullopt, {}, std::chrono::seconds(40), 1u << 20);
  });
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (read_server_info(tmp.dir, "s").has_value()) break;
  }

  cppchain::tool::ExecResult slow;
  std::thread slow_client([&] {
    slow = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_slow", "--workdir", tmp.dir.string(), "--session", "s",
         "--as-client", "slow"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (count_node_enters(tmp.dir, "s", "act") > 0) break;
  }

  cppchain::tool::ExecResult queued;
  std::thread queued_client([&] {
    queued = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
         "--as-client", "--request-id", "q1", "queued"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  auto const cc = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "cancel", "--workdir", tmp.dir.string(), "--session", "s",
       "--request", "q1"},
      std::nullopt, {}, std::chrono::seconds(10), 1u << 20);
  EXPECT_EQ(cc.exit_code, 0) << cc.output;

  queued_client.join();
  EXPECT_EQ(queued.exit_code, 130) << queued.output;  // 排队中被取消
  slow_client.join();
  EXPECT_EQ(slow.exit_code, 0) << slow.output;        // 正在跑的不受影响

  (void)cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "stop", "--workdir", tmp.dir.string(), "--session", "s"},
      std::nullopt, {}, std::chrono::seconds(15), 1u << 20);
  server.join();
}

// P2c：cancel --request —— 取消执行中的请求（服务端 SIGTERM 子进程）
TEST(WfSessionChannel, cancel_running_request_by_id) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP();
  TempDir tmp;
  write_quick_workflow(tmp.dir);
  write_slow_workflow(tmp.dir);
  seed_tool_call(tmp.dir, "s");

  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server"},
        std::nullopt, {}, std::chrono::seconds(40), 1u << 20);
  });
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (read_server_info(tmp.dir, "s").has_value()) break;
  }

  cppchain::tool::ExecResult slow;
  std::thread slow_client([&] {
    slow = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_slow", "--workdir", tmp.dir.string(), "--session", "s",
         "--as-client", "--request-id", "s1", "slow"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (count_node_enters(tmp.dir, "s", "act") > 0) break;
  }

  auto const cc = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "cancel", "--workdir", tmp.dir.string(), "--session", "s",
       "--request", "s1"},
      std::nullopt, {}, std::chrono::seconds(10), 1u << 20);
  EXPECT_EQ(cc.exit_code, 0) << cc.output;

  slow_client.join();
  EXPECT_EQ(slow.exit_code, 130) << slow.output;
  EXPECT_GT(count_node_enters(tmp.dir, "s", "__cancelled__"), 0);

  (void)cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "stop", "--workdir", tmp.dir.string(), "--session", "s"},
      std::nullopt, {}, std::chrono::seconds(15), 1u << 20);
  server.join();
}

// Web 审批 UI 的后端入口：CGI action=approve → 服务端 FIFO approve_response
TEST(WfSessionChannel, cgi_approve_action_forwards_control) {
  auto const cgi = wfr_bin().parent_path() / "wfr_cgi";
  if (!std::filesystem::exists(cgi) || !std::filesystem::exists(wfr_bin())) GTEST_SKIP();
  auto const repo = wfr_bin().parent_path().parent_path().parent_path().parent_path();
  TempDir tmp;
  write_quick_workflow(tmp.dir);

  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (read_server_info(tmp.dir, "s").has_value()) break;
  }
  std::string const qs = "QUERY_STRING=action=approve&ws=" + tmp.dir.string() +
                        "&s=s&request=r1&decision=approve";
  auto const r = cppchain::tool::detail::run_argv({"/usr/bin/env", qs, cgi.string()}, repo,
                                                  {}, std::chrono::seconds(10), 1u << 20);
  EXPECT_EQ(r.exit_code, 0) << r.output;
  EXPECT_NE(r.output.find("\"ok\":true"), std::string::npos) << r.output;

  (void)cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "stop", "--workdir", tmp.dir.string(), "--session", "s"},
      std::nullopt, {}, std::chrono::seconds(15), 1u << 20);
  server.join();

  // 没有服务端 → 明确报错（审批 UI 只适用于 C/S）
  auto const r2 = cppchain::tool::detail::run_argv({"/usr/bin/env", qs, cgi.string()}, repo,
                                                   {}, std::chrono::seconds(10), 1u << 20);
  EXPECT_EQ(r2.exit_code, 0) << r2.output;
  EXPECT_NE(r2.output.find("没有运行中的服务端"), std::string::npos) << r2.output;
}
