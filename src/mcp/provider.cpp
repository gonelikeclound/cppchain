// cppchain —— mcp/provider.cpp
// MCP provider 节点实现：两段式装配 + P3 批②按需变化监听（见 provider.hpp 头注释）。
#include "cppchain/mcp/provider.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cppchain::mcp {

using Json = nlohmann::json;

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

// P3 批②：收到 tools/list_changed 通知 → 重拉该 server 的 tools → **整体重建其前缀注册**
//（删旧加新，一个语义覆盖 新增/变更/删除；重拉失败则保留旧注册、等下一个通知再试）。
// **自由函数，不捕获任何调用环境** —— 会被 Long-lived 的 on_change 闭包按住很久，
// 引用 mcp_provider 栈上局部量必悬垂（实测 SEGV）。只接受持久对象指针/值。
void refresh_server_registry(tool::ToolRegistry& registry, MCPClient& client,
                              std::string const& prefix) {
  auto tools = client.list_tools();
  if (!tools) return;
  std::string const pref = prefix + "_";
  std::vector<std::string> own;
  for (tool::ToolSpec const& t : registry.all())
    if (t.name.starts_with(pref)) own.push_back(t.name);
  for (std::string const& n : own) registry.remove(n);
  for (McpToolInfo const& t : *tools) {
    auto spec = make_mcp_tool(client, t, /*read_only=*/true);
    spec.name = pref + sanitize_part(t.name);
    registry.add(std::move(spec));
  }
}

}  // namespace

std::expected<McpProviderResult, std::string> mcp_provider(MCPConnectionManager& mgr,
                                                           std::string_view from,
                                                           tool::ToolRegistry& registry,
                                                           McpProviderOptions const& opts) {
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

  // —— P3 批②：按需变化监听（默认空 = 零长连接；watch 点名才尝试）——
  // on_change 闭包**只按值捕获持久指针**（reg/c 指向 mcp_provider 之外、由宿主保活的对象），
  // 绝不引用本函数栈上局部量 —— 该闭包会被 McpListener 按住远超本函数寿命。
  for (std::string const& w : opts.watch) {
    McpServerInfo const* info = nullptr;
    for (McpServerInfo const& s : *servers)
      if (s.name == w) { info = &s; break; }
    if (info == nullptr)
      return std::unexpected("watch 点名 '" + w + "' 不在发现的 mcpServers 列表里");
    auto client = mgr.client_for(*info);  // 池内已建 → 秒回，不再握手
    if (!client)
      return std::unexpected("MCP '" + w + "' 不可用（无法挂变化监听）：" + client.error());

    // 能力门：server 未声明 tools.listChanged → 不挂流（挂上也没推送），留 error 观察位。
    Json const& caps = (*client)->capabilities();
    bool const list_changed =
        caps.contains("tools") && caps["tools"].is_object() &&
        caps["tools"].contains("listChanged") && caps["tools"]["listChanged"].is_boolean() &&
        caps["tools"]["listChanged"].get<bool>();

    MCPClient* const c = *client;
    std::string const prefix = sanitize_part(info->name);
    if (list_changed) {
      std::function<void()> on_change = [reg = &registry, c, prefix] {
        refresh_server_registry(*reg, *c, prefix);
      };
      out.listeners.push_back(
          std::make_unique<McpListener>(*c, std::move(on_change)));
    } else {
      out.listeners.push_back(std::make_unique<McpListener>(
          *c, std::function<void()>{},
          "该 server 未声明 tools.listChanged，跳过变化监听"));
    }
  }

  return out;
}

}  // namespace cppchain::mcp