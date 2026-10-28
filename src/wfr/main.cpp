// cppchain —— src/wfr/main.cpp
// wfr —— workflow runner（正式 CLI）：按名执行 workflow（绑定工作目录 + .wfr meta 目录）。
//
// 用法:
//   wfr <name-or-path> [--workdir DIR] [--session NAME] [--set k=v]... [--reset] [--wait]
//       [--allow-import] <query>...
//   wfr list [--workdir DIR]
//   wfr debug <name-or-path> [--session N|--all] [--out F] [--open]
//   wfr -h | --help
//
// <name>：内置名（chat：最简单聊天，model-only + load_history）优先；否则按
// `.wfr/workflows/<name>.json` 解析；含 / 或 *.json 且存在则当文件。
// query 作为本轮输入（input_messages，引擎写 __input__ 事件）；--set 注入 @config.* 键。
// trigger 可省略：无 trigger 的 workflow 由 wfr 手工拉起（D1）。
// 会话统一落 <workdir>/.wfr/sessions/。退出码：0 成功 / 2 用法・找不到・解析校验编译失败 /
// 1 fire 运行时错误。
#include <cppchain/http/curl_multi_transport.hpp>
#include <cppchain/message/serializer.hpp>  // Message ↔ JSON
#include <cppchain/message/utf8.hpp>        // 答案打印前净化断字节
#include <cppchain/model/openai/openai_chat_model.hpp>
#include <cppchain/state/checkpointer.hpp>  // wfr-㊸ @②：人审悬挂/恢复的进程内快照
#include <cppchain/tool/bash_tool.hpp>      // wfr-㊸ @①/@②：bash 工具（NeedHuman 人审）
#include <cppchain/tool/command_classify.hpp>  // wfr-㊸ ⑤：allow 前缀记录命令类别（封顶自动放行）
#include <cppchain/tool/calculator.hpp>
#include <cppchain/tool/datetime.hpp>
#include <cppchain/tool/exec_guard.hpp>
#include <cppchain/tool/file_tools.hpp>
#include <cppchain/tool/registry.hpp>
#include <cppchain/tool/sandbox.hpp>
#include <cppchain/tool/spawn_runner.hpp>  // wfr-㊿ cron：重进本进程跑批
#include <cppchain/tool/trip_db.hpp>  // B2-B 出行规划 DB 面  // wfr-㊸ @③：沙箱执行器选择（bash 后端）
#include <cppchain/tool/weather.hpp>
#include <cppchain/tool/web_search.hpp>
#include <cppchain/tool/skill_tools.hpp>  // wfr-context（⑤）：read_skill / wiki_search
#include <cppchain/tool/sketch_verify.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/components.hpp>
#include <cppchain/workflow/debug_view.hpp>
#include <cppchain/workflow/import.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/ir_walk.hpp>
#include <cppchain/workflow/session_channel.hpp>
#include <cppchain/workflow/session_lock.hpp>
#include <cppchain/workflow/session_log.hpp>
#include <cppchain/workflow/wfr.hpp>
#include <cppchain/workflow/wfr_lookup.hpp>
#include <cppchain/workflow/plugin_node.hpp>  // load_workflow_imports（动态库，v1 仅顶层）

#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>  // wfr-㊸ @②：人审 stdin
#include <map>
#include <memory>
#include <print>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>  // getpid（会话锁持有者标识）
#include <vector>

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::tool::make_calculator_tool;
using cppchain::tool::make_datetime_tool;
using cppchain::tool::make_sketch_verify_tool;
using cppchain::tool::make_file_read_tool;
using cppchain::tool::make_file_grep_tool;
using cppchain::tool::make_file_extract_tool;
using cppchain::tool::make_file_sample_tool;
using cppchain::tool::make_list_dir_tool;
using cppchain::tool::make_weather_tool;
using cppchain::tool::make_web_search_tool;
using cppchain::tool::make_read_skill_tool;
using cppchain::tool::make_wiki_search_tool;
using cppchain::wfr::ca_bundle_from_env;
using cppchain::wfr::env_or;
using cppchain::wfr::load_dotenv;

namespace {

// problems/07 §8 P1：SIGINT/SIGTERM → 节点边界优雅取消（handler 只做原子写，不碰 I/O）
std::atomic<bool> g_cancel_requested{false};
extern "C" void handle_cancel_signal(int) { g_cancel_requested.store(true); }


// --stream 的辅助：StreamEvent → JSON（node_start/node_end/interrupt/values + payload）
nlohmann::json ev_json(cppchain::graph::StreamEvent const& ev) {
  return nlohmann::json{{"type", ev.type}, {"node", ev.node}, {"node_kind", ev.node_kind},
                        {"step", ev.step}, {"payload", ev.payload}, {"ok", ev.ok}};
}

std::string last_ai_text(Json const& state) {
  if (!state.contains("messages") || !state["messages"].is_array()) return {};
  auto const& arr = state["messages"];
  for (auto it = arr.rbegin(); it != arr.rend(); ++it) {
    if (it->value("type", "") != "ai") continue;
    std::string out;
    for (auto const& blk : (*it)["content"])
      if (blk.value("type", "") == "text") out += blk.value("text", "");
    return out;
  }
  return {};
}

// 内置名（chat…）优先；否则路径/按名。
// 返回 {kind: builtin|file, ir/路径} —— 现仅内置直接命中返回；文件走 resolve_workflow_path。
// 发布自带的固定 workflow 目录（排在查找末尾）：
//   1) WFR_WORKFLOWS_DIR 环境
//   2) 安装布局：<bin>/../share/wfr/workflows（从 /proc/self/exe 推断，搬到别处仍有效）
//   3) 编译期宏（开发构建 = 源码 share/wfr/workflows）兜底
std::vector<std::string> publish_workflow_dirs() {
  std::vector<std::string> out;
  if (char const* env = std::getenv("WFR_WORKFLOWS_DIR"); env && *env) out.push_back(env);
  {
    std::error_code ec;
    auto const exe = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) {
      std::filesystem::path const share = exe.parent_path().parent_path() / "share" / "wfr" / "workflows";
      if (std::filesystem::is_directory(share, ec)) out.push_back(share.string());
    }
  }
  out.push_back(WFR_SHARE_WORKFLOWS_DIR);
  return out;
}

// 发布自带的定 tool 库目录（tool_registry 按 `lib<from>.so` 在此找）：三档，仿 publish_workflow_dirs：
//   1) WFR_TOOL_LIB_DIRS 环境（':' 分隔多目录）
//   2) 安装布局：<bin>/../lib（从 /proc/self/exe 推断）
//   3) 编译期宏（开发构建 = tests 目录，由 tests/CMakeLists.txt 对 wfr 注入）兜底
std::vector<std::string> publish_tool_lib_dirs() {
  std::vector<std::string> out;
  if (char const* env = std::getenv("WFR_TOOL_LIB_DIRS"); env && *env) {
    std::string s(env);
    std::size_t pos = 0;
    while ((pos = s.find(':')) != std::string::npos) {
      if (!s.substr(0, pos).empty()) out.push_back(s.substr(0, pos));
      s.erase(0, pos + 1);
    }
    if (!s.empty()) out.push_back(s);
  }
  {
    std::error_code ec;
    auto const exe = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) {
      std::filesystem::path const lib = exe.parent_path().parent_path() / "lib";
      if (std::filesystem::is_directory(lib, ec)) out.push_back(lib.string());
    }
  }
  if (WFR_TOOL_LIB_DIRS != nullptr && WFR_TOOL_LIB_DIRS[0] != '\0')  // 编译宏兜底（空则跳过）
    out.push_back(WFR_TOOL_LIB_DIRS);
  return out;
}

// 按名/路径定位 workflow 文件：显式路径存在 → 之；否则按 lookup 约定
//（settings.workflow_dirs → ws → 全局 → 发布自带）名字查找。找不到 → nullopt。
std::optional<std::string> locate(std::string const& workdir, std::string const& spec,
                                  Json const& settings) {
  std::filesystem::path maybe(spec);
  bool const looks_path = spec.find('/') != std::string::npos || spec.ends_with(".json");
  std::string cand;
  if (looks_path) cand = (maybe.is_absolute() ? maybe : std::filesystem::path(workdir) / maybe).string();
  if (maybe.is_absolute()) cand = maybe.string();
  if (!cand.empty() && std::filesystem::exists(cand)) return cand;
  return wfr_lookup_workflow(workdir, spec, settings, publish_workflow_dirs());
}

// 递归扫（含 loop body）：迁移后 chat 的 model 节点在 loop 里，漏了就会把
// "需要 LLM" 的 workflow 误判成无需 key，运行到一半才炸。
bool has_model_node_in(Json const& nodes) {
  for (Json const& n : nodes) {
    if (!n.is_object()) continue;
    if (n.value("kind", "") == "model" || n.contains("model")) return true;
    if (n.contains("body") && n["body"].is_array() && has_model_node_in(n["body"])) return true;
  }
  return false;
}

bool has_model_node(Json const& ir_json) {
  return has_model_node_in(ir_json.value("nodes", Json::array()));
}

// ---- problems/07 §8 P2b：会话 C/S（FIFO 服务端/客户端）----

std::filesystem::path self_exe_path() {
  char buf[4096];
  ssize_t const n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  return std::filesystem::path(std::string(buf, static_cast<std::size_t>(n)));
}

// 取消来源：SIGINT/SIGTERM 标志，或 .cancel 标记明确指向 server_pid
bool cs_cancel_requested(std::string const& ws, std::string const& session, long server_pid) {
  if (g_cancel_requested.load(std::memory_order_relaxed)) return true;
  auto const req = read_session_cancel(ws, session);
  if (req.has_value() && req->pid == server_pid) {
    clear_session_cancel(ws, session);
    return true;
  }
  return false;
}

// 审批通道（--approve <unix socket>）：连上后发一条 approve_request，等一行决定。
// 连接/读失败 → nullopt（调用方回退 stdin）。
std::optional<Json> ask_approval_channel(std::string const& path, std::string const& node,
                                         Json const& payload) {
  if (path.empty()) return std::nullopt;
  int const fd = unix_connect(path);
  if (fd < 0) return std::nullopt;
  LineChannel ch(fd);
  Json const req{{"kind", "approve_request"},
                 {"node", node},
                 {"payload", payload}};
  if (!ch.write_line(req.dump())) return std::nullopt;
  std::string line;
  if (!ch.read_line(line, 600000)) return std::nullopt;  // ≤10min
  Json const r = Json::parse(line, nullptr, false);
  if (r.is_discarded() || !r.is_object()) return std::nullopt;
  return std::optional<Json>(r);
}

