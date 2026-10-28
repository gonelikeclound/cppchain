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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>  // open /dev/null

#include "cppchain/message/serializer.hpp"
#include "cppchain/tool/spawn_runner.hpp"
#include "cppchain/workflow/session_lock.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "wfr_cgi_lib.hpp"  // 纯逻辑（parse_session_id / default_sets_for / 枚举等，测试共用）

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

// 从 CGI stdin 读 JSON body：{"config": {k: v, …}} → 任意 --set 配置（大 payload 走 body，避开 QUERY_STRING 的 env 上限）。
// geo-server 内部调用用；body 配置在后追加，覆盖 default_sets_for 的默认。
std::map<std::string, std::string> body_config() {
  std::map<std::string, std::string> out;
  char const* cl = std::getenv("CONTENT_LENGTH");
  if (!cl) return out;
  int const len = std::atoi(cl);
  if (len <= 0 || len > 8 * 1024 * 1024) return out;
  std::string buf(len, '\0');
  if (std::fread(buf.data(), 1, static_cast<std::size_t>(len), stdin) !=
      static_cast<std::size_t>(len))
    return out;
  try {
    Json j = Json::parse(buf);
    if (j.contains("config") && j["config"].is_object())
      for (auto const& [k, v] : j["config"].items()) {
        if (v.is_string())
          out[k] = v.get<std::string>();
        else if (!v.is_null())
          out[k] = v.dump();
      }
  } catch (...) {
    // body 非 JSON：忽略，按无配置处理
  }
  return out;
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

// workflow 枚举目录集（顺序即优先级）：工作区 settings.workflow_dirs → 显式 cgi-workflows →
// 发布（WFR_WORKFLOWS_DIR 环境 → share/wfr/workflows）。CGI 已 chdir 到 repo 根。
std::vector<std::filesystem::path> workflow_dirs_for(std::string const& ws) {
  std::vector<std::filesystem::path> dirs;
  // 1) 工作区 .wfr/settings.json 的 workflow_dirs（相对 ws 解析；与 wfr_read_settings 同语义）
  std::filesystem::path const settings = std::filesystem::path(ws) / ".wfr" / "settings.json";
  if (std::ifstream in(settings); in) {
    Json j;
    try { in >> j; } catch (...) { j = Json::object(); }
    if (j.contains("workflow_dirs") && j["workflow_dirs"].is_array())
      for (auto const& d : j["workflow_dirs"])
        if (d.is_string()) {
          std::filesystem::path dp(d.get<std::string>());
          dirs.push_back(dp.is_absolute() ? dp : std::filesystem::path(ws) / dp);
        }
  }
  // 2) 显式 cgi-workflows（本专题的自定义 workflow 目录，隔离自 share/）
  dirs.push_back(std::filesystem::path("cgi-workflows"));
  // 3) 发布目录：WFR_WORKFLOWS_DIR 环境 > share/wfr/workflows
  if (char const* e = std::getenv("WFR_WORKFLOWS_DIR"); e && *e)
    dirs.push_back(e);
  dirs.push_back(std::filesystem::path("share") / "wfr" / "workflows");
  return dirs;
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

  // session 名 = `wfname.sessionid`（workflow 绑定编码在名字里）→ 解析 workflow + 校验白名单
  auto const seg = cppchain::cgi::parse_session_id(s);
  if (!seg) {
    std::cout << "Content-Type: application/x-ndjson\r\n\r\n"
              << "{\"type\":\"error\",\"msg\":\"session 未绑定 workflow（名字需为 wfname.sessionid）\"}\n";
    return;
  }
  auto const dirs = workflow_dirs_for(ws);
  if (!cppchain::cgi::is_registered_workflow(dirs, seg->workflow)) {
    std::cout << "Content-Type: application/x-ndjson\r\n\r\n"
              << "{\"type\":\"error\",\"msg\":\"workflow '" + seg->workflow + "' 不在可枚举目录里\"}\n";
    return;
  }

  // argv：`wfr <workflow> --workdir <ws> --session <wfname.sessionid> [--set k=v…] --stream <text>`
  std::vector<std::string> av = {wfr_bin(), seg->workflow,
                                 "--workdir", ws, "--session", s};  // s = 完整 wfname.sessionid
  for (auto const& [k, v] : cppchain::cgi::default_sets_for(seg->workflow, text)) {
    av.push_back("--set");
    av.push_back(k + "=" + v);
  }
  // 通用化：stdin body 的任意 config 也作为 --set 追加（后置 → 覆盖默认）
  for (auto const& [k, v] : body_config()) {
    av.push_back("--set");
    av.push_back(k + "=" + v);
  }
  // problems/07 §8 P2：忙时策略（前端"插话"传 interject → wfr 取消当前轮后接管）
  std::string const on_busy = param(q, "on_busy");
  if (on_busy == "reject" || on_busy == "wait" || on_busy == "interject")
    av.push_back("--on-busy=" + on_busy);
  av.push_back("--stream");
  av.push_back(text);
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
  Json out = cppchain::cgi::read_workspaces_file(cfg);
  return Json{{"workspaces", std::move(out)}};
}

// workflows：枚举 session 可绑定的 workflow（工作区 settings → cgi-workflows → 发布）。
// 出 {workflows:[{name, source}]}，source = 所在目录 basename（官方/自定义角标）。
Json action_workflows(std::vector<std::pair<std::string, std::string>> const& q) {
  std::string const ws = param(q, "ws");
  if (ws.empty()) return Json{{"error", "missing ws"}};
  Json arr = Json::array();
  auto const dirs = workflow_dirs_for(ws);
  for (auto const& d : dirs) {
    std::string const source = d.filename().string();
    for (std::string const& n : cppchain::cgi::list_workflow_names(d))
      arr.push_back(Json{{"name", n}, {"source", source}});
  }
  // 按名去重（首个 win，即目录优先级序）——保持与 wfr locate 语义一致
  Json seen = Json::object();
  Json dedup = Json::array();
  for (auto const& w : arr) {
    std::string const n = w.value("name", "");
    if (seen.contains(n)) continue;
    seen[n] = true;
    dedup.push_back(w);
  }
  return Json{{"workflows", std::move(dedup)}};
}

// workspace_create：建目录 + 种子 settings + 注册进 workspaces.json（原子写）。
Json action_workspace_create(std::vector<std::pair<std::string, std::string>> const& q) {
  std::string const name = param(q, "name");
  std::string const dir = param(q, "dir");
  if (!cppchain::cgi::valid_workspace_name(name))
    return Json{{"error", "非法 workspace 名（字母数字开头，≤32 字符）"}};
  auto const dir_ok = cppchain::cgi::sanitize_workspace_dir(std::filesystem::current_path(), dir);
  if (!dir_ok)
    return Json{{"error", "非法目录（非绝对、无 ..、以仓库根为前缀）"}};
  std::filesystem::path const cfg = std::filesystem::path("web") / "workspaces.json";
  // 重名检查（读现有）
  for (auto const& w : cppchain::cgi::read_workspaces_file(cfg))
    if (w.is_object() && (w.value("name", "") == name || w.value("dir", "") == *dir_ok))
      return Json{{"error", "name 或 dir 已存在"}};
  // 建目录
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(*dir_ok), ec);
  if (ec) return Json{{"error", "建目录失败"}};
  // 种子 settings：指向 cgi-workflows（保证新 ws 能跑自定义 workflow）
  std::filesystem::path const settings =
      std::filesystem::path(*dir_ok) / ".wfr" / "settings.json";
  if (!std::filesystem::exists(settings, ec)) {
    std::filesystem::create_directories(settings.parent_path(), ec);
    Json s = Json{{"workflow_dirs",
                   Json::array({(std::filesystem::current_path() / "cgi-workflows").string()})}};
    if (std::ofstream out(settings); out) out << s.dump(2) << "\n";
  }
  if (!cppchain::cgi::append_workspace(cfg, name, *dir_ok))
    return Json{{"error", "写入 workspaces.json 失败"}};
  return Json{{"ok", true}, {"workspace", Json{{"name", name}, {"dir", *dir_ok}}}};
}

