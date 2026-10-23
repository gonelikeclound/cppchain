// cppchain —— examples/mcp_mock/main.cpp
// 自包含 mock MCP server：真实 HTTP 网络服务（POSIX socket，回环），零外部依赖。
// 目的：不依赖 node/Everything，把"整个 MCP"在真网络上传真验证——
//   · POST /mcp     —— JSON-RPC：initialize（发会话）/ tools/list / tools/call / resources/list
//   · GET  /mcp     —— SSE 通知长连（带会话头；工具集变更时推 notifications/tools/list_changed）
//   · GET  /mcp.json —— 发现列表（url:// 路线直接拉），指向本 server 自身
// 可控性：内置两个"变更工具"——add_tool / remove_tool，调用即改工具集并**主动推送**，
// 正好驱动 provider 的 watch 变化监听全链路。
//
// 交互风格仿官方 Everything 的"参考测试面"：零凭证、能力齐全（tools.listChanged=true）。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

using Json = nlohmann::json;

namespace {

constexpr char kProtocol[] = "2025-03-26";
std::atomic<int> g_sess{0};  // 会话 id 序号（全局单调，跨连接不撞号）

// ------------- 共享状态 -------------
struct SseConn {
  int fd = -1;
  int last_gen = -1;  // 已发送到的变更代数
};

struct State {
  int port = 0;
  std::mutex mu;
  std::condition_variable cv;
  int change_gen = 0;                       // 工具集变更代数（SSE 靠它决定要不要写帧）
  std::vector<Json> tools;                  // 当前工具集
  std::unordered_set<std::string> sessions; // 已握手会话
  std::vector<std::shared_ptr<SseConn>> sse;  // 活跃 SSE 长连
};

Json tool_spec(std::string name, std::string desc, Json props = Json::object()) {
  return Json{{"name", std::move(name)},
              {"description", std::move(desc)},
              {"inputSchema",
               Json{{"type", "object"},
                    {"properties", std::move(props)}}}};
}

Json rpc_done(Json result) { return Json{{"jsonrpc", "2.0"}, {"result", std::move(result)}}; }
Json rpc_err(int code, std::string msg) {
  return Json{{"jsonrpc", "2.0"},
              {"error", Json{{"code", code}, {"message", std::move(msg)}}}};
}
Json text_content(std::string text) {
  return Json{{"content", Json::array({Json{{"type", "text"}, {"text", std::move(text)}}})}};
}

// 工具集变更 → 代数 +1 并唤醒 SSE 等待者（各 SSE 线程唤醒后自己写帧）。
void bump_tools(State& st) {
  {
    std::lock_guard g(st.mu);
    ++st.change_gen;
  }
  st.cv.notify_all();
}

Json serialized_tools(State& st) {
  std::lock_guard g(st.mu);
  Json out = Json::array();
  for (Json const& t : st.tools) out.push_back(t);
  return out;
}

// 一帧 list_changed 通知（真实 MCP 格式：event/ data 行）。
std::string list_changed_frame() {
  return "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}\n\n";
}

bool sse_write(int fd, std::string const& frame) {
  std::size_t sent = 0;
  while (sent < frame.size()) {
    ssize_t const n = ::write(fd, frame.data() + sent, frame.size() - sent);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

// ------------- 极简 HTTP 请求解析 -------------
struct HttpReq {
  std::string method;
  std::string path;
  std::string session;  // mcp-session-id 头（若有）
  std::string body;
};

std::size_t parse_content_length(std::string const& headers, std::size_t pos) {
  // headers 从请求行后开始；逐行找 Content-Length。
  // 关键：末行常常**没有尾部 CRLF**（\r\n\r\n 边界把它吃掉，如 curl 把 Content-Length 放最后）——
  // 遇 npos 要把剩余整段当末行处理，**不能 break**（否则永远漏掉最后一行）。
  while (pos < headers.size()) {
    std::size_t e = headers.find("\r\n", pos);
    bool const last = (e == std::string::npos);
    if (last) e = headers.size();
    std::string const h = headers.substr(pos, e - pos);
    std::size_t const colon = h.find(':');
    if (colon != std::string::npos) {
      std::string k = h.substr(0, colon);
      std::string v = h.substr(colon + 1);
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
      while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
      if (k == "Content-Length") return static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10));
    }
    if (last) break;
    pos = e + 2;
  }
  return 0;
}

// 从 fd 读一整个请求（请求行 + 头 + Content-Length 体），完整才返回。
std::optional<HttpReq> read_http_request(int fd) {
  std::string buf;
  char tmp[4096];
  while (true) {
    ssize_t const n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return std::nullopt;
    buf.append(tmp, static_cast<std::size_t>(n));

    std::size_t const sep = buf.find("\r\n\r\n");
    if (sep == std::string::npos) {
      if (buf.size() > 64 * 1024) return std::nullopt;  // 头过大防呆
      continue;
    }
    std::string const head = buf.substr(0, sep);

    HttpReq req;
    std::size_t const first_eol = head.find("\r\n");
    if (first_eol == std::string::npos) return std::nullopt;
    std::string const line = head.substr(0, first_eol);
    std::size_t const sp1 = line.find(' ');
    std::size_t const sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return std::nullopt;
    req.method = line.substr(0, sp1);
    req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);

    // 头区子串（请求行之后的各行）
    std::string const headers = head.substr(first_eol + 2);
    std::size_t const body_left = parse_content_length(headers, 0);

    std::size_t const avail = buf.size() - (sep + 4);
    if (avail >= body_left) {
      req.body = buf.substr(sep + 4, body_left);
      // 会话头
      std::size_t pos = 0;
      while (pos < headers.size()) {
        std::size_t e = headers.find("\r\n", pos);
        std::string const h = headers.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        std::size_t const colon = h.find(':');
        if (colon != std::string::npos && h.substr(0, colon) == "mcp-session-id") {
          std::string v = h.substr(colon + 1);
          while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
          req.session = std::move(v);
        }
        if (e == std::string::npos) break;
        pos = e + 2;
      }
      return req;
    }
    // 体还没读完，继续收。
  }
}

