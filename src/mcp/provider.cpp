// cppchain —— mcp/provider.cpp
// MCP provider 节点实现：两段式装配（见 provider.hpp 头注释）。
#include "cppchain/mcp/provider.hpp"

#include <cctype>
#include <utility>

namespace cppchain::mcp {

namespace {

// 收紧到 OpenAI 函数名语料 [A-Za-z0-9_-]：其余字符换 '_'，空名兜底 "mcp"。
std::string sanitize_part(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    unsigned char const u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '_' || c == '-') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  if (out.empty()) out = "mcp";
  return out;
}

}  // namespace

std::expected<McpProviderResult, std::string> mcp_provider(MCPConnectionManager& mgr,
                                                           std::string_view from,
                                                           tool::ToolRegistry& registry) {
  auto servers = mgr.discover(from);
  if (!servers) return std::unexpected(std::move(servers.error()));

  // 第一阶段：全部建连 + list_tools（不改 registry）。任一失败 → 整体失败。
  // pending 汇总每个 server 的工具（client 指针存活于 manager 池，注册时直接用）。
  struct Pending {
    MCPClient* client;
    McpToolInfo tool;
  };
  std::vector<Pending> pending;
  for (McpServerInfo const& info : *servers) {
    auto client = mgr.client_for(info);
    if (!client) return std::unexpected("MCP '" + info.name + "' 不可用：" + client.error());
    auto tools = (*client)->list_tools();
    if (!tools) return std::unexpected("MCP '" + info.name + "' 的 tools/list 失败：" + tools.error());
    for (McpToolInfo const& t : *tools)
      pending.push_back(Pending{*client, t});
  }

  // 第二阶段：一次性注册（make_mcp_tool 无失败路径，此阶段不再失败）。
  // 注意顺序：先记全名、再 move 进 registry（move 后 spec.name 为空串）。
  McpProviderResult out;
  out.servers = strip_meta(*servers);
  for (Pending const& p : pending) {
    std::string const prefix = sanitize_part(p.client->server_name());
    std::string const tname = sanitize_part(p.tool.name);
    auto spec = make_mcp_tool(*p.client, p.tool, /*read_only=*/true);
    spec.name = prefix + "_" + tname;
    out.tools.push_back(spec.name);
    registry.add(std::move(spec));
  }
  return out;
}

}  // namespace cppchain::mcp