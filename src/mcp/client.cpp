// cppchain —— mcp/client.cpp
// MCP 客户端实现：JSON-RPC 2.0 over HTTP（同步，复用 tool::http_json 桥接异步 Transport）。
// P3 批①：SSE 单发响应解析（真实 streamable-http 服务器连单发结果也用 text/event-stream 帧
// 包裹）+ mcp-session-id 会话头回带（initialize 握手响应头里取，后续请求自动带上）。
#include "cppchain/mcp/client.hpp"

#include <cctype>
#include <optional>
#include <string>
#include <utility>

#include "cppchain/core/cancel.hpp"
#include "cppchain/tool/http_json.hpp"

namespace cppchain::mcp {

using Json = nlohmann::json;

namespace {
// MCP 协议版本（2025-03-26 稳定版）。每次请求都要带在头里（服务端校验）。
constexpr char kMcpProtocolVersion[] = "2025-03-26";
std::atomic<int64_t> g_id{0};  // JSON-RPC id（单调）

// 从响应体抽 MCP 消息：纯 JSON 对象直接用；SSE 帧（event: message / data: {…}，data 可多行）
// 按行抽 data 拼接后解析。拿不到合法 JSON 对象 → nullopt（fail-closed 交回调用方报错）。
std::optional<Json> parse_sse_or_json(std::string_view raw) {
  Json direct = Json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (!direct.is_discarded() && direct.is_object()) return direct;

  std::string data;
  std::optional<Json> out;
  auto try_flush = [&] {
    if (!data.empty() && !out) {
      Json j = Json::parse(data, nullptr, /*allow_exceptions=*/false);
      if (!j.is_discarded() && j.is_object()) out = std::move(j);
    }
    data.clear();
  };
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t const nl = raw.find('\n', pos);
    std::string_view line = raw.substr(pos, nl == std::string_view::npos ? raw.size() - pos
                                                                        : nl - pos);
    pos = nl == std::string_view::npos ? raw.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.starts_with("data:")) {
      auto v = line.substr(5);
      if (!v.empty() && v.front() == ' ') v.remove_prefix(1);
      if (!data.empty()) data += '\n';
      data.append(v);
    } else if (line.empty()) {
      try_flush();  // 事件边界
      if (out) break;
    }
  }
  try_flush();  // 末尾无空行的事件
  return out;
}

// 从响应头取 mcp-session-id（键名大小写不敏感）。
std::string session_id_from_headers(std::vector<std::string> const& headers) {
  for (std::string const& h : headers) {
    auto const colon = h.find(':');
    if (colon == std::string::npos) continue;
    std::string key;
    key.reserve(colon);
    for (char c : std::string_view(h).substr(0, colon))
      if (c != ' ' && c != '\t') key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (key != "mcp-session-id") continue;
    std::string_view v(h.data() + colon + 1, h.size() - colon - 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
      v.remove_suffix(1);
    return std::string(v);
  }
  return {};
}
}  // namespace

McpResult MCPClient::call(std::string_view method, Json params) {
  Json req = Json{{"jsonrpc", "2.0"}, {"id", ++g_id},
                  {"method", std::string(method)}, {"params", std::move(params)}};

  http::HttpRequest hreq;
  hreq.url = base_url_;
  hreq.method = "POST";
  hreq.body = req.dump();
  hreq.headers = {"Content-Type: application/json", "Accept: application/json, text/event-stream",
                  std::string("MCP-Protocol-Version: ") + kMcpProtocolVersion};
  if (!session_id_.empty()) hreq.headers.push_back("mcp-session-id: " + session_id_);
  hreq.headers.insert(hreq.headers.end(), headers_.begin(), headers_.end());

  auto resp = tool::http_json(*transport_, hreq, core::CancelToken{});
  if (!resp) return McpResult{.error = resp.error().message, .http_status = 0, .result = {}};

  // 会话回带：任何响应头带 mcp-session-id 就记下（initialize 握手一般首发）。
  if (auto const sid = session_id_from_headers(resp->headers); !sid.empty()) session_id_ = sid;

  // 消息体可能是纯 JSON，也可能 SSE 帧包裹 —— 统一抽成 JSON 对象。
  auto const payload = parse_sse_or_json(resp->raw);
  if (!payload)
    return McpResult{.error = "MCP 响应非 JSON 对象/SSE 帧", .http_status = resp->status, .result = {}};
  Json const& body = *payload;

  long const status = resp->status;
  bool const non_2xx = status != 0 && (status < 200 || status >= 300);
  auto const rpc_err = [&]() -> std::string {
    auto const& e = body["error"];
    return std::string("MCP error ")
           + (e.contains("code") ? std::to_string(e["code"].get<int64_t>()) : std::string("?"))
           + ": " + e.value("message", "（无消息）");
  };

  if (body.contains("error") && body["error"].is_object())
    return McpResult{.error = rpc_err(), .http_status = status, .result = {}};  // 400+ 的 RPC 错误更可读
  if (non_2xx) return McpResult{.error = "MCP HTTP " + std::to_string(status), .http_status = status, .result = {}};
  if (!body.contains("result"))
    return McpResult{.error = "MCP 响应缺 result", .http_status = status, .result = {}};
  // 成功：result 原样返回（error 缺省空 = ok）。
  return McpResult{.error = {}, .http_status = status, .result = body["result"]};
}