// debug：跑 `wfr debug <wf> --workdir <ws> --session <wfname.sessionid> --out <tmp>` 读回单 HTML。
// 自己写 Content-Type: text/html（iframe 直接显示）；失败给 text/html 错误页。
void action_debug(std::vector<std::pair<std::string, std::string>> const& q) {
  std::string const ws = param(q, "ws");
  std::string const s = param(q, "s");
  auto const seg = cppchain::cgi::parse_session_id(s);
  if (ws.empty() || !seg) {
    std::cout << "Content-Type: text/html; charset=utf-8\r\n\r\n"
              << "<html><body style='font-family:monospace;padding:1em'>"
                 "<b>无法生成调试台</b>：session 未绑定 workflow（名字需为 wfname.sessionid）"
                 "</body></html>\n";
    return;
  }
  // 临时文件
  std::string tmpl = "/tmp/wfr-dbg-XXXXXX.html";
  std::vector<char> buf(tmpl.begin(), tmpl.end()); buf.push_back('\0');
  int fd = mkstemps(buf.data(), 5 /* .html */);
  if (fd < 0) {
    std::cout << "Content-Type: text/html\r\n\r\n<html><body>mkstemps 失败</body></html>\n";
    return;
  }
  close(fd);
  std::string const tmp_out(buf.data());

  std::vector<std::string> av = {wfr_bin(), "debug", seg->workflow,
                                 "--workdir", ws, "--session", s,  // 完整 wfname.sessionid
                                 "--out", tmp_out};
  std::vector<char*> cargv;
  for (auto& a : av) cargv.push_back(a.data());
  cargv.push_back(nullptr);

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    unlink(tmp_out.c_str());
    std::cout << "Content-Type: text/html\r\n\r\n<html><body>pipe 失败</body></html>\n";
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    unlink(tmp_out.c_str());
    std::cout << "Content-Type: text/html\r\n\r\n<html><body>fork 失败</body></html>\n";
    return;
  }
  if (pid == 0) {
    // stdout 丢弃（wfr debug 的人类提示行不进响应）
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]); close(pipefd[1]);
    execvp(cargv[0], cargv.data());
    _exit(127);
  }
  close(pipefd[1]);
  // 消费子进程 stderr（防阻塞）；等结束
  FILE* errfp = fdopen(pipefd[0], "r");
  char ebuf[4096];
  if (errfp) while (fgets(ebuf, sizeof ebuf, errfp)) {}
  if (errfp) fclose(errfp);
  int status = 0;
  waitpid(pid, &status, 0);
  bool const ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (ok && std::filesystem::is_regular_file(tmp_out)) {
    std::cout << "Content-Type: text/html; charset=utf-8\r\n\r\n";
    if (std::ifstream in(tmp_out); in)
      std::cout << in.rdbuf();
  } else {
    std::cout << "Content-Type: text/html; charset=utf-8\r\n\r\n"
              << "<html><body style='font-family:monospace;padding:1em'>"
                 "<b>调试台生成失败</b>（wfr debug 退出码 "
              << (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : "1")
              << "）——会话可能还没有任何记录</body></html>\n";
  }
  unlink(tmp_out.c_str());
}

}  // namespace

