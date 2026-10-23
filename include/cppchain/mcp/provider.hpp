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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppchain/mcp/listen.hpp"
#include "cppchain/mcp/manager.hpp"
#include "cppchain/tool/registry.hpp"

namespace cppchain::mcp {

// provider 的发布面：server 列表（已 strip_meta，无凭证）+ 实际注册进 registry 的工具全名。
// 命中 opts.watch 的 server → listeners 里是活跃的变化监听（长连接在挂）。
// **move-only**（listen 持有唯一句柄）：保有本结果即保有监听。
struct McpProviderResult {
  nlohmann::json servers;        // strip_meta() 数组（{name,type,transport,...}，无 meta）
  std::vector<std::string> tools;  // registry 里的工具全名（`<server>_<tool>`）
  std::vector<std::unique_ptr<McpListener>> listeners;  // 变化监听句柄（P3 批②）
};

// provider 可选项。默认全空 = P2 原行为：只装配不监听，**零长连接**。
struct McpProviderOptions {
  // 要点名"挂变化监听"的 server 名（默认空）。规则：
  //   · 名字必须出现在发现的 mcpServers 里（点名了却不在 → fail-closed 整体报错）；
  //   · server 须声明 tools.listChanged 才真挂流，未声明 → listeners 里放一个带 error 的
  //     句柄（观察位："为什么没挂"），不当作报错。
  std::vector<std::string> watch;
};

// 从发现源 from 装配 MCP 工具进 registry。失败 → 整体失败（部分 server 已建连的缓存仍留在
// manager 池里，但 registry 不改动）。transport/so_dirs 由 manager 构造时注入。
// opts.watch 非空 → 额外为点名 server 挂变化监听（见 McpProviderOptions）。
[[nodiscard]] std::expected<McpProviderResult, std::string>
mcp_provider(MCPConnectionManager& mgr, std::string_view from, tool::ToolRegistry& registry,
             McpProviderOptions const& opts = {});

}  // namespace cppchain::mcp