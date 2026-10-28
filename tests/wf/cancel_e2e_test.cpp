// tests/wf —— problems/07 §9.4：**真实端到端取消**（spawn wfr + 动态慢工具 slow_sleep）。
// 剧本：session 里预置一条 AI tool_call(slow_sleep ms=3000) → workflow:
//   tool_registry(load slow_sleep) → load_history → run_tools → echo(after)
// 运行中写 <session>.cancel → wfr 在 act 之后的节点边界取消：
//   exit 130、锁释放、会话日志有 __cancelled__、after 从未执行、act 已执行。
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

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
            ("wf_cancel_e2e_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~TempDir() { std::filesystem::remove_all(dir); }
};

std::filesystem::path self_exe() {
  char buf[4096];
  ssize_t const n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  return std::filesystem::path(std::string(buf, static_cast<std::size_t>(n)));
}
std::filesystem::path wfr_bin() { return self_exe().parent_path().parent_path() / "bin" / "wfr"; }
std::filesystem::path slow_lib() { return self_exe().parent_path() / "libslow_tools.so"; }

void write_quick_workflow(std::filesystem::path const& ws) {
  Json const ir = Json::parse(R"json({
    "id": "wf_quick",
    "nodes": [{"id": "e", "kind": "echo", "text": "ok", "next": "_turn_end"}]
  })json");
  auto const dir = ws / ".wfr" / "workflows";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "wf_quick.json") << ir.dump(2);
}

void write_workflow(std::filesystem::path const& ws) {
  Json const ir = Json::parse(R"json({
    "id": "wf_slow",
    "nodes": [
      {"id": "treg", "kind": "tool_registry",
       "tools": [{"from": "slow_tools", "type": "so", "load": ["slow_sleep"]}],
       "next": "recall"},
      {"id": "recall", "kind": "load_history",
       "path": "@config.workspace", "session": "@config.session", "next": "act"},
      {"id": "act", "kind": "run_tools", "next": "after"},
      {"id": "after", "kind": "echo", "text": "done", "next": "_turn_end"}
    ]
  })json");
  auto const dir = ws / ".wfr" / "workflows";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "wf_slow.json") << ir.dump(2);
}

void seed_tool_call(std::filesystem::path const& ws, std::string const& session) {
  message::Message ai = message::ai_message("");
  ai.tool_calls.push_back(message::ToolCall{"c1", "slow_sleep", Json{{"ms", 3000}}});
  SessionLog log(session_path(ws.string(), session));
  log.engine_enter("r-0", "seed", "model");
  log.engine_exit("r-0", "seed", "model", true, "ok", 0,
                  Json{{"messages", Json::array({Json(ai)})}});
}

std::vector<std::string> node_enters(std::filesystem::path const& ws, std::string const& session) {
  std::vector<std::string> out;
  std::ifstream in(session_path(ws.string(), session));
  std::string line;
  while (std::getline(in, line)) {
    Json const j = Json::parse(line, nullptr, false);
    if (j.is_discarded()) continue;
    if (j.value("source", "") != "engine" || j.value("phase", "") != "enter") continue;
    out.push_back(j.value("node", ""));
  }
  return out;
}

}  // namespace