McpResult MCPClient::initialize() {
  auto r = call("initialize", Json{{"protocolVersion", kMcpProtocolVersion},
                                 {"capabilities", Json::object()},
                                 {"clientInfo", Json{{"name", "cppchain"}, {"version", "0.1.0"}}}});
  // 失败即清空 capabilities（fail-closed：别拿上一个会话的能力当这次用）。
  auto const cap = r.result.find("capabilities");
  capabilities_ = (r.ok() && cap != r.result.end() && cap->is_object())
                      ? *cap
                      : Json::object();
  return r;
}

std::expected<void, std::string> MCPClient::listen(http::StreamCallbacks scbs,
                                                    core::CancelToken token) {
  auto* sp = dynamic_cast<http::StreamProvider*>(transport_);
  if (sp == nullptr)
    return std::unexpected("传输层不支持长连接（未实现 StreamProvider）");
  http::StreamRequest req;
  req.url = base_url_;
  req.method = "GET";
  req.headers = {"Accept: text/event-stream"};
  if (!session_id_.empty()) req.headers.push_back("mcp-session-id: " + session_id_);
  sp->open_stream(req, std::move(scbs), std::move(token));
  return {};
}

std::expected<std::vector<McpToolInfo>, std::string> MCPClient::list_tools() {
  // cursor 可选，**缺省省略**——显式 null 会被严格校验的 server（官方 Everything）拒掉。
  auto r = call("tools/list", Json::object());
  if (!r.ok()) return std::unexpected(r.error);
  auto const tools_it = r.result.find("tools");
  if (tools_it == r.result.end() || !tools_it->is_array())
    return std::unexpected("MCP tools/list 缺 tools 数组");
  std::vector<McpToolInfo> out;
  for (Json const& t : *tools_it) {
    if (!t.is_object()) continue;
    McpToolInfo info;
    info.name = t.value("name", "");
    info.description = t.value("description", "");
    if (t.contains("inputSchema") && t["inputSchema"].is_object())
      info.input_schema = t["inputSchema"];
    out.push_back(std::move(info));
  }
  return out;
}

McpResult MCPClient::call_tool(std::string const& name, Json args) {
  return call("tools/call", Json{{"name", name}, {"arguments", std::move(args)}});
}

McpResult MCPClient::list_resources() {
  return call("resources/list", Json::object());  // 同 tools/list：cursor 省略
}

// MCP 工具的 result 形如 {content:[{type:"text",text}], structuredContent:{...}, isError}。
// 优先 text 段落；无 text 但带 structuredContent → 原样返回结构化；都没有 → dump 整个 result。
namespace {
std::string result_to_text(Json const& result) {
  auto const content = result.find("content");
  if (content != result.end() && content->is_array()) {
    std::string out;
    for (Json const& blk : *content) {
      if (blk.is_object() && blk.value("type", "") == "text" && blk.contains("text") &&
          blk["text"].is_string()) {
        if (!out.empty()) out += '\n';
        out += blk["text"].get<std::string>();
      }
    }
    if (!out.empty()) return out;
  }
  return {};  // 无 text → 由 handler 回退 structuredContent / 原样 result
}
}  // namespace

tool::ToolSpec make_mcp_tool(MCPClient& client, McpToolInfo const& info, bool read_only) {
  tool::ToolSpec spec;
  spec.name = info.name;
  spec.description = info.description.empty() ? "MCP 工具 " + info.name : info.description;
  // MCP inputSchema 已是 JSON Schema；tool 层要求 object+properties（缺则补空壳）。
  spec.parameters = info.input_schema.is_object() && !info.input_schema.is_null()
                        ? info.input_schema
                        : Json{{"type", "object"}, {"properties", Json::object()}};
  if (spec.parameters.is_object() && !spec.parameters.contains("type"))
    spec.parameters["type"] = "object";
  if (spec.parameters.is_object() && !spec.parameters.contains("properties"))
    spec.parameters["properties"] = Json::object();
  spec.read_only = read_only;
  spec.handler = [&client, name = info.name](Json const& args, tool::ToolContext&)
                     -> std::expected<Json, tool::ToolError> {
    auto r = client.call_tool(name, args);
    if (!r.ok()) return std::unexpected(tool::ToolError{r.error});
    std::string text = result_to_text(r.result);
    if (!text.empty()) return Json{{"text", std::move(text)}};
    // 结构化 content（MCP tools/call 的 structuredContent 槽）→ 原样返回。
    auto const sc = r.result.find("structuredContent");
    if (sc != r.result.end() && sc->is_object()) return *sc;
    return r.result;  // 兜底：原样整个 result
  };
  return spec;
}

}  // namespace cppchain::mcp