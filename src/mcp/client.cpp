// cppchain —— mcp/client.cpp
// MCP 客户端实现：JSON-RPC 2.0 over HTTP（同步，复用 tool::http_json 桥接异步 Transport）。
#include "cppchain/mcp/client.hpp"

#include <utility>

#include "cppchain/core/cancel.hpp"
#include "cppchain/tool/http_json.hpp"

namespace cppchain::mcp {

using Json = nlohmann::json;

namespace {
// MCP 协议版本（2025-03-26 稳定版）。每次请求都要带在头里（服务端校验）。
constexpr char kMcpProtocolVersion[] = "2025-03-26";
std::atomic<int64_t> g_id{0};  // JSON-RPC id（单调）
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
  hreq.headers.insert(hreq.headers.end(), headers_.begin(), headers_.end());

  auto resp = tool::http_json(*transport_, hreq, core::CancelToken{});
  if (!resp) return McpResult{.error = resp.error().message};
  if (resp->status != 0 && (resp->status < 200 || resp->status >= 300))
    return McpResult{.error = "MCP HTTP " + std::to_string(resp->status),
                     .http_status = resp->status};
  if (resp->body.is_discarded() || !resp->body.is_object())
    return McpResult{.error = "MCP 响应非 JSON 对象", .http_status = resp->status};
  // JSON-RPC：result 或 error。
  if (resp->body.contains("error") && resp->body["error"].is_object()) {
    auto const& e = resp->body["error"];
    return McpResult{.error = "MCP error "
                              + (e.contains("code") ? std::to_string(e["code"].get<int64_t>())
                                                    : std::string("?"))
                              + ": " + e.value("message", "（无消息）"),
                     .http_status = resp->status};
  }
  if (!resp->body.contains("result"))
    return McpResult{.error = "MCP 响应缺 result", .http_status = resp->status};
  return McpResult{.http_status = resp->status, .result = resp->body["result"]};
}

McpResult MCPClient::initialize() {
  return call("initialize", Json{{"protocolVersion", kMcpProtocolVersion},
                                 {"capabilities", Json::object()},
                                 {"clientInfo", Json{{"name", "cppchain"}, {"version", "0.1.0"}}}});
}

std::expected<std::vector<McpToolInfo>, std::string> MCPClient::list_tools() {
  auto r = call("tools/list", Json{{"cursor", Json(nullptr)}});
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
  return call("resources/list", Json{{"cursor", Json(nullptr)}});
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