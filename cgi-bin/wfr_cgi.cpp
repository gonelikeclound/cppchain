// cgi-bin/wfr_cgi —— CGI WebUI 最小脚手架后端（wfr-🈟 CGI 专题，discuss 17）
//
// C++ CGI 可执行：读 QUERY_STRING → 分发 action → 复用 cppchain 读/运行件 → JSON 输出。
//   · 绝不解析 wfr 的人类可读 stdout；跑完 wfr 后调 replay_session_messages 拿结构化消息，
//     最后一条 AI 文本即回答（与 wfr main.cpp:74-88 的 last_ai_text 同判据，但直接读日志）。
//   · wfr 路径：环境变量 WFR_BIN（缺省 ./build/dev/bin/wfr）。
//   · workflow：cgi-workflows/（隔离自 share/），经工作区 settings.workflow_dirs 指定。
//
// action:
//   ?action=workspaces                       → {workspaces:[{name,dir}]}
//   ?action=sessions&ws=<dir>                → {sessions:[{name,path,size,updated}]}
//   ?action=chat&ws=<dir>&s=<session>&q=<text>
//                                             → {answer, messages:N, steps}
//   ?action=history&ws=<dir>&s=<session>     → {messages:[Message.json...]}
//
// 编译：examples CMake target `wfr_cgi`，链接 cppchain_workflow（replay_session_messages）。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "cppchain/message/serializer.hpp"
#include "cppchain/tool/spawn_runner.hpp"
#include "cppchain/workflow/session_log.hpp"

using Json = nlohmann::json;

namespace {

std::string url_decode(std::string_view src) {
  std::string out;
  out.reserve(src.size());
  for (std::size_t i = 0; i < src.size();) {
    if (src[i] == '%' && i + 2 < src.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int h = hex(src[i + 1]), l = hex(src[i + 2]);
      if (h >= 0 && l >= 0) {
        out.push_back(static_cast<char>((h << 4) | l));
        i += 3;
        continue;
      }
    } else if (src[i] == '+') {
      out.push_back(' ');
      ++i;
      continue;
    }
    out.push_back(src[i]);
    ++i;
  }
  return out;
}

// 解析 QUERY_STRING → k=v 映射（URL decoded）
std::vector<std::pair<std::string, std::string>> parse_query() {
  std::vector<std::pair<std::string, std::string>> out;
  std::string const qs = std::getenv("QUERY_STRING") ? std::getenv("QUERY_STRING") : "";
  std::size_t pos = 0;
  while (pos <= qs.size()) {
    std::size_t const amp = qs.find('&', pos);
    std::string const part = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    if (!part.empty()) {
      std::size_t const eq = part.find('=');
      if (eq == std::string::npos)
        out.emplace_back(url_decode(part), "");
      else
        out.emplace_back(url_decode(part.substr(0, eq)), url_decode(part.substr(eq + 1)));
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return out;
}

std::string param(std::vector<std::pair<std::string, std::string>> const& q,
                  std::string const& key, std::string const& dflt = "") {
  for (auto const& [k, v] : q) if (k == key) return v;
  return dflt;
}

// 工作区目录 → 会话日志路径（与 session_path 同构；这里独立拼避免拉整个 wfr 层）
std::string session_log_path(std::string const& ws, std::string const& name) {
  std::string const n = name.empty() ? "session" : "session-" + name;
  return (std::filesystem::path(ws) / ".wfr" / "sessions" / (n + ".log.jsonl")).string();
}

std::string wfr_bin() {
  if (char const* e = std::getenv("WFR_BIN"); e && *e) return e;
  return "build/dev/bin/wfr";
}

void json_out(Json const& j) {
  std::cout << "Content-Type: application/json; charset=utf-8\r\n\r\n" << j.dump() << "\n";
}

// chat：跑 wfr 主 workflow（--stream 事件流）并把事件逐行转发到 CGI stdout（NDJSON，实时 flush）。
// 前端用 fetch ReadableStream 逐行读；不解析 wfr 人类可读 stdout，只透传 JSON 事件行。
void action_chat_stream(std::vector<std::pair<std::string, std::string>> const& q) {
  std::string const ws = param(q, "ws");
  std::string const s = param(q, "s");
  std::string const text = param(q, "q");
  if (ws.empty() || s.empty() || text.empty()) {
    std::cout << "Content-Type: application/x-ndjson\r\n\r\n"
              << "{\"type\":\"error\",\"msg\":\"missing ws/s/q\"}\n";
    return;
  }

  std::vector<std::string> av = {wfr_bin(), "chat_cgi",
                                 "--workdir", ws, "--session", s,
                                 "--set", "compress_when=messages",
                                 "--set", "compress_max_msgs=40",
                                 "--set", "compress_keep_last=6",
                                 "--stream",
                                 text};
  // argv → C 数组（execvp 用）
  std::vector<char*> cargv;
  for (auto& a : av) cargv.push_back(a.data());
  cargv.push_back(nullptr);

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    std::cout << "Content-Type: application/x-ndjson\r\n\r\n"
              << "{\"type\":\"error\",\"msg\":\"pipe failed\"}\n";
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    std::cout << "Content-Type: application/x-ndjson\r\n\r\n"
              << "{\"type\":\"error\",\"msg\":\"fork failed\"}\n";
    return;
  }
  if (pid == 0) {
    // 子进程：stdout 接管道写端，exec wfr --stream
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[0]); close(pipefd[1]);
    execvp(cargv[0], cargv.data());
    _exit(127);  // exec 失败（wfr 不存在等）
  }
  close(pipefd[1]);

  // 先发 HTTP 头（NDJSON），再逐行转发子进程 stdout
  std::cout << "Content-Type: application/x-ndjson; charset=utf-8\r\n\r\n";
  std::cout.flush();

  FILE* fp = fdopen(pipefd[0], "r");
  char buf[65536];
  while (fp && fgets(buf, sizeof buf, fp)) {
    std::string line(buf);
    if (line.empty()) continue;
    // 只透传 JSON 行（wfr 的 --stream 事件行；人类可读行过滤）
    if (line[0] == '{') std::cout << line;
    std::cout.flush();
  }
  if (fp) fclose(fp);
  int status = 0;
  waitpid(pid, &status, 0);
  std::cout << "{\"type\":\"end\",\"exit\":"
            << (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : "1")
            << "}\n";
  std::cout.flush();
}

Json action_history(std::vector<std::pair<std::string, std::string>> const& q) {
  std::string const ws = param(q, "ws");
  std::string const s = param(q, "s");
  if (ws.empty() || s.empty()) return Json{{"error", "missing ws/s"}};
  auto const msgs = cppchain::workflow::replay_session_messages(session_log_path(ws, s));
  Json arr = Json::array();
  for (auto const& m : msgs) arr.push_back(m);
  return Json{{"messages", std::move(arr)}};
}

Json action_sessions(std::vector<std::pair<std::string, std::string>> const& q) {
  std::string const ws = param(q, "ws");
  if (ws.empty()) return Json{{"error", "missing ws"}};
  Json arr = Json::array();
  std::error_code ec;
  std::filesystem::path const dir = std::filesystem::path(ws) / ".wfr" / "sessions";
  if (std::filesystem::is_directory(dir, ec))
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
      if (!std::filesystem::is_regular_file(it->path(), ec)) continue;
      std::string const fname = it->path().filename().string();
      if (!fname.ends_with(".log.jsonl")) continue;
      // session-<name>.log.jsonl → name
      std::string name = fname.substr(0, fname.size() - std::string(".log.jsonl").size());
      if (name.starts_with("session-")) name = name.substr(std::string("session-").size());
      else if (name == "session") name = "";
      auto const t = std::filesystem::last_write_time(it->path(), ec);
      (void)t;  // 时间戳暂不进 JSON（保持简单；需要时转 epoch）
      arr.push_back(Json{{"name", name}, {"size", static_cast<long>(it->file_size(ec))}});
    }
  return Json{{"sessions", std::move(arr)}};
}