std::vector<std::string> cs_child_argv(std::string const& workflow, std::string const& workdir,
                                       std::string const& session,
                                       std::map<std::string, std::string> const& sets,
                                       bool stream, std::string const& approve,
                                       std::string const& query) {
  std::vector<std::string> argv{self_exe_path().string(), workflow, "--workdir", workdir,
                                "--session", session, "--no-session-lock"};
  for (auto const& [k, v] : sets) {
    argv.push_back("--set");
    argv.push_back(k + "=" + v);
  }
  if (stream) argv.push_back("--stream");
  if (!approve.empty()) {
    argv.push_back("--approve");
    argv.push_back(approve);
  }
  argv.push_back(query);
  return argv;
}

// fork/exec 子进程，逐行读 stdout（relay），期间轮询取消（标记/信号）→ SIGTERM 子进程。
int cs_spawn_relay(std::vector<std::string> const& argv, std::string const& ws,
                   std::string const& session, long server_pid,
                   std::function<void(std::string const&)> const& on_line,
                   std::function<bool()> const& extra_cancel, bool& cancelled) {
  int p[2];
  if (::pipe(p) != 0) return -1;
  ::fcntl(p[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(p[1], F_SETFD, FD_CLOEXEC);
  pid_t const pid = ::fork();
  if (pid == 0) {
    int const devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
    ::dup2(p[1], STDOUT_FILENO);  // dup2 会清 FD_CLOEXEC
    ::close(p[0]);
    ::close(p[1]);
    if (devnull > 2) ::close(devnull);
    std::vector<char*> av;
    for (auto const& s : argv) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    ::execvp(av[0], av.data());
    ::_exit(127);
  }
  ::close(p[1]);
  if (pid < 0) {
    ::close(p[0]);
    return -1;
  }
  std::string buf;
  bool killed = false;
  for (;;) {
    struct pollfd pf{p[0], POLLIN, 0};
    int const pr = ::poll(&pf, 1, 200);
    if (pr < 0 && errno == EINTR) continue;
    if (pr > 0) {
      char tmp[4096];
      ssize_t const n = ::read(p[0], tmp, sizeof(tmp));
      if (n > 0) {
        buf.append(tmp, static_cast<std::size_t>(n));
        std::size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
          on_line(buf.substr(0, pos));
          buf.erase(0, pos + 1);
        }
        continue;
      }
      if (n == 0) break;
      if (errno == EAGAIN || errno == EINTR) continue;
      break;
    }
    if (!killed && (cs_cancel_requested(ws, session, server_pid) ||
                    (extra_cancel && extra_cancel()))) {
      ::kill(pid, SIGTERM);
      killed = true;
      cancelled = true;
    }
  }
  if (!buf.empty()) on_line(buf);
  ::close(p[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

// 执行一轮 query（子进程），把 stdout 行 relay 给本地 stdout / 回包 FIFO。
int cs_dispatch(std::string const& workflow, std::string const& workdir,
                std::string const& session, std::map<std::string, std::string> const& sets,
                bool stream, std::string const& query, int reply_fd, bool local,
                long server_pid, std::string const& approve = {},
                std::function<bool()> const& extra_cancel = {}) {
  bool cancelled = false;
  auto const argv = cs_child_argv(workflow, workdir, session, sets, stream, approve, query);
  int const code = cs_spawn_relay(
      argv, workdir, session, server_pid,
      [&](std::string const& line) {
        if (local) {
          std::cout << line << '\n';
          std::cout.flush();
        }
        if (reply_fd >= 0)
          (void)write_fifo_line(reply_fd, Json{{"type", "out"}, {"line", line}}.dump());
      },
      extra_cancel, cancelled);
  if (reply_fd >= 0)
    (void)write_fifo_line(reply_fd,
                    Json{{"type", "end"}, {"exit", code}, {"cancelled", cancelled}}.dump());
  return code;
}

int cs_run_server(std::string const& workdir, std::string const& session,
                  std::string const& workflow, std::map<std::string, std::string> const& sets,
                  bool stream, bool stay_alive, int idle_timeout_s, int max_queue,
                  std::string const& approve_path_in, std::string const& own_query) {
  long const self_pid = static_cast<long>(::getpid());
  auto const lk = acquire_session_lock(workdir, session, "server-" + std::to_string(self_pid));
  if (!lk.acquired) {
    if (lk.holder)
      std::println("会话 '{}' 已被 pid={} run={} 占用，无法成为服务端", session, lk.holder->pid,
                   lk.holder->run);
    else
      std::println("会话锁获取失败：{}", lk.error.empty() ? "未知原因" : lk.error);
    return 2;
  }
  struct Cleanup {
    std::string ws, session;
    std::filesystem::path fifo;
    std::filesystem::path approve_sock;
    ~Cleanup() {
      remove_session_fifo(fifo);
      if (!approve_sock.empty()) ::unlink(approve_sock.c_str());
      clear_server_info(ws, session);
      release_session_lock(ws, session);
    }
  } cleanup{workdir, session, session_fifo_path(workdir, session), {}};

  if (!ensure_session_fifo(cleanup.fifo)) {
    std::println("无法创建会话 FIFO：{}", cleanup.fifo.string());
    return 2;
  }
  // 审批通道：--approve 指定外部通道 → 直接透传给子进程；
  // 未指定 → 服务端自己建一个 UNIX socket，做"客户端 → 服务端 stdin"的代理。
  std::string approve = approve_path_in;
  int approve_listen = -1;
  bool const internal_approve = approve.empty();
  if (internal_approve) {
    cleanup.approve_sock = workdir + "/.wfr/sessions/session-" +
                           (session.empty() ? std::string("session") : "session-" + session) +
                           ".approve.sock";
    approve_listen = unix_listen(cleanup.approve_sock.string());
    if (approve_listen >= 0) approve = cleanup.approve_sock.string();
  }
  if (!write_server_info(workdir, session,
                         SessionServerInfo{self_pid, workflow,
                                           static_cast<long>(std::time(nullptr))})) {
    std::println("无法写服务端登记：{}", session_server_path(workdir, session).string());
    return 2;
  }
  std::signal(SIGINT, handle_cancel_signal);
  std::signal(SIGTERM, handle_cancel_signal);
  std::println("会话服务端已启动：pid={} session='{}' fifo={}{}", self_pid, session,
               cleanup.fifo.string(),
               stay_alive ? "（常驻，Ctrl-C 退出）" : "（本轮结束 + 500ms 后退出）");

  // 队列：读线程收请求 → 回 queued{position}/error；主线程串行执行（服务端一次只跑一轮）。
  struct Pending {
    Json req;
    int reply_fd = -1;
    bool local = false;
  };
  std::mutex mu;
  std::deque<Pending> queue;
  std::string current_id;                 // 正在执行的请求 id（取消 --request 用）
  std::atomic<bool> cancel_current{false};
  std::atomic<bool> stop_reader{false};
  int const cap = max_queue > 0 ? max_queue : 32;
  {
    std::lock_guard<std::mutex> lk2(mu);
    if (!own_query.empty())
      queue.push_back(Pending{Json{{"query", own_query},
                                   {"stream", stream},
                                   {"workflow", workflow},
                                   {"sets", Json(sets)}},
                              -1, true});
  }
  // 审批中转：客户端应答通过 FIFO 控制消息回来；无客户端应答时回退服务端 stdin。
  std::mutex ap_mu;
  std::condition_variable ap_cv;
  std::map<std::string, Json> ap_resp;
  std::string ap_current_id;
  int ap_reply_fd = -1;
  std::thread approver;
  if (approve_listen >= 0) {
    approver = std::thread([&] {
      while (!stop_reader) {
        int const c = unix_accept(approve_listen, 200);
        if (c < 0) continue;
        LineChannel ch(c);
        std::string line;
        if (!ch.read_line(line, 5000)) continue;
        Json const req = Json::parse(line, nullptr, false);
        if (req.is_discarded() || !req.is_object()) continue;
        std::string id;
        int rfd = -1;
        {
          std::lock_guard<std::mutex> lk2(ap_mu);
          id = ap_current_id;
          rfd = ap_reply_fd;
        }
        Json decision;
        bool got = false;
        if (rfd >= 0 && !id.empty()) {
          (void)write_fifo_line(
              rfd, Json{{"type", "approve"},
                        {"request", id},
                        {"node", req.value("node", "")},
                        {"payload", req.value("payload", Json::object())}}
                       .dump());
          std::unique_lock<std::mutex> lk2(ap_mu);
          if (ap_cv.wait_for(lk2, std::chrono::seconds(120),
                             [&] { return ap_resp.contains(id) || stop_reader.load(); })) {
            auto it = ap_resp.find(id);
            if (it != ap_resp.end()) {
              decision = it->second;
              ap_resp.erase(it);
              got = true;
            }
          }
        }
        if (got && decision.value("action", "") == "fallback") got = false;
        if (!got) {  // 回退服务端 stdin（EOF → reject，fail-closed）
          std::println();
          std::println("⏸ 审批回退到服务端 stdin（节点 {}）：", req.value("node", "?"));
          std::print("  批准？[approve | reject <理由> | abort]：");
          std::cout.flush();
          std::string input;
          if (!std::getline(std::cin, input)) {
            decision = Json{{"action", "reject"}, {"feedback", "服务端 stdin 已结束"}};
          } else {
            std::string const t = cppchain::wfr::trim(input);
            if (t == "y" || t.starts_with("approve")) decision = Json{{"action", "approve"}};
            else if (t.starts_with("reject")) {
              decision = Json{{"action", "reject"},
                              {"feedback", cppchain::wfr::trim(t.substr(6))}};
            } else if (t == "n") decision = Json{{"action", "reject"}};
            else decision = Json{{"action", "abort"}};
          }
        }
        (void)ch.write_line(Json{{"action", decision.value("action", "reject")},
                                 {"feedback", decision.value("feedback", "")}}
                                .dump());
      }
    });
  }

  std::thread reader([&] {
    FifoLineReader r;
    if (!r.open(cleanup.fifo, /*keep_write_end=*/true)) return;
    while (!stop_reader) {
      std::string line;
      if (!r.read_line(line, 200)) continue;
      Json const req = Json::parse(line, nullptr, false);
      if (req.is_discarded() || !req.is_object()) continue;
      if (req.value("type", "") == "approve_response") {
        std::string const id = req.value("request", "");
        {
          std::lock_guard<std::mutex> lk2(ap_mu);
          ap_resp[id] = req;
        }
        ap_cv.notify_all();
        continue;
      }
      // 控制消息：取消某个排队/执行中的请求
      if (req.value("type", "") == "cancel") {
        std::string const target = req.value("request", "");
        if (target.empty()) continue;
        bool found = false;
        {
          std::lock_guard<std::mutex> lk2(mu);
          for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (it->req.value("id", "") == target) {
              if (it->reply_fd >= 0) {
                (void)write_fifo_line(
                    it->reply_fd,
                    Json{{"type", "end"}, {"exit", 130}, {"cancelled", true}}.dump());
                ::close(it->reply_fd);
              }
              queue.erase(it);
              found = true;
              break;
            }
          }
          if (!found && current_id == target) cancel_current = true;
        }
        continue;
      }
      if (req.value("query", "").empty()) continue;
      std::string const reply = req.value("reply", "");
      int rfd = -1;
      if (!reply.empty()) {
        rfd = ::open(reply.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (rfd < 0) continue;  // 客户端已经走了
      }
      std::size_t pos = 0;
      bool full = false;
      {
        std::lock_guard<std::mutex> lk2(mu);
        if (queue.size() >= static_cast<std::size_t>(cap)) {
          full = true;
        } else {
          pos = queue.size() + 1;
          queue.push_back(Pending{req, rfd, false});
        }
      }
      if (full) {
        if (rfd >= 0) {
          (void)write_fifo_line(rfd, Json{{"type", "error"}, {"msg", "queue full"}}.dump());
          ::close(rfd);
        }
        continue;
      }
      if (rfd >= 0)
        (void)write_fifo_line(rfd, Json{{"type", "queued"}, {"position", pos}}.dump());
    }
  });

  using clock = std::chrono::steady_clock;
  auto const idle_span = stay_alive
                             ? (idle_timeout_s > 0 ? std::chrono::seconds(idle_timeout_s)
                                                   : std::chrono::hours(24 * 365))
                             : std::chrono::milliseconds(500);
  auto deadline = clock::now() + idle_span;
  int rc = 0;
  while (!g_cancel_requested) {
    Pending p;
    bool have = false;
    {
      std::lock_guard<std::mutex> lk2(mu);
      if (!queue.empty()) {
        p = std::move(queue.front());
        queue.pop_front();
        current_id = p.req.value("id", "");
        cancel_current = false;
        {
          std::lock_guard<std::mutex> alk(ap_mu);
          ap_current_id = current_id;
          ap_reply_fd = p.reply_fd;
          ap_resp.erase(current_id);
        }
        have = true;
      }
    }
    if (!have) {
      if (clock::now() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    std::string const q = p.req.value("query", "");
    if (q.empty()) continue;
    bool const req_stream = p.req.value("stream", stream);
    std::string wf = p.req.value("workflow", workflow);
    if (wf.empty()) wf = workflow;
    std::map<std::string, std::string> merged = sets;
    if (p.req.contains("sets") && p.req["sets"].is_object())
      for (auto const& [k, v] : p.req["sets"].items())
        if (v.is_string()) merged[k] = v.get<std::string>();
    int const code = cs_dispatch(wf, workdir, session, merged, req_stream, q, p.reply_fd,
                                 p.local, self_pid, approve,
                                 [&] { return cancel_current.load(); });
    if (p.reply_fd >= 0) ::close(p.reply_fd);
    if (p.local) rc = code;
    {
      std::lock_guard<std::mutex> lk2(mu);
      current_id.clear();
      cancel_current = false;
      std::lock_guard<std::mutex> alk(ap_mu);
      ap_current_id.clear();
      ap_reply_fd = -1;
    }
    deadline = clock::now() + idle_span;
  }
  stop_reader = true;
  ap_cv.notify_all();
  if (reader.joinable()) reader.join();
  if (approver.joinable()) approver.join();
  if (approve_listen >= 0) ::close(approve_listen);
  {
    std::lock_guard<std::mutex> lk2(mu);
    for (auto& p : queue) {
      if (p.reply_fd >= 0) {
        (void)write_fifo_line(p.reply_fd,
                        Json{{"type", "error"}, {"msg", "server stopping"}}.dump());
        ::close(p.reply_fd);
      }
    }
    queue.clear();
  }
  return rc;
}

bool cs_send_control(std::string const& workdir, std::string const& session, Json const& msg) {
  int const fd = ::open(session_fifo_path(workdir, session).c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  bool const ok = write_fifo_line(fd, msg.dump());
  ::close(fd);
  return ok;
}

int cs_run_client(std::string const& workdir, std::string const& session,
                  std::string const& workflow,
                  std::map<std::string, std::string> const& sets, bool stream, bool post,
                  std::string const& request_id, std::string const& query) {
  auto const info = read_server_info(workdir, session);
  if (!info.has_value() || !session_server_alive(workdir, session)) {
    std::println("会话 '{}' 没有运行中的服务端（先用 --lock-session/--as-server 启动）", session);
    return 2;
  }
  long const self_pid = static_cast<long>(::getpid());
  std::filesystem::path reply;
  std::unique_ptr<FifoLineReader> reader;
  if (!post) {
    reply = session_reply_fifo_path(workdir, session, self_pid, 0);
    (void)ensure_session_fifo(reply);
    reader = std::make_unique<FifoLineReader>();
    if (!reader->open(reply, /*keep_write_end=*/false)) {
      std::println("无法打开回包 FIFO：{}", reply.string());
      return 2;
    }
  }
  int const wfd = ::open(session_fifo_path(workdir, session).c_str(), O_WRONLY | O_CLOEXEC);
  if (wfd < 0) {
    std::println("无法连接服务端 FIFO：{}", session_fifo_path(workdir, session).string());
    if (!post) remove_session_fifo(reply);
    return 2;
  }
  std::string const id = request_id.empty() ? std::to_string(self_pid) + "-0" : request_id;
  Json const req{{"id", id},
                 {"query", query},
                 {"workflow", workflow},
                 {"reply", post ? std::string{} : reply.string()},
                 {"stream", stream},
                 {"sets", Json(sets)}};
  bool const sent = write_fifo_line(wfd, req.dump());
  ::close(wfd);
  if (!sent) {
    std::println("发送请求失败（服务端 FIFO 关闭？）");
    if (!post) remove_session_fifo(reply);
    return 2;
  }
  if (post) {
    std::println("已投递到会话服务端（pid={}，不等待结果）", info->pid);
    return 0;
  }
  int rc = 0;
  for (;;) {
    std::string line;
    if (!reader->read_line(line, 600000)) {
      std::println("等待服务端响应超时/服务端已退出");
      rc = 1;
      break;
    }
    Json const m = Json::parse(line, nullptr, false);
    if (m.is_discarded() || !m.is_object()) continue;
    std::string const type = m.value("type", "");
    if (type == "queued") {
      if (!stream) {
        std::println("排队第 {} 位…", m.value("position", 0));
        std::cout.flush();
      }
    } else if (type == "error") {
      std::println("服务端错误：{}", m.value("msg", "unknown"));
      rc = 1;
      break;
    } else if (type == "approve") {
      std::string const rid = m.value("request", id);
      if (stream) {
        // Web/CGI：把审批事件原样转发给浏览器；决定由前端 POST
        // action=approve → 服务端 FIFO 的 approve_response 回传（不在这里替用户决定）。
        std::cout << line << '\n';
        std::cout.flush();
        continue;
      }
      std::string action = "fallback";  // 非交互客户端（EOF）→ 回退服务端 stdin
      std::string feedback;
      {
        Json const payload = m.value("payload", Json::object());
        std::println();
        std::println("⏸ 待审批（节点 {}）：", m.value("node", "?"));
        if (payload.contains("calls") && payload["calls"].is_array()) {
          for (Json const& c : payload["calls"])
            std::println("  · tool={} id={} args={}", c.value("tool", "?"), c.value("id", "?"),
                         c.value("args", Json::object()).dump());
        } else {
          std::println("  {}", payload.dump());
        }
        std::print("  批准？[approve | reject <理由> | abort]：");
        std::cout.flush();
        std::string line;
        if (std::getline(std::cin, line)) {
          std::string const t = cppchain::wfr::trim(line);
          if (t == "y" || t.starts_with("approve")) action = "approve";
          else if (t.starts_with("reject")) {
            action = "reject";
            feedback = cppchain::wfr::trim(t.substr(6));
          } else if (t == "n") action = "reject";
          else action = "abort";
        }
      }
      (void)cs_send_control(workdir, session,
                            Json{{"type", "approve_response"},
                                 {"request", rid},
                                 {"action", action},
                                 {"feedback", feedback}});
    } else if (type == "out") {
      std::cout << m.value("line", "") << '\n';
      std::cout.flush();
    } else if (type == "end") {
      rc = m.value("exit", 0);
      break;
    }
  }
  remove_session_fifo(reply);
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  std::string workdir = ".", session = "default", name_or_path, input;
  bool reset = false, allow_import = false;
  // problems/07 §8 P2：会话忙时的策略 reject（默认）/ wait / interject（取消当前轮后接管）
  std::string on_busy = "reject";
  // problems/07 §8 P2b：会话 C/S（FIFO）
  bool lock_session = false, as_server = false, as_client = false, post_only = false;
  bool no_session_lock = false;   // 内部：子进程不取锁（父服务端持有）
  int server_idle_timeout = 0;    // --as-server 空闲退出秒数（0 = 常驻）
  int server_max_queue = 32;      // 服务端请求队列上限（超出回 error）
  std::string request_id;         // --request-id：客户端请求标识（cancel --request 用）
  std::string approve_path;       // --approve：人工审批的 UNIX socket 通道
  bool stream_events = false;  // --stream：把引擎事件（node_start/node_end）逐条打 JSON 行到 stdout
  std::size_t max_parallel = 1;  // wfr-㊾ 批③：Send 并行波宽度（编译期选项）
  std::map<std::string, std::string> sets;

  auto usage = [] {
    std::println(
        "用法:\n"
        "  wfr <name-or-path> [--workdir DIR] [--session NAME] [--set key=value]... "
        "[--max-parallel N] [--reset] [--allow-import] [--stream] "
        "[--on-busy=reject|wait|interject] "
        "[--lock-session [--as-server|--as-client] [--post]] <query>...\n"
        "  wfr list [--workdir DIR]\n"
        "  wfr debug <name-or-path> [--session N|--all|--graph-only] [--out F] [--open]\n"
        "        --graph-only：只按 workflow 出页面（不内嵌任何会话），\n"
        "        打开后把 .wfr/sessions/*.log.jsonl 拖进左栏、或在左栏填日志 URL，即可看日志。\n"
        "  wfr stop [--workdir DIR] [--session NAME]\n"
        "        停止该会话的服务端或当前进行中的查询（SIGTERM → 节点边界优雅退出）。\n"
        "  wfr cancel [--workdir DIR] [--session NAME] [--no-wait] [--request ID]\n"
        "--on-busy（problems/07 §8 P2）：reject（默认，直接拒绝）/ wait（排队等待）/\n"
        "        interject（取消当前轮→等它停→接管会话跑新查询；等价 --wait + cancel）。\n"
        "会话 C/S（P2b）：--lock-session 首个进程成为服务端（持会话锁 + FIFO），\n"
        "        后来者自动/显式 --as-client 投递 query；--post 只投递不等待；\n"
        "        --as-server 常驻（--server-idle-timeout N 空闲退出；--server-max-queue N 队列上限）。\n"
        "        --request-id ID 指定请求标识（配合 wfr cancel --request ID）。\n"
        "        --approve <socket> 人工审批走 UNIX socket（无则转客户端；客户端不可用回退 stdin）。\n"
        "        请求取消当前轮：写 <session>.cancel，运行中的 wfr 在**节点边界**停止\n"
        "        （已完成节点保留在会话日志）；默认等待锁释放（≤30s）。\n"
        "  wfr -h | --help\n"
        "--max-parallel N（wfr-㊾ 批③）：Send 并行波宽度（编译期选项，默认 1=串行）。\n"
        "        >1 时 `parallel`/`executor` 编排的同一批 sends 在 worker 线程并发执行（波=屏障，\n"
        "        join 后按声明序归约），与串行最终 state 逐键相等；仅 mch_par 这类含 parallel 的\n"
        "        workflow 有意义。\n"
        "内置（发布自带 share/wfr/workflows）：chat（最简单聊天，多轮续聊；文件取数 "
        "file_context 预取 + **≤1 轮工具 + 强制收口**，--set file_route=on 开预取；需 LLM "
        "key，见 .env.example）；chat2（chat 的超集：历史渲染三档 + 思考回传/厂商参数开关，"
        "用 --set 切换 —— history_mode=full|drop_round|keep_evidence、"
        "reasoning_echo=drop|tool_calls|always、reasoning_effort=…）。\n"
        "副作用 demo：approve_bash_demo（bash 人审悬挂/恢复；--set allow_prefixes=on 会话级"
        "allow 前缀复用，--set approve_policy=allow|deny|deny:<理由> 可 mock 人工审批免交互）、"
        "sidefx_approve_demo（file_write deny 硬拦）、sidefx_mock_demo（可逆性梯度 mock："
        "订单幂等/微信窗口可撤/短信不可撤/电话任状态可关 + 模拟时钟；需 tool_registry 动态库，"
        "dev 构建自动命中，release 用 WFR_TOOL_LIB_DIRS env 指向 lib 目录）。\n"
        "<name> 查找顺序：显式路径 → <ws>/.wfr/settings.json 的 workflow_dirs → "
        "<ws>/.wfr/workflows → ~/.wfr/workflows → 发布自带。\n"
        "workspace 的 .wfr 路径本身不可被覆盖；settings 只能追加 workflow_dirs。\n"
        "--set 注入 @config.* 的额外键；保留名 debug/list/help 作为首参时为子命令。");
  };
  if (argc < 2) {
    usage();
    return 2;
  }
  std::string const cmd = argv[1];
  if (cmd == "-h" || cmd == "--help") {
    usage();
    return 0;
  }

  // ================= 子命令：cancel（取消当前轮，节点边界生效） =================
  if (cmd == "cancel") {
    std::string wd = ".", sess = "default", req_id;
    bool wait = true;
    for (int j = 2; j < argc; ++j) {
      std::string const a = argv[j];
      if (a == "--workdir" && j + 1 < argc) wd = argv[++j];
      else if (a == "--session" && j + 1 < argc) sess = argv[++j];
      else if (a == "--request" && j + 1 < argc) req_id = argv[++j];
      else if (a == "--no-wait") wait = false;
      else {
        std::println("cancel：未知参数 '{}'", a);
        return 2;
      }
    }
    if (!req_id.empty()) {  // 取消指定请求（排队中 → 直接回 end；执行中 → SIGTERM 子进程）
      if (!session_server_alive(wd, sess)) {
        std::println("会话 '{}' 没有运行中的服务端；--request 只用于 C/S 模式", sess);
        return 2;
      }
      int const fd = ::open(session_fifo_path(wd, sess).c_str(), O_WRONLY | O_CLOEXEC);
      if (fd < 0) { std::println("无法连接服务端 FIFO"); return 2; }
      bool const ok = write_fifo_line(fd, Json{{"type", "cancel"}, {"request", req_id}}.dump());
      ::close(fd);
      if (!ok) { std::println("发送取消请求失败"); return 2; }
      std::println("已请求取消请求 '{}'。", req_id);
      return 0;
    }
    auto const lock = read_session_lock(wd, sess);
    if (!lock.has_value()) {
      std::println("会话 '{}' 当前没有进行中的查询。", sess);
      return 0;
    }
    if (!write_session_cancel(wd, sess, lock->pid, lock->run)) {
      std::println("写取消标记失败：{}", session_cancel_path(wd, sess).string());
      return 2;
    }
    std::println("已请求取消会话 '{}'（pid={} run={}）；将在节点边界生效。", sess, lock->pid,
                 lock->run);
    if (wait) {
      for (int i = 0; i < 300; ++i) {  // ≤30s：等锁释放
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!read_session_lock(wd, sess).has_value()) {
          std::println("已取消（锁已释放）。");
          return 0;
        }
      }
      std::println("取消标记已写入，但 30s 内未观察到锁释放（可能正在长节点/模型调用中）。");
    }
    return 0;
  }

  // ================= 子命令：stop（停止会话服务端/当前持有者） =================
  if (cmd == "stop") {
    std::string wd = ".", sess = "default";
    for (int j = 2; j < argc; ++j) {
      std::string const a = argv[j];
      if (a == "--workdir" && j + 1 < argc) wd = argv[++j];
      else if (a == "--session" && j + 1 < argc) sess = argv[++j];
      else {
        std::println("stop：未知参数 '{}'", a);
        return 2;
      }
    }
    long pid = 0;
    if (auto const info = read_server_info(wd, sess); info.has_value() &&
                                                     session_server_alive(wd, sess))
      pid = info->pid;
    else if (auto const lk = read_session_lock(wd, sess); lk.has_value())
      pid = lk->pid;
    if (pid <= 0) {
      std::println("会话 '{}' 当前没有服务端/进行中的查询。", sess);
      return 0;
    }
    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
      std::println("发送 SIGTERM 失败（pid={}）：{}", pid, std::strerror(errno));
      return 2;
    }
    std::println("已请求停止会话 '{}'（pid={}）；等待退出…", sess, pid);
    for (int i = 0; i < 100; ++i) {  // ≤10s
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      bool const server_gone = !read_server_info(wd, sess).has_value();
      bool const lock_gone = !read_session_lock(wd, sess).has_value();
      if (server_gone && lock_gone) {
        std::println("已停止。");
        return 0;
      }
    }
    std::println("10s 内未完全退出（可能卡在长节点/模型调用中）。");
    return 0;
  }

  // ================= 子命令：debug（可视化调试台） =================
  if (cmd == "debug") {
    std::string session_arg, out_file;
    bool all = false, open = false, graph_only = false;
    if (argc < 3) {
      usage();
      return 2;
    }
    std::string const dname = argv[2];
    for (int j = 3; j < argc; ++j) {
      std::string const a = argv[j];
      if (a == "--workdir" && j + 1 < argc) workdir = argv[++j];
      else if (a == "--session" && j + 1 < argc) session_arg = argv[++j];
      else if (a == "--all") all = true;
      else if (a == "--graph-only") graph_only = true;   // 只按 workflow 出图，日志靠页面拖入
      else if (a == "--out" && j + 1 < argc) out_file = argv[++j];
      else if (a == "--open") open = true;
      else if (a == "-h" || a == "--help") {
        usage();
        return 0;
      } else {
        std::println("未知 debug 选项: {}", a);
        return 2;
      }
    }
    if (session_arg.empty() && !all && !graph_only) all = true;

    Json dir_json;
    Json const settings = wfr_read_settings(workdir);
    auto dpath = locate(workdir, dname, settings);
    if (!dpath) {
      std::println("找不到 workflow '{}'（按 settings.workflow_dirs → <ws>/.wfr/workflows → "
                   "~/.wfr/workflows → 发布自带 查找）", dname);
      return 2;
    }
    {
      std::ifstream din(*dpath);
      if (!din) {
        std::println("找不到 workflow '{}'（searched: {}）", dname, *dpath);
        return 2;
      }
      try {
        din >> dir_json;
      } catch (std::exception const& e) {
        std::println("workflow JSON 解析失败: {}", e.what());
        return 2;
      }
      if (dir_json.contains("imports"))
        if (!load_workflow_imports(dir_json, true, nullptr)) {
          std::println("动态库 import 失败");
          return 2;
        }
      if (dir_json.contains("import")) {
        auto expanded = expand_workflow_imports(dir_json, workdir);
        if (!expanded) {
          std::println("workflow import 展开失败: {}", expanded.error());
          return 2;
        }
      }
    }
    auto dir_ir = WorkflowIR::parse(dir_json);
    if (!dir_ir) {
      std::println("IR 解析失败: {}", dir_ir.error());
      return 2;
    }

    std::vector<Json> recs;   // --graph-only 时保持为空：页面只出图，日志靠拖入
    if (graph_only) {
      // 只按 workflow 出页面：不内嵌任何会话，打开后把 .log.jsonl 拖进去即可
    } else if (all) {
      std::error_code ec;
      for (std::filesystem::directory_iterator it(wfr_root(workdir) / "sessions", ec), end;
           it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) ||
            !it->path().filename().string().ends_with(".log.jsonl"))
          continue;
        SessionLog slog(it->path().string());
        // 每个会话的 run 都从 r-1 起编号 —— 跨会话会撞车：run 前缀「会话名」，轮次才不会并组
        std::filesystem::path stem = it->path().filename();
        stem.replace_extension("");
        stem.replace_extension("");
        std::string const sess = stem.string();
        for (auto& r : slog.records()) {
          r["session"] = sess;
          std::string const run = r.value("run", "");
          if (!run.empty() && run.find(sess + "/") != 0) r["run"] = sess + "/" + run;
          recs.push_back(std::move(r));
        }
      }
    } else {
      SessionLog slog(session_path(workdir, session_arg));
      recs = slog.records();
      // 与 --all 分支对齐：run 带上会话前缀。否则前端"会话/轮次"二级选择器按
      // run.split('/')[0] 分桶，每个 r-N 都会被当成一个独立会话（下拉里显示整串）。
      for (Json& r : recs) {
        r["session"] = session_arg;
        std::string const run = r.value("run", "");
        if (!run.empty() && run.find(session_arg + "/") != 0) r["run"] = session_arg + "/" + run;
      }
    }

    Json const payload = build_debug_payload(*dir_ir, recs);
    std::string const html = render_debug_html(payload);
    std::string const out =
        out_file.empty() ? (wfr_root(workdir) / "debug" / (dname + ".html")).string() : out_file;
    {
      std::filesystem::create_directories(std::filesystem::path(out).parent_path());
      std::ofstream o(out, std::ios::trunc);
      o << html;
    }
    std::println("调试台已生成: {}  ({} bytes, {} 条日志记录)", out, html.size(), recs.size());
    if (open) [[unlikely]] {
      std::string const cmdline = "xdg-open \"" + out + "\" >/dev/null 2>&1 &";
      [[maybe_unused]] int const sys_rc = std::system(cmdline.c_str());
    }
    return 0;
  }

  // ---- 解析选项（首参=workflow 名/路径；其余 --opt 与 query）----
  int i = 2;
  if (cmd != "list") name_or_path = cmd;
  for (; i < argc; ++i) {
    std::string const a = argv[i];
    if (a == "--workdir" && i + 1 < argc) workdir = argv[++i];
    else if (a == "--session" && i + 1 < argc) session = argv[++i];
    else if (a == "--set" && i + 1 < argc) {
      std::string const kv = argv[++i];
      auto const eq = kv.find('=');
      sets[kv.substr(0, eq)] = eq == std::string::npos ? std::string{} : kv.substr(eq + 1);
    } else if (a == "--reset") reset = true;
    else if (a == "--wait") on_busy = "wait";  // 兼容旧写法 = --on-busy=wait
    else if (a == "--lock-session") lock_session = true;
    else if (a == "--as-server") { as_server = true; lock_session = true; }
    else if (a == "--as-client") { as_client = true; lock_session = true; }
    else if (a == "--post") post_only = true;
    else if (a == "--no-session-lock") no_session_lock = true;
    else if (a == "--server-idle-timeout" && i + 1 < argc) {
      try { server_idle_timeout = std::stoi(argv[++i]); }
      catch (std::exception const&) { std::println("--server-idle-timeout 需要整数秒"); return 2; }
    }
    else if (a == "--request-id" && i + 1 < argc) request_id = argv[++i];
    else if (a == "--approve" && i + 1 < argc) approve_path = argv[++i];
    else if (a.starts_with("--approve=")) approve_path = a.substr(std::string("--approve=").size());
    else if (a == "--server-max-queue" && i + 1 < argc) {
      try { server_max_queue = std::stoi(argv[++i]); }
      catch (std::exception const&) { std::println("--server-max-queue 需要整数"); return 2; }
    }
    else if (a == "--on-busy" && i + 1 < argc) on_busy = argv[++i];
    else if (a.starts_with("--on-busy=")) on_busy = a.substr(std::string("--on-busy=").size());
    else if (a == "--allow-import") allow_import = true;
    else if (a == "--stream") stream_events = true;
    else if (a == "--max-parallel" && i + 1 < argc) {
      // 编译期并行度（不写 IR；host 决定）。非法值 → 报错（fail-closed，不用 0 当串行掩盖）。
      try { max_parallel = std::stoul(argv[++i]); }
      catch (std::exception const&) {
        std::println("--max-parallel 需要正整数，收到 '{}'", argv[i]);
        return 2;
      }
      if (max_parallel == 0) { std::println("--max-parallel 需要 ≥1（1=串行，默认）"); return 2; }
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else
      input = input.empty() ? a : input + " " + a;
  }

  // ---- list：按查找约定列出各来源目录的 workflow ----
  if (cmd == "list") {
    Json const settings = wfr_read_settings(workdir);
    std::vector<std::string> dirs = publish_workflow_dirs();  // 发布自带（内置/固定）
    auto base = wfr_lookup_dirs(workdir, settings);           // settings → ws → 全局
    dirs.insert(dirs.end(), base.begin(), base.end());
    // 去重（按绝对规整路径）
    std::vector<std::string> seen, unique;
    for (auto const& d : dirs) {
      std::string const a = std::filesystem::absolute(d).lexically_normal().string();
      if (std::find(seen.begin(), seen.end(), a) == seen.end()) {
        seen.push_back(a);
        unique.push_back(d);
      }
    }
    for (auto const& d : unique) {
      auto names = wfr_list_workflows_in(d);
      if (names.empty()) continue;
      std::println("{}:", d);
      for (std::string const& n : names) {
        std::ifstream in(std::filesystem::path(d) / (n + ".json"));
        Json j;
        try {
          in >> j;
        } catch (...) {
          std::println("  {:<14} （读不到/坏 JSON）", n);
          continue;
        }
        std::string trig;
        if (j.contains("trigger") && j["trigger"].is_object()) {
          Json const& timing = j["trigger"].value("timing", Json::object());
          trig = "schedule " + timing.value("schedule", "");
        } else {
          trig = "manual（手工触发）";
        }
        std::println("  {:<14} nodes={}  trigger={}", n,
                     j.value("nodes", Json::array()).size(), trig);
      }
    }
    return 0;
  }

  // 批③：轻量常驻调度（wfr-㊿）—— 读 .wfr/cron.json 任务表，到期重进本进程跑一个 lot 批；--once = 跑一轮即收。
  if (cmd == "cron") {
    bool once = false;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--once") once = true;
    auto self = [&]() -> std::string {
      std::error_code ec; auto const e = std::filesystem::read_symlink("/proc/self/exe", ec);
      return ec ? std::string("wfr") : e.string();
    }();
    long sleep_secs = 10;
    long max_rounds = 0;  // 0 = 常驻
    for (int i = 2; i + 1 < argc; ++i) {
      std::string const a = argv[i];
      if (a == "--sleep-secs" && i + 2 < argc) sleep_secs = std::stol(argv[++i]);
      if (a == "--max-rounds" && i + 2 < argc) max_rounds = std::stol(argv[++i]);
    }
    long rounds = 0;
    for (;;) {
      std::filesystem::path const cj = std::filesystem::path(workdir) / ".wfr" / "cron.json";
      Json jobs = Json::array();
      if (std::ifstream in(cj); in) { try { in >> jobs; jobs = jobs.value("jobs", Json::array()); } catch (...) {} }
      long const now = static_cast<long>(std::time(nullptr));
      bool fired = false;
    for (auto& job : jobs) {
      if (!job.is_object()) continue;
      std::string const T = job.value("task", "");
      if (T.empty()) continue;
      long const every = job.value("every_minutes", 0L);
      long const last = job.value("last_run", 0L);
      if (every <= 0 || now - last < every * 60) continue;
      // arch 27 §十：project 感知装配（job.project → lot_project；否则 lot 现状兼容）
      WfrCronBatch const batch = wfr_cron_batch(workdir, job, self);
      if (batch.args.empty()) continue;  // task 缺失
      std::println("⏱ cron ➜ 触发批次 workflow={} task={} session={}", batch.workflow,
                   batch.task, batch.session);
      auto const spawned = cppchain::tool::detail::run_argv(
          batch.args, std::nullopt, cppchain::tool::detail::SpawnLimits{},
          std::chrono::minutes(30), 1u << 20);
      if (spawned.exit_code != 0)  // 拉起失败不丢现场，但视为已触发（沿用原语义）
        std::println("⚠ cron ➜ 批次拉起失败（exit={}）{}", spawned.exit_code,
                     spawned.note.empty() ? spawned.output : spawned.note);
      job["last_run"] = now;
      fired = true;
    }
    if (fired) {
      std::error_code ec; std::filesystem::create_directories(cj.parent_path(), ec);
      if (std::ofstream out(cj); out) out << Json{{"jobs", jobs}}.dump(2);
    }
      ++rounds;
      std::println("cron 轮次 {} 完成（sleep {}s）", rounds, sleep_secs);
      if (once || (max_rounds > 0 && rounds >= max_rounds)) return 0;
      std::this_thread::sleep_for(std::chrono::seconds(sleep_secs));
    }
  }

  if (on_busy != "reject" && on_busy != "wait" && on_busy != "interject") {
    std::println("--on-busy 只支持 reject | wait | interject，收到 '{}'", on_busy);
    return 2;
  }
  if (post_only && !lock_session) {
    std::println("--post 需要配合 --lock-session / --as-client");
    return 2;
  }

  // ---- problems/07 §8 P2b：会话 C/S（FIFO） ----
  if (lock_session) {
    if (as_client) {
      if (input.empty()) {
        std::println("--as-client 需要一条 query");
        return 2;
      }
      return cs_run_client(workdir, session, name_or_path, sets, stream_events, post_only, request_id, input);
    }
    if (session_server_alive(workdir, session)) {
      if (as_server) {
        std::println("会话 '{}' 已有服务端在运行；要接管请先让它退出（Ctrl-C / kill）", session);
        return 2;
      }
      if (input.empty()) {
        std::println("会话服务端已在运行：请给出 query（自动作为客户端投递）");
        return 2;
      }
      return cs_run_client(workdir, session, name_or_path, sets, stream_events, post_only, request_id, input);
    }
    return cs_run_server(workdir, session, name_or_path, sets, stream_events, as_server,
                         server_idle_timeout, server_max_queue, approve_path, input);
  }
  if (as_server || as_client || post_only) {
    std::println("--as-server / --as-client / --post 需要配合 --lock-session");
    return 2;
  }

  // ---- problems/07 §8 P0：会话单飞锁 ----
  // 同一 session 同一时刻只允许一轮 query：默认 busy 直接拒绝（fail-closed）；
  // --wait 则轮询等待（≤600s）。锁在进程退出/异常时由 RAII 释放；崩溃残留由 stale 回收。
  struct SessionLockReleaser {
    std::string ws;
    std::string session;
    bool held = false;
    ~SessionLockReleaser() {
      if (held) release_session_lock(ws, session);
    }
  } lock_releaser{workdir, session, false};
  long const self_pid = static_cast<long>(::getpid());
  // P1：Ctrl-C / SIGTERM → 节点边界优雅取消（不是硬杀）
  std::signal(SIGINT, handle_cancel_signal);
  std::signal(SIGTERM, handle_cancel_signal);
  std::function<bool()> should_stop;
  if (!no_session_lock) {  // C/S 子进程：父服务端已持锁
    std::string const lock_run = "cli-" + std::to_string(self_pid);
    bool interjected = false;
    SessionLockAcquire lk = acquire_session_lock(workdir, session, lock_run);
    if (!lk.acquired && lk.holder) {
      if (on_busy == "reject") {
        std::println(
            "会话 '{}' 正被另一轮查询占用（pid={} run={}）；可用 --wait 等待、"
            "--on-busy=interject 打断后发送，或先取消该轮。",
            session, lk.holder->pid, lk.holder->run);
        return 2;
      }
      if (on_busy == "interject") {
        if (!write_session_cancel(workdir, session, lk.holder->pid, lk.holder->run)) {
          std::println("插话失败：无法写取消标记（{}）",
                       session_cancel_path(workdir, session).string());
          return 2;
        }
        interjected = true;
        std::println("插话：已请求取消当前轮（pid={} run={}），等待其停止…",
                     lk.holder->pid, lk.holder->run);
      } else {
        std::println("会话 '{}' 正被 pid={} run={} 占用，等待中（最多 600s）…", session,
                     lk.holder->pid, lk.holder->run);
      }
      for (int i = 0; i < 3000 && !lk.acquired && lk.error.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        lk = acquire_session_lock(workdir, session, lock_run);
      }
    }
    if (!lk.acquired) {
      if (lk.holder)
        std::println("等待超时：会话 '{}' 仍被 pid={} run={} 占用", session, lk.holder->pid,
                     lk.holder->run);
      else
        std::println("会话锁获取失败：{}", lk.error.empty() ? "未知原因" : lk.error);
      return 2;
    }
    lock_releaser.held = true;  // 生命周期到 main 结束（覆盖后续所有 return）
    // 清掉"目标不是本进程"的陈旧取消标记（例如 holder 在消费前自然结束）；
    // 若标记恰好指向本进程（刚接管就被取消），保留给 should_stop 处理。
    if (auto const stale = read_session_cancel(workdir, session);
        stale.has_value() && stale->pid != self_pid)
      clear_session_cancel(workdir, session);
    if (interjected) std::println("已接管会话，开始新查询。");
  }
  // 取消检查：信号标志 或 本会话的 .cancel 标记（pid 必须是自己）
  should_stop = [workdir, session, self_pid]() -> bool {
    if (g_cancel_requested.load(std::memory_order_relaxed)) return true;
    auto const req = read_session_cancel(workdir, session);
    if (req.has_value() && req->pid == self_pid) {
      clear_session_cancel(workdir, session);  // 消费掉，避免影响下一轮
      return true;
    }
    return false;
  };

  // ---- 定位 workflow（settings.workflow_dirs → ws → 全局 → 发布自带）----
  Json ir_json;
  bool model_needed_key = false;
  Json const settings = wfr_read_settings(workdir);
  auto const path = locate(workdir, name_or_path, settings);
  if (!path) {
    std::println("找不到 workflow '{}'（按 settings.workflow_dirs → <ws>/.wfr/workflows → "
                 "~/.wfr/workflows → 发布自带 查找）",
                 name_or_path);
    return 2;
  }
  {
    std::ifstream in(*path);
    if (!in) {
      std::println("找不到 workflow '{}'（searched: {}）", name_or_path, *path);
      return 2;
    }
    try {
      in >> ir_json;
    } catch (std::exception const& e) {
      std::println("workflow JSON 解析失败: {}", e.what());
      return 2;
    }
    model_needed_key = has_model_node(ir_json);
    if (ir_json.contains("imports")) {
      auto loaded = load_workflow_imports(ir_json, allow_import, nullptr);
      if (!loaded) {
        std::println("动态库 import 失败: {}", loaded.error());
        return 2;
      }
    }
    if (ir_json.contains("import")) {
      auto expanded = expand_workflow_imports(ir_json, workdir);
      if (!expanded) {
        std::println("workflow import 展开失败: {}", expanded.error());
        return 2;
      }
    }
  }

  auto ir = WorkflowIR::parse(ir_json);
  if (!ir) {
    std::println("IR 解析失败: {}", ir.error());
    return 2;
  }

  // ---- 组件：内置宿主工具（datetime / calculator / weather / web_search）+ 真实模型 ----
  auto dotenv = load_dotenv({".env", "../.env", "../../.env"});
  // transport 供工具与模型共用（天气/网络搜索/LLM 都走它；纯离线 workflow 无副作用）
  auto transport = std::make_shared<http::CurlMultiTransport>();
  tool::ToolRegistry registry;
  registry.add(make_datetime_tool());
  registry.add(make_sketch_verify_tool());
  registry.add(make_calculator_tool());
  registry.add(make_weather_tool(transport, {}));              // Open-Meteo，免密钥
  {
    tool::WebSearchConfig ws_cfg;
    ws_cfg.api_key = env_or(dotenv, "WEB_SEARCH_API_KEY");     // feedcoop Bearer；未配则运行时搜索报错
    registry.add(make_web_search_tool(transport, std::move(ws_cfg)));
  }
  // wfr-㊶：文件系统对（file_read / list_dir / **file_write**）——sandbox 根 = 工作区。
  // 只读对兑现 artifact 引用行；file_write（read_only=false）由 ExecutionGuard 防护
  // （wfr-㊷）：防重放/幂等以会话日志为事实源，崩溃缺环由磁盘 probe 兜底。
  tool::FileToolsConfig ft_cfg;
  ft_cfg.root_dir = std::filesystem::absolute(workdir);  // 沙箱根 = 工作区；越界/绝对路径/链接逃逸由工具拒
  registry.add(make_file_read_tool(ft_cfg));
  registry.add(make_list_dir_tool(ft_cfg));
  registry.add(make_file_grep_tool(ft_cfg));  // problems/02 追加 S2：确定性定位（大文件不整段喂）
  registry.add(make_file_extract_tool(ft_cfg));  // S4：结构化提取（json/yaml/md/xml）
  registry.add(make_file_sample_tool(ft_cfg));   // S5：采样跳读（head/middle/tail）
  registry.add(make_file_write_tool(ft_cfg));
  // wfr-context（⑤）：HMI 编辑器 skill 文档 + LLM Wiki 检索（目录由 WFR_SKILL_DIR /
  // WFR_WIKI_DIR 指定；未配置 → 工具运行时报错，不影响其它工具）。
  registry.add(make_read_skill_tool());
  registry.add(make_wiki_search_tool());
  // 编译自检回环：写/改完页面由模型调用，host 跑 hmi 编译管线并把日志回显。
  // wfr-㊸ @①/@②/@③：bash 工具 —— 副作用任意（NeedHuman 人审 + 词面静态门禁）。
  // 执行后端 = SandboxManager 解析的沙箱（Auto：bwrap 可用→bwrap；否则受限子进程兜底）；
  // 无任何可用 provider → bash 以**空执行器**注册，handler 侧 fail-closed 拒跑（绝不裸奔）。
  // sbx 必须活得比 registry 久（spec.executor 是非拥有指针）。
  tool::SandboxConfig sbx_cfg;
  sbx_cfg.workspace = std::filesystem::absolute(workdir);
  tool::SandboxManager const sbx(sbx_cfg);
  if (sbx.available()) {
    registry.add(tool::make_bash_tool(sbx.executor()));
    std::println("bash 执行后端: {}", sbx.describe());
  } else {
    registry.add(tool::make_bash_tool(nullptr));
    std::println("⚠️ bash 不可用：{} —— bash 调用将 fail-closed 拒绝", sbx.describe());
  }
  ComponentRegistry comps;
  std::vector<std::string> all_tools;
  for (tool::ToolSpec const& t : registry.all()) all_tools.push_back(t.name);
  comps.toolsets["all"] = all_tools;
  comps.toolsets["ts"] = all_tools;
  // 内置 chat 专用：常用件 + 文件对（天气/搜索之外，兑现 artifact 引用 + 写文件）
  comps.toolsets["chat"] = {"get_weather", "web_search", "file_read", "list_dir", "file_grep",
                            "file_extract", "file_sample", "file_write", "read_skill",
                            "wiki_search"};
  // 题目修订工作流：只给一个自检工具（模型生成 sketch 后必须调用）。
  comps.toolsets["question"] = {"verify_sketch"};
  // wfr-㊸ @②：chat 的增强档 —— 带上 bash（副作用任意 → NeedHuman 人审悬挂）。
  // 独立 toolset，不污染既有 chat 的既有行为（approve_bash_demo 等用它）。
  comps.toolsets["chat_bash"] = {"get_weather", "web_search", "file_read",
                                 "list_dir",    "file_grep",  "file_extract",
                                 "file_sample", "file_write", "bash",
                                 "read_skill",  "wiki_search"};
  // B2-B 出行规划工具面：只读业务 DB 查询 + submit_result（自动验证器写 .out/trip_verdict.json）
  {
    auto ts = tool::make_trip_tools();
    std::vector<std::string> names;
    for (auto& t : ts) { names.push_back(t.name); registry.add(std::move(t)); }
    names.push_back("get_weather");
    // A2：D1 L2 用 file 落盘判据；problems/02 S2：file_grep 供确定性定位
    names.insert(names.end(), {"file_read", "file_write", "list_dir", "file_grep",
                               "file_extract", "file_sample"});
    comps.toolsets["trip"] = std::move(names);
  }
  // A6（problems/39）：验收 subagent 的只读工具面（verify kind=agent 用）。
  // 只放读工具——写工具不在 registry 里，验收者模型发不出写调用（"不存在而非被拒"）。
  comps.toolsets["verify_ro"] = {"file_read", "list_dir", "file_grep", "file_extract",
                                 "file_sample"};

  // 治理（problems/33 · G4 包装层自检）：有副作用却未声明效果分类的工具 → 启动即失败。
  // 缺省 Read = 只读；read_only=false 必须显式声明 Idempotent/Irreversible（否则运行期遗漏是静默的）。
  // 位置：所有内置工具（含 bash/file_write/submit_result）注册完成后 —— ⑤ 动态插件在其后注册的
  // 非只读工具需自行复调 effect_class_violations。
  {
    auto const v = tool::effect_class_violations(registry);
    if (!v.empty()) {
      std::println("治理自检失败：以下工具 read_only=false 但未声明效果分类（effect_class）：");
      for (std::string const& n : v) std::println("  - {}", n);
      std::println("请用 .with_effect_class(EffectClass::Idempotent|Irreversible) 声明（见 tool.hpp）");
      return 2;
    }
  }

  std::optional<model::openai::OpenAIChatModel> model;
  if (model_needed_key) {
    std::string const key = env_or(dotenv, "LLM_API_KEY");
    if (key.empty()) {
      std::println("workflow 含 model 节点，但缺少 LLM_API_KEY（见 .env.example）");
      return 2;
    }
    model::ModelConfig cfg;
    cfg.api_key = key;
    cfg.base_url = env_or(dotenv, "LLM_BASE_URL", "https://api.deepseek.com/v1");
    cfg.model = env_or(dotenv, "LLM_MODEL", "deepseek-chat");
    cfg.ca_info = ca_bundle_from_env();
    model.emplace(cfg, transport);
    comps.models["main"] = &*model;
  }

  // ---- wfr-㊷：副作用执行点防护 ----
  // guard 以**会话日志为事实源**：跨进程续聊时读旧会话历史，判定 file_write 等副作用
  // 工具的执行状态（未开始/已完成/Intent 残留）。审计信**写到本会话目录**（.wfr/audit/）。
  // 注：guard 在 main 作用域持有 → 生命周期覆盖 compile + 全部 fire_once。
  std::vector<message::Message> hist_msgs;
  {
    std::string const hp = session_path(workdir, session);
    std::error_code ec;
    if (std::filesystem::exists(hp, ec))
      hist_msgs = replay_session_messages(hp);  // 首跑无文件 → 空历史（未开始 → 可安全执行）
  }
  std::size_t audit_written = 0;  // 审计落盘计数（供调试验证，非同步关键路径）
  tool::ExecutionGuard::Options guard_opts;
  guard_opts.log = cppchain::workflow::replay_exec_log_reader(hist_msgs);  // 只建一次，运行期只查
  guard_opts.audit = [&](tool::AuditRecord const& rec) {
    // 追加写 .wfr/audit/session-<name>.jsonl（原子改写：临时 + rename，同 FileJournal 纪律）。
    // 审计不是关键路径：写失败只计数不打断（fail-open 于可观测性，fail-closed 于副作用本身）。
    std::filesystem::path const af =
        wfr_root(workdir) / "audit" / ("session-" + (session.empty() ? "default" : session) +
                                      ".audit.jsonl");
    std::error_code ec;
    std::filesystem::create_directories(af.parent_path(), ec);
    std::string tmp = af.string() + ".tmp";
    {
      std::ifstream in(af);
      std::ofstream out(tmp, std::ios::app);
      if (in && out) out << in.rdbuf();  // 拷贝现有再追加（简单原子）；会话量小，可接受
      if (out) out << nlohmann::json{{"ts", rec.ts},   {"tool", rec.tool},
                                     {"key", rec.key}, {"decision", rec.decision},
                                     {"who", rec.who}, {"why", rec.why}}
                          .dump()
                   << "\n";
    }
    if (!ec) std::filesystem::rename(tmp, af, ec);  // rename 失败 → tmp 残留文件（无害，下轮覆盖）
    ++audit_written;
  };
  tool::ExecutionGuard guard{std::move(guard_opts)};

  // wfr-㊹ 治理（problems/34）：tool_registry 节点的共享注册表 + 动态库搜索目录。
  //   shared_registry   —— tool_registry 运行期把 `*.so` 工具加载进它；独立于内置表，
  //                        消费点（model/run_tools/approve_tools）共享优先、内置兜底。
  //   tool_lib_dirs     —— 按 `lib<from>.so` 找库的目录；三档：env → 安装布局 → 编译宏。
  tool::ToolRegistry shared_registry;
  std::vector<std::string> const tool_lib_dirs = publish_tool_lib_dirs();

  CompileOptions opts;
  opts.components = &comps;
  // P1.5（arch 25）：声明式配置基准目录 = 主 workflow 文件所在目录（declarations.configs 的
  // from 相对它解析；子 workflow 装载沿用同一根）。显式路径与按名定位都从这里取。
  opts.config_dir = std::filesystem::path(*path).parent_path().string();
  opts.tool_policy = &guard;  // wfr-㊷：挂进 c_tool / c_run_tools 两个执行点
  opts.shared_registry = &shared_registry;  // wfr-㊹：tool_registry 加载目标（可空则 workflow 里该节点报错）
  if (!tool_lib_dirs.empty()) opts.tool_lib_dirs = &tool_lib_dirs;  // wfr-㊹：`lib<from>.so` 搜索目录
  opts.max_parallel = max_parallel;  // wfr-㊾ 批③：Send 并行波宽度（>1 时 parallel/executor 生效）
  // wfr-㊿：subworkflow（proxyNode）子 workflow 装载器 —— 按名/路径定位 + 静态 import 展开。
  // 与主 workflow 的装载同路径（locate + expand_workflow_imports），只多一层"子"语义。
  opts.subworkflow_loader = [&settings, &workdir](std::string const& spec) -> std::optional<Json> {
    auto const p = locate(workdir, spec, settings);
    if (!p) return std::nullopt;
    std::ifstream in(*p);
    if (!in) return std::nullopt;
    Json j;
    try {
      in >> j;
    } catch (...) {
      return std::nullopt;
    }
    if (j.contains("import")) {  // 静态展开（base_dir = 子文件所在目录），动态库 import 同主流程约定
      auto const base = std::filesystem::path(*p).parent_path().string();
      auto e = expand_workflow_imports(j, base);
      if (!e) return std::nullopt;
    }
    return j;
  };
  // 不做 config_keys 白名单（生成器侧防护；wfr 跑手写/内置 workflow 由 --set 提供键）
  // wfr-㊸ @②：人审悬挂（interrupt）需要 checkpointer + thread_id。用**进程内**
  // MemoryCheckpointer：悬挂↔恢复只发生在同一次 wfr 调用内；跨进程续聊仍走会话
  // 日志 replay（cp 每次进程重建，"日志即事实源"语义不变）。
  state::MemoryCheckpointer mcp;
  auto wf = compile_workflow(*ir, registry, &mcp, opts);
  if (!wf) {
    std::println("编译失败: {}", wf.error().message);
    return 2;
  }
  std::string const trig_desc =
      wf->trigger ? "trigger: schedule " + wf->trigger->schedule : "trigger: manual（wfr 拉起）";
  {
    std::size_t count = 0;
    cppchain::workflow::for_each_node(*ir, [&count](cppchain::workflow::NodeIR const&,
                                                    cppchain::workflow::LoopScope const*) {
      ++count;
      return true;
    });
    std::println("workflow: {}（{}）  ·  {} 个节点", wf->ir_id, trig_desc, count);
  }
  std::println("工作路径: {}", std::filesystem::absolute(workdir).string());

  // ---- 会话日志（.wfr/sessions/；session_path 已迁入 .wfr）----
  std::string const slog_path = session_path(workdir, session);
  if (reset) {
    std::error_code ec;
    std::filesystem::remove(slog_path, ec);
    std::println("已清空会话: {}", slog_path);
  }
  SessionLog slog(slog_path, wf->ir_id);
  seed_run_seq(slog.max_run_seq());   // 跨进程续编号：本轮 run = 该会话已有最大 + 1（轮次不重号）
  // problems/07 §16：在本轮写 __input__ **之前**判定上一轮是否被优雅取消
  //（之后再看会把自己本轮的 __input__ 当成最后一条 run）。
  bool const prev_round_cancelled = session_last_run_cancelled(workdir, session);
  if (input.empty()) {
    std::println("（未给输入。加一句输入即续跑/聊一句；--reset 清空会话）");
    return 0;
  }

  FireOptions fo;
  fo.log = &slog;
  fo.thread_id = "fire";  // wfr-㊸ @②：人审悬挂/恢复共用的线程（进程内 cp）
  fo.should_stop = should_stop;  // problems/07 §8 P1：节点边界取消
  // --stream：引擎事件逐条打 JSON 行到 stdout（CGI/前端流式渲染思考/工具/文本的通道）。
  // 事件含 node_start/node_end + node_kind + step + payload（= 节点 update，model→messages
  // 含 AI/reasoning/tool_calls，run_tools→Tool 消息）。逐行 flush 保证边跑边出。
  // 同时投影到会话日志（project_stream_to_log）：--stream 让 wrapper 结构日志让位（streaming=true），
  // 若只打 stdout 不投影，AI/工具消息不会落会话日志 → 下轮 replay / CGI history / debug 读不到。
  if (stream_events) {
    fo.stream = [wf_ptr = &wf.value()](cppchain::graph::StreamEvent const& ev) {
      std::cout << ev_json(ev).dump() << '\n';
      std::cout.flush();
      // 投影到会话日志：与默认 wrapper 同信封（engine_enter/exit + update），
      // 保证会话历史完整可回放（CGI 的 history/debug 依赖它）。
      if (wf_ptr->sink->log != nullptr) {
        cppchain::workflow::project_stream_to_log(*wf_ptr->sink->log, wf_ptr->sink->run,
                                                  ev, "");
      }
    };
  }
  Json inp = Json::array();
  inp.push_back(message::human_message(input));
  fo.input_messages = std::move(inp);

  Json config{{"workspace", workdir}, {"session", session}};
  if (prev_round_cancelled) config["prev_round"] = "未完成（被新消息打断）";
  for (auto const& [k, v] : sets) config[k] = v;

  // wfr-㊸ @④：会话级 allow 前缀（范围不变量）。开关 = `--set allow_prefixes=on`：
  //   - 把 <session>.allowlist.jsonl 的批准记录灌进 config → approve_tools 自动放行同前缀命令
  //   - 批准时把命令原文追加进 allowlist（下文 fire 循环 approve 分支）
  // 关（缺省）→ 不读不写，每次 NeedHuman 依旧悬挂人审（既有行为不变）。
  // 沙箱身份无论开关都注入：范围不变量 = 自动放行只在「同会话 + 同沙箱 provider」下生效。
  if (sbx.available()) config["sandbox_provider"] = sbx.provider_name();
  bool const allow_persist = sets.contains("allow_prefixes") && sets.at("allow_prefixes") == "on";
  if (allow_persist) {
    config["approved_prefixes"] = cppchain::workflow::wfr_load_allow_prefixes(workdir, session);
    std::println("会话级 allow 前缀: {} 条（{}）", config["approved_prefixes"].size(),
                 cppchain::workflow::wfr_allowlist_path(workdir, session).string());
  }

  // wfr-㊸ @②：一次触发 = fire 循环直至跑完。每逢 approve_tools 悬挂（bash 等
  // NeedHuman 工具），打印待批请求、读一行人工裁决（approve / reject <理由> / abort），
  // 再 resume 续跑。不悬挂的 workflow 只走一次 fire，与既有行为一致。
  // 取消收尾：落一条可查的终态记录（replay 只读 ok 记录，不影响历史），释放锁由 RAII 完成。
  auto handle_cancelled = [&]() -> int {
    std::string const run = "r-cancelled-" + std::to_string(self_pid);
    slog.engine_enter(run, "__cancelled__", "cancel");
    slog.engine_exit(run, "__cancelled__", "cancel", /*ok=*/false, "cancelled", 0, std::nullopt);
    if (stream_events) {  // 流通道给前端一个明确终态（NDJSON）
      std::cout << Json{{"type", "cancelled"}}.dump() << '\n';
      std::cout.flush();
    }
    std::println("（已取消：节点边界停止；已完成节点的记录保留在会话日志；锁将释放。）");
    return 130;  // 128 + SIGINT 惯例
  };

  Json result_state;
  for (;;) {
    if (should_stop && should_stop()) return handle_cancelled();
    auto r = fire_once(*wf, config, fo);
    if (!r) {
      if (r.error().kind == WorkflowError::Kind::Cancelled) return handle_cancelled();
      std::println("触发失败: {}", r.error().message);
      return 1;
    }
    result_state = *r;
    // wfr-㊹：动态工具在 tool_registry 运行期才进共享表 → 治理自检延后到 fire 后复调
    //（插件 descriptor 契约本已声明；这是纵深防御，防"新库漏声明"成为静默盲区）。
    {
      auto const v2 = tool::effect_class_violations(shared_registry);
      if (!v2.empty()) {
        std::println("治理自检失败：动态加载工具 read_only=false 未声明 effect_class：");
        for (std::string const& n : v2) std::println("  - {}", n);
        return 2;
      }
    }
    auto const& pend = wf->graph.last_interrupt();
    if (!pend.interrupted) break;  // 跑完（或本就不需要人审）

    // —— 悬挂分支：plan_review（步骤边界确认） / approve_tools（工具批） ——
    if (pend.payload.value("kind", "") == "plan_review_request") {
      std::println();
      std::println("⏸ 步骤边界确认（节点 {}）：", pend.node);
      Json const pl = pend.payload.value("plan", Json::object());
      std::println("   目标: {}", pl.value("goal", ""));
      std::println("   进度: {} / {}   已完成步骤摘要:\n   {}", pl.value("cursor", 0L),
                   pl.value("count", 0L), pl.value("done", ""));
      std::println("   下一步: {}", pl.value("next", ""));
      std::string const policy = config.value("approve_policy", "prompt");
      Json resume;
      if (policy != "prompt") {
        std::println("  下一步？[continue | rollback_to <K> | abort]： → mock 边界裁决（approve_policy={}）", policy);
        // allow/continue → 放行；其余（deny/未知）→ abort（fail-closed，留痕）
        resume = (policy == "allow" || policy == "continue") ? Json{{"action", "continue"}}
                                                             : Json{{"action", "abort"}};
      } else {
        std::optional<Json> via_ch;
        if (!approve_path.empty()) via_ch = ask_approval_channel(approve_path, pend.node, pend.payload);
        if (via_ch.has_value()) {
          std::string const act = via_ch->value("action", "");
          if (act == "continue" || act == "approve") resume = Json{{"action", "continue"}};
          else if (act == "rollback_to")
            resume = Json{{"action", "rollback_to"}, {"to", via_ch->value("to", 0L)}};
          else resume = Json{{"action", "abort"}};
          std::println("  → 裁决（approve 通道）{}", resume.dump());
        } else {
        std::print("  下一步？[continue | rollback_to <K> | abort]：");
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) {
          std::println();
          // 治理（problems/33）：stdin 结束也走 abort 续跑 → _cancelled 终态成档
          //（不再 return 0 悬挂残留，取消必须是可查的终态）
          std::println("（stdin 结束，未获裁决 —— 按取消处理，落 _cancelled 档。）");
          resume = Json{{"action", "abort"}};
        } else {
          std::string const t = cppchain::wfr::trim(line);
          if (t.empty() || t == "abort" || t == "a" || t == "q" || t == "quit") {
            std::println("（已中止 —— 落 _cancelled 档。）");
            resume = Json{{"action", "abort"}};
          } else if (t == "continue" || t == "c") {
            resume = Json{{"action", "continue"}};
          } else if (t.starts_with("rollback_to") || t == "r" || t.starts_with("r ")) {
            long to = 0;
            for (char ch : t)
              if (ch >= '0' && ch <= '9') to = to * 10 + (ch - '0');
            resume = Json{{"action", "rollback_to"}, {"to", to}};
          } else {
            resume = Json{{"action", "abort"}};  // 未识别 → abort（fail-closed，留痕）
          }
        }
        std::println("  → 裁决 {}", resume.dump());
        }
      }
      fo.resume = std::move(resume);
      fo.input_messages.clear();  // 恢复续跑：input 由引擎忽略
      continue;
    }

    // —— 悬挂：人工审批 ——
    std::println();
    std::println("⏸ 需人工审批（节点 {}）—— 本轮执行停在审批点：", pend.node);
    if (pend.payload.contains("calls") && pend.payload["calls"].is_array()) {
      for (Json const& c : pend.payload["calls"])
        std::println("  · tool={}  id={}  args={}", c.value("tool", "?"), c.value("id", "?"),
                     c.value("args", Json::object()).dump());
    } else {
      std::println("  待批请求: {}", pend.payload.dump());
    }
    // 裁决来源：mock（--set approve_policy=allow|deny|deny:理由，免交互自动化测试）
    // 或人工 stdin（缺省 prompt，现状）。mock 未知值 fail-closed 拒绝。
    std::string const policy = config.value("approve_policy", "prompt");
    Json resume;
    if (policy == "prompt") {
      std::optional<Json> via_ch;
      if (!approve_path.empty()) via_ch = ask_approval_channel(approve_path, pend.node, pend.payload);
      if (via_ch.has_value()) {
        std::string const act = via_ch->value("action", "");
        if (act == "approve") resume = Json{{"action", "approve"}};
        else if (act == "reject")
          resume = Json{{"action", "reject"}, {"feedback", via_ch->value("feedback", "")}};
        else resume = Json{{"action", "abort"}};
        std::println("  → 裁决（approve 通道）{}", resume.dump());
      } else {
      std::print("  批准？[approve | reject <理由> | abort]：");
      std::cout.flush();
      std::string line;
      if (!std::getline(std::cin, line)) {
        std::println();
        std::println("（stdin 结束，未获裁决 —— 本次触发止于审批点，可留待下次续审。）");
        return 0;
      }
      std::string const t = cppchain::wfr::trim(line);
      if (t.empty() || t == "abort" || t == "q" || t == "quit") {
        std::println("（已中止 —— 本次触发止于审批点。）");
        return 0;
      }
      if (t == "y" || t.starts_with("approve")) {
        resume = Json{{"action", "approve"}};
      } else if (t == "n") {
        resume = Json{{"action", "reject"}};
      } else if (t.starts_with("reject") || t.starts_with("拒")) {
        std::string const fb = cppchain::wfr::trim(t.substr(std::string("reject").size()));
        resume = Json{{"action", "reject"}, {"feedback", fb}};
      } else {
        // 未识别输入 → 按拒绝处理（fail-closed：不执行）
        resume = Json{{"action", "reject"}, {"feedback", t}};
      }
      }
    } else {
      std::println("  批准？[approve | reject <理由> | abort]： → mock 审批（approve_policy={}）", policy);
      if (policy == "allow") resume = Json{{"action", "approve"}};
      else if (policy == "deny") resume = Json{{"action", "reject"}};
      else if (policy.starts_with("deny:")) {
        // 只在这里对 "deny:" 取后缀——policy 是 allow/deny/空等短串时不可 substr(5)（越界）
        std::string const fb = cppchain::wfr::trim(policy.substr(5));
        resume = Json{{"action", "reject"}, {"feedback", fb}};
      }
      else resume = Json{{"action", "reject"}, {"feedback", "未知 approve_policy: " + policy}};
    }

    // 公共（mock 与人工一致）：批准 → 会话级 allow 前缀落盘（wfr-㊸ @④）。
    // 只记「命令类」调用（bash 的 command 参数）；无 command 的 NeedHuman 不持久化。
    if (resume.value("action", "") == "approve" && allow_persist) {
      std::size_t added = 0;
      if (pend.payload.contains("calls") && pend.payload["calls"].is_array()) {
        for (Json const& c : pend.payload["calls"]) {
          if (c.value("tool", "") != "bash") continue;
          auto const& args = c.value("args", Json::object());
          if (!args.is_object() || !args.contains("command") || !args["command"].is_string())
            continue;
          std::string const cmd = args["command"].get<std::string>();
          if (cmd.empty()) continue;
          // wfr-㊸ ⑤：落盘时记命令的**词面类别** —— allow 前缀自动放行按类别封顶，
          // 防止「批过只读前缀 → 注入/串联危险后缀」被自动放行。
          cppchain::workflow::wfr_append_allow_prefix(
              workdir, session, cmd, config.value("sandbox_provider", ""),
              tool::command_class_name(tool::classify_bash_command(cmd)));
          ++added;
        }
      }
      if (added > 0)
        std::println("  ↳ 已把 {} 条批准命令记入本会话 allow 前缀（同前缀后续自动放行）", added);
    }
    fo.resume = std::move(resume);
    fo.input_messages.clear();  // 恢复续跑：input 由引擎忽略（快照已含本轮前半段）
    std::println("  → 裁决 {}\n", fo.resume->dump());
  }

  std::string answer = last_ai_text(result_state);
  message::sanitize_utf8(answer);  // 模型串流拼接可能带断字节：净化后完整打印（不截断）
  // --stream：事件流就是输出（CGI 按行解析 JSON），抑制人类可读打印避免污染。
  if (!stream_events)
    std::println("\n{}", answer.empty() ? "(无文本回复)" : answer);
  // wfr-㊷：副作用防护统计（验证陷阱纪律：计数/审计行数证明被测路径真被走到）
  if (!stream_events && guard.has_log())
    std::println("副作用防护: 执行 {} · 重放 {} · fail-closed {} · 审计 {} 行",
                 guard.executions(), guard.replays(), guard.blocked(), audit_written);
  if (!stream_events)
    std::println("会话已更新: {} 条消息", replay_session_messages(slog_path).size());
  return 0;
}