// MCP（Model Context Protocol）客户端 P1 测试：HTTP 同步 + JSON-RPC。
// 用 FakeTransport 离线脚本化 MCP server 响应；测 initialize / list_tools / call_tool /
// make_mcp_tool 的 handler / 错误路径（HTTP 非 2xx、JSON-RPC error、缺 result）。
// 纪律：断言请求内容（MCP-Protocol-Version 头、JSON-RPC body、id 单调）与响应解析。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/mcp/client.hpp"

using namespace cppchain;
using namespace cppchain::http;
using namespace cppchain::mcp;
using cppchain::tool::ToolContext;
using Json = nlohmann::json;

namespace {

// 依次 push 脚本响应（每 script 对应一次 request）。
struct McpHarness {
  FakeTransport tx;
  MCPClient client;
  McpHarness(std::string base = "http://mcp.test/rpc")
      : client(tx, std::move(base), /*headers=*/{"Authorization: Bearer test-token"},
               /*server_name=*/"echo-server") {}
};

Json rpc_result(Json result) {
  return Json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", std::move(result)}};
}
Json rpc_error(int code, std::string msg) {
  return Json{{"jsonrpc", "2.0"},
              {"id", 1},
              {"error", Json{{"code", code}, {"message", std::move(msg)}}}};
}
Json ok_initialize() {
  return rpc_result(Json{{"protocolVersion", "2025-03-26"},
                         {"capabilities", Json{{"tools", Json{{"listChanged", true}}}}}});
}
Json ok_tools_list(std::vector<Json> tools) {
  return rpc_result(Json{{"tools", std::move(tools)}});
}
Json ok_call_text(std::string text) {
  return rpc_result(Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})}});
}

// ---- FakeTransport::Script 构造：字段显式给全，零告警（-Wmissing-field-initializers）----
FakeTransport::Script script_raw(long status, std::string raw) {
  return FakeTransport::Script{.status = status, .chunks = {std::move(raw)}, .headers = {},
                               .error = {}, .timeout = false, .delay_ms = 0};
}
FakeTransport::Script script_fail(std::string message) {
  return FakeTransport::Script{.status = 0, .chunks = {}, .headers = {},
                               .error = std::move(message), .timeout = false, .delay_ms = 0};
}
}  // namespace

// initialize：返回 capabilities；请求带 MCP 版本头 + Bearer。
TEST(McpClient, initialize_handshake_and_headers) {
  McpHarness h;
  h.tx.push(script_raw(200, ok_initialize().dump()));
  auto r = h.client.initialize();
  ASSERT_TRUE(r.ok()) << r.error;
  EXPECT_TRUE(r.result["capabilities"]["tools"].is_object());

  ASSERT_EQ(h.tx.requests().size(), 1u);
  HttpRequest const& req = h.tx.requests()[0];
  EXPECT_EQ(req.url, "http://mcp.test/rpc");
  EXPECT_EQ(req.method, "POST");
  EXPECT_TRUE(req.body.find("\"method\":\"initialize\"") != std::string::npos);
  EXPECT_TRUE(req.body.find("\"protocolVersion\":\"2025-03-26\"") != std::string::npos);
  bool saw_mcp_ver = false, saw_auth = false, saw_json = false;
  for (std::string const& hdr : req.headers) {
    if (hdr.starts_with("MCP-Protocol-Version:")) saw_mcp_ver = true;
    if (hdr.starts_with("Authorization:")) saw_auth = true;
    if (hdr.starts_with("Content-Type:")) saw_json = true;
  }
  EXPECT_TRUE(saw_mcp_ver);
  EXPECT_TRUE(saw_auth);
  EXPECT_TRUE(saw_json);
}

// list_tools：解析 tools 数组 → McpToolInfo（name/description/input_schema）。
TEST(McpClient, list_tools_parses_schema) {
  McpHarness h;
  h.tx.push(script_raw(200, ok_tools_list(
      Json::array({
          Json{{"name", "get_weather"}, {"description", "查天气"},
               {"inputSchema", Json{{"type", "object"},
                                    {"properties", Json{{"city", Json{{"type", "string"}}}}}}}},
          Json{{"name", "no_desc"},
               {"inputSchema", Json{{"type", "object"}, {"properties", Json::object()}}}}}))
      .dump()));
  auto tools = h.client.list_tools();
  ASSERT_TRUE(tools.has_value()) << tools.error();
  ASSERT_EQ(tools->size(), 2u);
  EXPECT_EQ((*tools)[0].name, "get_weather");
  EXPECT_EQ((*tools)[0].description, "查天气");
  EXPECT_EQ((*tools)[0].input_schema["properties"]["city"]["type"], "string");
  EXPECT_EQ((*tools)[1].name, "no_desc");
  // 真机发现（P3 批① · Everything）：显式 cursor:null 会被严格校验的 server 拒 —— 必须省略。
  EXPECT_EQ(h.tx.requests()[0].body.find("\"cursor\""), std::string::npos);
}