TEST(WfCancelE2E, running_query_is_cancelled_at_node_boundary) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP() << "wfr / libslow_tools.so 未构建";

  TempDir tmp;
  write_workflow(tmp.dir);
  seed_tool_call(tmp.dir, "s");

  cppchain::tool::ExecResult res;
  std::thread runner([&] {
    res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_slow", "--workdir", tmp.dir.string(), "--session", "s", "go"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });

  // 等锁出现（= wfr 正在跑），再在 act（sleep 3s）执行期间写取消标记
  std::optional<SessionLockInfo> lock;
  for (int i = 0; i < 100 && !lock; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    lock = read_session_lock(tmp.dir, "s");
  }
  ASSERT_TRUE(lock.has_value()) << "wfr 未在 5s 内启动";
  // 等 act 节点真正开始（日志里出现 enter）再取消 —— 消除启动耗时带来的时序竞争
  bool act_started = false;
  for (int i = 0; i < 100 && !act_started; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (auto const& n : node_enters(tmp.dir, "s"))
      if (n == "act") act_started = true;
  }
  ASSERT_TRUE(act_started) << "act 节点未在 10s 内开始";
  ASSERT_TRUE(write_session_cancel(tmp.dir, "s", lock->pid, lock->run));

  runner.join();

  EXPECT_EQ(res.exit_code, 130) << res.output;
  EXPECT_NE(res.output.find("已取消"), std::string::npos) << res.output;
  EXPECT_FALSE(std::filesystem::exists(session_lock_path(tmp.dir, "s")));  // 锁已释放

  auto const nodes = node_enters(tmp.dir, "s");
  auto const has = [&](std::string const& n) {
    return std::find(nodes.begin(), nodes.end(), n) != nodes.end();
  };
  EXPECT_TRUE(has("act"));         // 慢节点确实执行了（取消发生在它之后）
  EXPECT_FALSE(has("after"));      // 取消点之后不再执行
  EXPECT_TRUE(has("__cancelled__"));  // 终态留痕

  clear_session_cancel(tmp.dir, "s");
}

// P2：插话（--on-busy=interject）—— 新 query 取消当前轮、等锁释放后自动接管会话。
TEST(WfInterjectE2E, new_query_interjects_and_takes_over) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP() << "wfr / libslow_tools.so 未构建";

  TempDir tmp;
  write_workflow(tmp.dir);
  write_quick_workflow(tmp.dir);
  seed_tool_call(tmp.dir, "s");

  cppchain::tool::ExecResult first;
  std::thread runner1([&] {
    first = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_slow", "--workdir", tmp.dir.string(), "--session", "s", "go"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });

  // 等第一轮进入 act（slow_sleep 3s）
  std::optional<SessionLockInfo> lock;
  for (int i = 0; i < 100 && !lock; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    lock = read_session_lock(tmp.dir, "s");
  }
  ASSERT_TRUE(lock.has_value()) << "第一轮未在 5s 内启动";
  bool act_started = false;
  for (int i = 0; i < 100 && !act_started; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (auto const& n : node_enters(tmp.dir, "s"))
      if (n == "act") act_started = true;
  }
  ASSERT_TRUE(act_started) << "act 未在 10s 内开始";

  // 第二轮：插话（会取消第一轮、等锁、再跑 wf_quick）
  cppchain::tool::ExecResult second;
  std::thread runner2([&] {
    second = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_quick", "--workdir", tmp.dir.string(), "--session", "s",
         "--on-busy=interject", "go2"},
        std::nullopt, {}, std::chrono::seconds(30), 1u << 20);
  });

  runner1.join();
  runner2.join();

  EXPECT_EQ(first.exit_code, 130) << first.output;   // 当前轮被取消
  EXPECT_EQ(second.exit_code, 0) << second.output;   // 插话轮正常完成
  EXPECT_NE(second.output.find("插话：已请求取消"), std::string::npos) << second.output;
  EXPECT_FALSE(std::filesystem::exists(session_lock_path(tmp.dir, "s")));

  auto const nodes = node_enters(tmp.dir, "s");
  auto const has = [&](std::string const& n) {
    return std::find(nodes.begin(), nodes.end(), n) != nodes.end();
  };
  EXPECT_TRUE(has("act"));
  EXPECT_TRUE(has("__cancelled__"));  // 第一轮取消留痕
  EXPECT_TRUE(has("e"));              // 第二轮（wf_quick）确实跑了
  EXPECT_FALSE(has("after"));         // 第一轮取消点之后不再执行

  clear_session_cancel(tmp.dir, "s");
}

// P2b：取消经由服务端传播到子进程（wfr cancel 目标是服务端 pid）。
TEST(WfCancelViaServerE2E, cancel_targets_server_and_propagates_to_child) {
  if (!std::filesystem::exists(wfr_bin()) || !std::filesystem::exists(slow_lib()))
    GTEST_SKIP() << "wfr / libslow_tools.so 未构建";

  TempDir tmp;
  write_workflow(tmp.dir);
  seed_tool_call(tmp.dir, "s");

  cppchain::tool::ExecResult server_res;
  std::thread server([&] {
    server_res = cppchain::tool::detail::run_argv(
        {wfr_bin().string(), "wf_slow", "--workdir", tmp.dir.string(), "--session", "s",
         "--lock-session", "--as-server", "go"},
        std::nullopt, {}, std::chrono::seconds(40), 1u << 20);
  });

  std::optional<SessionServerInfo> info;
  for (int i = 0; i < 100 && !info; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    info = read_server_info(tmp.dir, "s");
  }
  ASSERT_TRUE(info.has_value()) << "服务端未启动";
  bool act_started = false;
  for (int i = 0; i < 100 && !act_started; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (auto const& n : node_enters(tmp.dir, "s"))
      if (n == "act") act_started = true;
  }
  ASSERT_TRUE(act_started);

  // wfr cancel：读锁拿服务端 pid → 写 cancel → 服务端在派发循环里 SIGTERM 子进程
  auto const cc = cppchain::tool::detail::run_argv(
      {wfr_bin().string(), "cancel", "--workdir", tmp.dir.string(), "--session", "s",
       "--no-wait"},
      std::nullopt, {}, std::chrono::seconds(10), 1u << 20);
  EXPECT_EQ(cc.exit_code, 0) << cc.output;

  bool cancelled_seen = false;
  for (int i = 0; i < 100 && !cancelled_seen; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (auto const& n : node_enters(tmp.dir, "s"))
      if (n == "__cancelled__") cancelled_seen = true;
  }
  EXPECT_TRUE(cancelled_seen);
  for (auto const& n : node_enters(tmp.dir, "s")) EXPECT_NE(n, "after");

  ::kill(static_cast<pid_t>(info->pid), SIGTERM);
  server.join();
  EXPECT_FALSE(std::filesystem::exists(session_fifo_path(tmp.dir, "s")));
  EXPECT_FALSE(std::filesystem::exists(session_server_path(tmp.dir, "s")));
}
