// cppchain —— mcp/client.hpp
// MCP（Model Context Protocol）客户端（首期：HTTP/SSE transport）。
// 分层约束：只依赖 nlohmann + http::Transport **接口**（不链 libcurl）——真传输由使用方注入
// （CurlMultiTransport 或测试的 FakeTransport），与 web_search/weather 同层纪律。
//
// MCP 协议：JSON-RPC 2.0 over HTTP POST（首期同步）。四个方法打通：
//   initialize   —— 握手（协议版本 + client info），server 返回 capabilities
//   tools/list   —— 列出 server 暴露的工具（inputSchema 即 JSON Schema，可直转 ToolSpec）
//   tools/call   —— 调一个工具（args 是任意 JSON）
//   resources/list —— 列出资源（预留；首期仅打通方法）
// 变化监听（tools/list 轮询 diff）与 SSE 服务端推送属 P3，不在本头。
//
// make_mcp_tool：把 MCP 工具转成 ToolSpec（handler 里 client->call_tool），
// 与 make_web_search_tool 同构——外部 API → handler → 注册表 → workflow 消费。
#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/http/transport.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::mcp {

// 一次 MCP 调用的结果：成功返回 result（JSON），失败返回 error 消息。
struct McpResult {
  std::string error;  // 非空 = 失败（JSON-RPC error 或传输层错误）
  long http_status = 0;
  nlohmann::json result = nlohmann::json::object();
  [[nodiscard]] bool ok() const { return error.empty(); }
};

// 一个 MCP 工具的服务端描述（tools/list 返回的 schema）。
struct McpToolInfo {
  std::string name;
  std::string description;
  nlohmann::json input_schema = nlohmann::json::object();  // JSON Schema
};

class MCPClient {
 public:
  // transport 非拥有：调用方保证其寿命长于 client（同 web_search 的注入纪律）。
  MCPClient(http::Transport& transport, std::string base_url,
            std::vector<std::string> headers = {}, std::string server_name = "mcp")
      : transport_(&transport), base_url_(std::move(base_url)),
        headers_(std::move(headers)), server_name_(std::move(server_name)) {}

  [[nodiscard]] std::string const& server_name() const { return server_name_; }

  // JSON-RPC 调用：method + params → 期望 result 的 content（error 也返回 error 文本）。
  [[nodiscard]] McpResult call(std::string_view method, nlohmann::json params);

  // initialize：握手，返回 server capabilities（result 原样）。失败 → error 非空。
  [[nodiscard]] McpResult initialize();

  // tools/list：列出工具。失败 → error 非空（调用方转 ToolError）。
  [[nodiscard]] std::expected<std::vector<McpToolInfo>, std::string> list_tools();

  // tools/call：调一个工具。result 为任意 JSON（MCP 约定含 content 数组，调用方按需取）。
  [[nodiscard]] McpResult call_tool(std::string const& name, nlohmann::json args);

  // resources/list：列出资源（首期打通方法，消费端后续接）。
  [[nodiscard]] McpResult list_resources();

  // 已建立的 MCP 会话 id（initialize 握手响应头的 mcp-session-id；空 = 尚未握手成功）。
  [[nodiscard]] std::string const& session_id() const { return session_id_; }

 private:
  http::Transport* transport_;
  std::string base_url_;
  std::vector<std::string> headers_;
  std::string server_name_;
  std::string session_id_;  // streamable-http 会话：握手后回带 mcp-session-id 头
};

// MCP 工具 → ToolSpec。handler 里 client->call_tool(name,args)，返回 result 的 content 文本
//（MCP 约定 result = {content:[{type:"text",text}]} 或其它；统一取 text 拼接）。
// schema = inputSchema（已是 JSON Schema，直用）。read_only 由调用方决定（默认 true=工具多为查询）。
[[nodiscard]] tool::ToolSpec make_mcp_tool(MCPClient& client, McpToolInfo const& info,
                                           bool read_only = true);

}  // namespace cppchain::mcp