// ------------- 响应写入 -------------
void send_all(int fd, std::string const& s) {
  std::size_t sent = 0;
  while (sent < s.size()) {
    ssize_t const n = ::write(fd, s.data() + sent, s.size() - sent);
    if (n <= 0) return;
    sent += static_cast<std::size_t>(n);
  }
}

std::string status_reason(int code) {
  switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default: return "X";
  }
}

void reply_json(int fd, Json const& j, std::string const& extra_header = "") {
  std::string const body = j.dump();
  std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
  resp += extra_header + "Connection: close\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\n\r\n" + body;
  send_all(fd, resp);
}

void reply_text(int fd, int code, std::string const& body) {
  std::string resp = "HTTP/1.1 " + std::to_string(code) + " " + status_reason(code) +
                     "\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\n\r\n" + body;
  send_all(fd, resp);
}

// ------------- MCP 分发 -------------
Json dispatch_rpc(State& st, Json const& req, std::string const& session,
                  std::string& out_session_header) {
  if (!req.is_object() || !req.contains("method")) return rpc_err(-32600, "Invalid Request");

  std::string const method = req["method"];
  if (method == "initialize") {
    std::string const sid = "mock-sess-" + std::to_string(g_sess.fetch_add(1) + 1);
    {
      std::lock_guard g(st.mu);
      st.sessions.emplace(sid);
    }
    out_session_header = "mcp-session-id: " + sid + "\r\n";
    return rpc_done(Json{{"protocolVersion", kProtocol},
                         {"capabilities", Json{{"tools", Json{{"listChanged", true}}}}},
                         {"serverInfo", Json{{"name", "mock"},
                                             {"version", "0.1.0"},
                                             {"title", "cppchain Mock MCP"}}}});
  }

  // 除 initialize 外都要已握手会话（无会话 = "Server not initialized"）。
  bool const have_session = [&] {
    std::lock_guard g(st.mu);
    return st.sessions.count(session) > 0;
  }();
  if (session.empty() || !have_session)
    return rpc_err(-32000, "Bad Request: Server not initialized");

  if (method == "tools/list") {
    return rpc_done(Json{{"tools", serialized_tools(st)}});
  }
  if (method == "resources/list") {
    return rpc_done(Json{{"resources",
                          Json::array({Json{{"uri", "mock://demo/hello.txt"},
                                             {"name", "hello"},
                                             {"description", "演示资源"}}})}});
  }
  if (method == "tools/call") {
    std::string const name = req["params"].value("name", "");
    Json const args = req["params"].value("arguments", Json::object());

    if (name == "echo") {
      return rpc_done(text_content("Echo: " + args.value("message", "")));
    }
    if (name == "add") {
      int const a = args.value("a", 0), b = args.value("b", 0);
      return rpc_done(text_content(std::to_string(a + b)));
    }
    if (name == "add_tool" || name == "remove_tool") {
      std::string const target = args.value("name", "");
      if (target.empty()) return rpc_err(-32602, "缺 name");
      if (target == "add_tool" || target == "remove_tool")
        return rpc_err(-32602, "控制工具不可增删自身");

      if (name == "add_tool") {
        bool const dup = [&] {
          std::lock_guard g(st.mu);
          return std::any_of(st.tools.begin(), st.tools.end(),
                             [&](Json const& t) { return t["name"] == target; });
        }();
        if (dup) return rpc_err(-32602, "'" + target + "' 已存在");
        Json spec = tool_spec(target, args.value("description", "运行时新增工具"),
                              Json{{"q", Json{{"type", "string"}}}});
        {
          std::lock_guard g(st.mu);
          st.tools.push_back(std::move(spec));
        }
        bump_tools(st);
        return rpc_done(text_content("已新增工具 '" + target + "'"));
      }
      // remove_tool
      bool removed = false;
      {
        std::lock_guard g(st.mu);
        for (auto it = st.tools.begin(); it != st.tools.end(); ++it) {
          if ((*it)["name"] == target) { st.tools.erase(it); removed = true; break; }
        }
      }
      if (!removed) return rpc_err(-32602, "'" + target + "' 不存在");
      bump_tools(st);
      return rpc_done(text_content("已移除工具 '" + target + "'"));
    }
    // 运行时新增的工具（add_tool 加的）：存在于工具集即通用执行（mock 后端，どんな name 都响应）。
    bool const exists = [&] {
      std::lock_guard g(st.mu);
      return std::any_of(st.tools.begin(), st.tools.end(),
                         [&](Json const& t) { return t["name"] == name; });
    }();
    if (exists) return rpc_done(text_content("mock 工具 '" + name + "' 已执行"));
    return rpc_err(-32602, "工具不存在 '" + name + "'");
  }
  return rpc_err(-32601, "Method not found: " + method);
}

