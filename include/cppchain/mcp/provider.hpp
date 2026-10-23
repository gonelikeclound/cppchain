// cppchain —— mcp/provider.hpp
// MCP 二期：provider 节点 —— 从一条发现源装配 MCP 工具进 tool_registry（P2 的
// "tool_registry 加 type:'mcp' 源"）。链路：discover(from) → 每 server 建连+握手 → list_tools
// → make_mcp_tool → registry.add。
//
// 安全纪律：
//   · 发布面（McpProviderResult.servers）走 strip_meta —— token 绝不进 workflow 状态/results；
//   · 注册是"两段式"：先全部建连 + list_tools（任一失败整体失败，返回错误），成功后才一次性
//     add —— 与 load_dynamic_tools 同语义"任一失败整体失败"，不留半注册。
//   · 工具全名 = 清洗后的 `<server>_<tool>`（收紧到 [A-Za-z0-9_-]）——跨 server 工具撞名防
//     不住（两个 MCP server 都暴露 current 之类），前缀天然隔开。
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/mcp/manager.hpp"
#include "cppchain/tool/registry.hpp"

namespace cppchain::mcp {

// provider 的发布面：server 列表（已 strip_meta，无凭证）+ 实际注册进 registry 的工具全名。
struct McpProviderResult {
  nlohmann::json servers;        // strip_meta() 数组（{name,type,transport,...}，无 meta）
  std::vector<std::string> tools;  // registry 里的工具全名（`<server>_<tool>`）
};

// 从发现源 from 装配 MCP 工具进 registry。失败 → 整体失败（部分 server 已建连的缓存仍留在
// manager 池里，但 registry 不改动）。transport/so_dirs 由 manager 构造时注入。
[[nodiscard]] std::expected<McpProviderResult, std::string>
mcp_provider(MCPConnectionManager& mgr, std::string_view from, tool::ToolRegistry& registry);

}  // namespace cppchain::mcp