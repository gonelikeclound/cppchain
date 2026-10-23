// MCP（Model Context Protocol）客户端 P1 测试：HTTP 同步 + JSON-RPC。
// 用 FakeTransport 离线脚本化 MCP server 响应；测 initialize / list_tools / call_tool /
// make_mcp_tool 的 handler / 错误路径（HTTP 非 2xx、JSON-RPC error、缺 result）。
// 纪律：断言请求内容（MCP-Protocol-Version 头、JSON-RPC body、id 单调）与响应解析。
#include <gtest/gtest.h>

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
  return FakeTransport::Script{.status = status, .chunks = {std::move(raw)}, .error = {},
                               .timeout = false, .delay_ms = 0};
}
FakeTransport::Script script_fail(std::string message) {
  return FakeTransport::Script{.status = 0, .chunks = {}, .error = std::move(message),
                               .timeout = false, .delay_ms = 0};
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