// workspaces：读 <repo>/web/workspaces.json（{workspaces:[{name,dir}]}），缺省给空
Json action_workspaces() {
  std::filesystem::path const cfg = std::filesystem::path("web") / "workspaces.json";
  Json out = Json::array();
  if (std::ifstream in(cfg); in) {
    Json j;
    try { in >> j; } catch (...) { j = Json::object(); }
    if (j.contains("workspaces") && j["workspaces"].is_array())
      for (auto const& w : j["workspaces"])
        if (w.is_object()) out.push_back(w);
  }
  return Json{{"workspaces", std::move(out)}};
}

}  // namespace

int main() {
  // CGI 进程 cwd 是 web server 的工作目录（如 web/），所有相对路径（workspaces.json/ws/wfr）
  // 要在**仓库根**下解析才算对。定位：WFR_ROOT 环境变量 > 向上找含 web/workspaces.json 的目录。
  {
    std::filesystem::path root;
    if (char const* e = std::getenv("WFR_ROOT"); e && *e) {
      root = e;
    } else {
      std::filesystem::path cwd = std::filesystem::current_path();
      for (;;) {
        if (std::filesystem::exists(cwd / "web" / "workspaces.json")) { root = cwd; break; }
        auto parent = cwd.parent_path();
        if (parent == cwd) break;
        cwd = parent;
      }
    }
    if (!root.empty()) {
      std::error_code ec;
      std::filesystem::current_path(root, ec);
    }
  }

  auto q = parse_query();
  std::string const action = param(q, "action", "workspaces");
  if (action == "chat") {
    action_chat_stream(q);  // 流式：自己写 HTTP 头 + NDJSON 事件行
    return 0;
  }
  Json result;
  if (action == "history") result = action_history(q);
  else if (action == "sessions") result = action_sessions(q);
  else if (action == "workspaces") result = action_workspaces();
  else result = Json{{"error", "unknown action: " + action}};
  json_out(result);
  return 0;
}