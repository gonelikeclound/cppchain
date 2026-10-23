// cppchain —— mcp/list_schema.hpp
// 统一 MCP 服务发现列表 schema。**三条发现路线（so:// / file:// / url://）返回的格式完全一致**，
// 都是这个 JSON 结构 + parse 函数解析。任何一条路返回别样格式 → fail-closed。
//
// 列表格式：
// {
//   "mcpServers": [
//     {
//       "name": "weather",            // LLM 工具展示名（也是 load/from 引用名）
//       "type": "remote",             // "local" | "remote"
//       "url": "https://...",         // 连接地址（remote 必需；local 是进程启动描述）
//       "transport": "http",          // "http" | "sse"（P1 已支持 http；sse 后续期）
//       "meta": { "token": "..." },   // 连接凭证（运行时注入 MCPClient，**不进 workflow JSON/state/results**）
//       "description": "..."          // 可选，供 audit/展示
//     }
//   ]
// }
//
// 安全纪律：meta 是敏感凭证。parse 返回后，发布进 results/state 前必须剥掉（见 manager 的
// strip_meta / provider 节点的发布路径）。
#pragma once

#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cppchain::mcp {

struct McpServerInfo {
  std::string name;
  std::string type;              // "local" | "remote"
  std::string url;               // remote: 连接 URL；local: 进程启动路径（P5 stdio 用）
  std::string transport;         // "http" | "sse"
  nlohmann::json meta = nlohmann::json::object();  // 连接凭证（token 等；不进 workflow 状态）
  std::string description;
};

// 解析统一 MCP 列表 JSON。任何一条路返回别样结构 → 返回错误（fail-closed）。
[[nodiscard]] std::expected<std::vector<McpServerInfo>, std::string>
parse_mcp_server_list(nlohmann::json const& j);

// 把 meta 剥掉后的列表（provider 节点发布进 results/state 前用——token 不外泄）。
[[nodiscard]] nlohmann::json strip_meta(std::vector<McpServerInfo> const& servers);

}  // namespace cppchain::mcp