// ------------- 连接处理器 -------------
void handle_connection(State& st, int fd) {
  auto req = read_http_request(fd);
  if (!req) { ::close(fd); return; }

  if (req->method == "GET" && req->path == "/mcp.json") {
    // url:// 发现源端点：指向本 server 自身（运行时端口已知）。
    Json const list = Json{{"mcpServers",
                            Json::array({Json{{"name", "mock"},
                                              {"type", "remote"},
                                              {"url", "http://127.0.0.1:" +
                                                          std::to_string(st.port) + "/mcp"},
                                              {"transport", "http"},
                                              {"description", "cppchain mock MCP"}}})}};
    reply_json(fd, list);
    ::close(fd);
    return;
  }

  if (req->path != "/mcp") { reply_text(fd, 404, "not found"); ::close(fd); return; }

  if (req->method == "GET") {
    // SSE 通知长连（需已握手会话）。
    bool const ok = [&] {
      std::lock_guard g(st.mu);
      return st.sessions.count(req->session) > 0;
    }();
    if (!ok) {
      reply_json(fd, rpc_err(-32000, "Bad Request: Server not initialized"));
      ::close(fd);
      return;
    }
    auto conn = std::make_shared<SseConn>();
    conn->fd = fd;
    {
      std::lock_guard g(st.mu);
      conn->last_gen = st.change_gen;
      st.sse.push_back(conn);
    }
    std::string const head =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n\r\n";
    send_all(fd, head);
    std::fprintf(stderr, "[mcp_mock] SSE 长连接入（session=%s）\n", req->session.c_str());
    // 阻塞在变更代数的条件上：代数变了才写一帧（对端关闭时 sse_write 失败退出）。
    while (true) {
      std::unique_lock lk(st.mu);
      st.cv.wait(lk, [&] { return st.change_gen != conn->last_gen; });
      conn->last_gen = st.change_gen;
      lk.unlock();
      if (!sse_write(fd, list_changed_frame())) break;
    }
    ::close(fd);
    return;
  }

  if (req->method == "POST") {
    Json body = Json::parse(req->body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded() || !body.is_object()) {
      reply_text(fd, 400, "bad json");
      ::close(fd);
      return;
    }
    std::string sess_hdr;
    Json const resp = dispatch_rpc(st, body, req->session, sess_hdr);
    reply_json(fd, resp, sess_hdr);
    ::close(fd);
    return;
  }

  reply_text(fd, 405, "method not allowed");
  ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
  int port = 3100;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string const k = argv[i];
    if (k == "--port" || k == "-p") port = std::atoi(argv[i + 1]);
  }

  int const srv = ::socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) { std::fprintf(stderr, "[mcp_mock] socket 失败\n"); return 1; }
  int const reuse = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    std::fprintf(stderr, "[mcp_mock] bind %d 失败（端口被占?）\n", port);
    return 1;
  }
  if (::listen(srv, 8) != 0) { std::fprintf(stderr, "[mcp_mock] listen 失败\n"); return 1; }

  State st;
  st.port = port;
  st.tools.push_back(tool_spec("echo", "回显消息", Json{{"message", Json{{"type", "string"}}}}));
  st.tools.push_back(
      tool_spec("add", "整数加法", Json{{"a", Json{{"type", "integer"}}},
                                        {"b", Json{{"type", "integer"}}}}));
  st.tools.push_back(tool_spec(
      "add_tool", "运行时新增一个工具并推送 tools/list_changed",
      Json{{"name", Json{{"type", "string"}}}, {"description", Json{{"type", "string"}}}}));
  st.tools.push_back(tool_spec(
      "remove_tool", "运行时移除一个工具并推送 tools/list_changed",
      Json{{"name", Json{{"type", "string"}}}}));

  std::fprintf(stderr, "[mcp_mock] Mock MCP listening on 127.0.0.1:%d/mcp  (Ctrl+C 停)\n", port);
  std::fprintf(stderr, "[mcp_mock] 初始工具：echo, add, add_tool, remove_tool\n");

  while (true) {
    int const cfd = ::accept(srv, nullptr, nullptr);
    if (cfd < 0) {
      std::fprintf(stderr, "[mcp_mock] accept 失败，稍候重试\n");
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    std::thread([&st, cfd] { handle_connection(st, cfd); }).detach();
  }
}