// cancel：请求取消当前轮（problems/07 §8 P1）。不阻塞等待——wfr 在节点边界退出，
// 前端随后会看到流结束；返回持有者信息便于提示。
Json action_cancel(std::vector<std::pair<std::string, std::string>> const& q) {
  std::string const ws = param(q, "ws");
  std::string const s = param(q, "s");
  if (ws.empty() || s.empty()) return Json{{"error", "missing ws/s"}};
  auto const lock = cppchain::workflow::read_session_lock(ws, s);
  if (!lock.has_value())
    return Json{{"ok", true}, {"cancelled", false}, {"msg", "没有进行中的查询"}};
  bool const ok = cppchain::workflow::write_session_cancel(ws, s, lock->pid, lock->run);
  return Json{{"ok", ok},
              {"cancelled", ok},
              {"pid", lock->pid},
              {"run", lock->run},
              {"msg", ok ? "取消请求已发送（节点边界生效）" : "写取消标记失败"}};
}

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
  if (action == "debug") {
    action_debug(q);  // 返回 text/html（wfr debug 单页）
    return 0;
  }
  Json result;
  if (action == "history") result = action_history(q);
  else if (action == "sessions") result = action_sessions(q);
  else if (action == "workspaces") result = action_workspaces();
  else if (action == "workflows") result = action_workflows(q);
  else if (action == "workspace_create") result = action_workspace_create(q);
  else if (action == "cancel") result = action_cancel(q);
  else result = Json{{"error", "unknown action: " + action}};
  json_out(result);
  return 0;
}