// call_tool：调用返回 content text。
TEST(McpClient, call_tool_returns_text_content) {
  McpHarness h;
  h.tx.push(script_raw(200, ok_call_text("晴，23°C").dump()));
  auto r = h.client.call_tool("get_weather", Json{{"city", "上海"}});
  ASSERT_TRUE(r.ok()) << r.error;
  EXPECT_EQ(r.result["content"][0]["text"], "晴，23°C");
  EXPECT_TRUE(h.tx.requests()[0].body.find("\"method\":\"tools/call\"") != std::string::npos);
}

// make_mcp_tool：handler 调 client → 拼 text 进 ToolSpec；schema 直用 MCP inputSchema。
TEST(McpClient, make_mcp_tool_handler_calls_client) {
  McpHarness h;
  McpToolInfo info;
  info.name = "get_weather";
  info.description = "查天气";
  info.input_schema = Json{{"type", "object"},
                           {"properties", Json{{"city", Json{{"type", "string"}}}}}};
  auto spec = make_mcp_tool(h.client, info, /*read_only=*/true);

  EXPECT_EQ(spec.name, "get_weather");
  EXPECT_TRUE(spec.read_only);
  EXPECT_EQ(spec.parameters["properties"]["city"]["type"], "string");

  // server 返回 text content → handler 得 {"text": ...}
  h.tx.push(script_raw(200, ok_call_text("晴，23°C").dump()));
  ToolContext ctx;
  auto r = spec.handler(Json{{"city", "上海"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ((*r)["text"], "晴，23°C");
  EXPECT_EQ(h.client.server_name(), "echo-server");
}

// 错误路径：JSON-RPC error → error 非空。
TEST(McpClient, jsonrpc_error_surfaces) {
  McpHarness h;
  h.tx.push(script_raw(200, rpc_error(-32601, "method not found").dump()));
  auto r = h.client.initialize();
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error.find("-32601"), std::string::npos);
  EXPECT_NE(r.error.find("method not found"), std::string::npos);
}

// 错误路径：HTTP 非 2xx → error 带状态码。
TEST(McpClient, http_non_2xx_surfaces) {
  McpHarness h;
  h.tx.push(script_raw(401, R"({"error":"unauth"})"));
  auto r = h.client.list_resources();
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error.find("401"), std::string::npos);
}

// 错误路径：传输层失败（FakeTransport error）→ error 为传输消息。
TEST(McpClient, transport_failure_surfaces) {
  McpHarness h;
  h.tx.push(script_fail("connection refused"));
  auto r = h.client.initialize();
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error.find("connection refused"), std::string::npos);
}

// 错误路径：缺 result → error。
TEST(McpClient, missing_result_surfaces) {
  McpHarness h;
  h.tx.push(script_raw(200, R"({"jsonrpc":"2.0","id":1})"));
  auto r = h.client.initialize();
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error.find("缺 result"), std::string::npos);
}

// 无 text content、带 structuredContent → handler 返回 structuredContent。
TEST(McpClient, make_mcp_tool_structured_content) {
  McpHarness h;
  McpToolInfo info;
  info.name = "get_price";
  info.input_schema = Json{{"type", "object"}};
  auto spec = make_mcp_tool(h.client, info);
  // server 返回 {structuredContent:{price:799}, content:[]}
  Json structured_result = Json::object();  // 显式对象构型，避免嵌套列表初始化歧义
  structured_result["structuredContent"] = Json{{"price", 799}};
  structured_result["content"] = Json::array();
  h.tx.push(script_raw(200, rpc_result(structured_result).dump()));
  ToolContext ctx;
  auto r = spec.handler(Json::object(), ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ((*r)["price"], 799);
}

// ================= P3 批①：SSE 单发响应解析 + 会话头回带 =================
// 帧格式以真实 Everything server（streamableHttp）的实测为准：event: message / id: / data: 行。

// SSE 单发消息（非纯 JSON 体）→ 正常解出 result。
TEST(McpClient, sse_single_message_response_parsed) {
  McpHarness h;
  h.tx.push(script_raw(
      200, "event: message\n"
           "id: 87fc36cc-bac1-47b1-9bc8-ad7dbbd17ad7\n"
           "data: " + std::string(ok_initialize().dump()) + "\n\n"));
  auto r = h.client.initialize();
  ASSERT_TRUE(r.ok()) << r.error;
  EXPECT_TRUE(r.result["capabilities"]["tools"].is_object());
}

// SSE 帧里的 JSON-RPC error → error 上浮带 code+message。
TEST(McpClient, sse_error_frame_surfaces) {
  McpHarness h;
  h.tx.push(script_raw(
      200, "event: message\n"
           "data: " + std::string(rpc_error(-32601, "method not found").dump()) + "\n"));
  auto r = h.client.initialize();
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error.find("-32601"), std::string::npos);
  EXPECT_NE(r.error.find("method not found"), std::string::npos);
}

// initialize 响应头带会话 id（大小写不敏感的键名）→ 记入 client，后续请求自动回带会话头。
TEST(McpClient, session_id_roundtrip_sent_on_subsequent_requests) {
  McpHarness h;
  h.tx.push(FakeTransport::Script{.status = 200, .chunks = {ok_initialize().dump()},
                                  .headers = {"mcp-session-id: sess-abc-123"},
                                  .error = {}, .timeout = false, .delay_ms = 0});
  auto r = h.client.initialize();
  ASSERT_TRUE(r.ok()) << r.error;
  EXPECT_EQ(h.client.session_id(), "sess-abc-123");

  // 二次请求（tools/list）必须回带 mcp-session-id 头。
  h.tx.push(script_raw(200, ok_tools_list(
      Json::array({Json{{"name", "get_weather"},
                        {"description", "查天气"},
                        {"inputSchema", Json{{"type", "object"}}}}}))
      .dump()));
  auto tools = h.client.list_tools();
  ASSERT_TRUE(tools.has_value()) << tools.error();
  ASSERT_EQ(h.tx.requests().size(), 2u);
  bool const saw_session = std::any_of(
      h.tx.requests()[1].headers.begin(), h.tx.requests()[1].headers.end(),
      [](std::string const& hdr) { return hdr.starts_with("mcp-session-id: sess-abc-123"); });
  EXPECT_TRUE(saw_session);
  EXPECT_EQ(tools->size(), 1u);
}

// 服务端用大写变体键名（Mcp-Session-Id）→ 客户端大小写不敏感地取到。
TEST(McpClient, session_id_key_case_insensitive) {
  McpHarness h;
  h.tx.push(FakeTransport::Script{.status = 200, .chunks = {ok_initialize().dump()},
                                  .headers = {"Mcp-Session-Id: sess-ABC"}, .error = {},
                                  .timeout = false, .delay_ms = 0});
  ASSERT_TRUE(h.client.initialize().ok());
  EXPECT_EQ(h.client.session_id(), "sess-ABC");
}

// 没有会话头 → 请求不额外带 mcp-session-id。
TEST(McpClient, no_session_header_when_none) {
  McpHarness h;
  h.tx.push(script_raw(200, ok_initialize().dump()));
  ASSERT_TRUE(h.client.initialize().ok());
  EXPECT_TRUE(h.client.session_id().empty());

  h.tx.push(script_raw(200, ok_tools_list(Json::array()).dump()));
  ASSERT_TRUE(h.client.list_tools().has_value());
  for (std::string const& hdr : h.tx.requests()[1].headers)
    EXPECT_EQ(hdr.find("mcp-session-id"), std::string::npos);
}

// 无会话被拒（HTTP 400 + JSON-RPC error 体）→ 上浮可读的 RPC 错误，而非笼统 "HTTP 400"。
TEST(McpClient, session_required_400_reports_rpc_error) {
  McpHarness h;
  h.tx.push(script_raw(200, ok_initialize().dump()));
  ASSERT_TRUE(h.client.initialize().ok());
  h.tx.push(script_raw(400, R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"Server not initialized"},"id":null})"));
  auto r = h.client.list_tools();
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().find("Server not initialized"), std::string::npos);
  EXPECT_NE(r.error().find("-32000"), std::string::npos);
}