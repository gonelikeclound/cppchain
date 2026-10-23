// cppchain —— mcp/manager.hpp
// MCP 连接管理器：**cppchain 内部统一持有/管理 MCP 连接状态**（协议要求客户端/服务端各管
// 各的状态机，这里做客户端那半边）。
//   · server 池：name → MCPClient（懒建、复用，握手后才可用）
//   · 发现（provider）：`discover(from)` 解析三条发现路线，返回统一列表
//       so://<so-name>:<mcp_list_fn>   —— 从 .so dlsym `mcp_list_<fn>`（返回统一 JSON 字符串）
//       file://<path>                  —— 读本地 JSON 文件
//       url://<url>                    —— HTTP 拉取 JSON（复用 http_json）
//   · 凭证纪律：meta（token）只在注入 MCPClient 时用；`strip_meta` 列表供 provider 发布进
//     results/state（不含 token）。
// 依赖纪律同 MCPClient：只依赖 nlohmann + http::Transport 接口。
#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cppchain/http/transport.hpp"
#include "cppchain/mcp/client.hpp"
#include "cppchain/mcp/list_schema.hpp"

namespace cppchain::mcp {

class MCPConnectionManager {
 public:
  // transport 非拥有（宿主注入，同 web_search 纪律）。so_dirs 是 so:// 路线的库搜索目录
  //（仿 tool_lib_dirs）。
  MCPConnectionManager(http::Transport& transport, std::vector<std::string> so_dirs = {})
      : transport_(&transport), so_dirs_(std::move(so_dirs)) {}

  // 解析发现源 from（so:// / file:// / url://）→ 统一 MCP 列表。
  // 只发现不建连；token 剥掉返回（provider 发布安全面）。
  [[nodiscard]] std::expected<std::vector<McpServerInfo>, std::string> discover(
      std::string_view from);

  // 取/建某个 server 的 client（懒建 + initialize 握手）。失败 → 错误。
  [[nodiscard]] std::expected<MCPClient*, std::string> client_for(McpServerInfo const& info);

  // 已建立的连接数（供观察）。
  [[nodiscard]] std::size_t connected() const { return clients_.size(); }

  // 池内全部 server 名（已发现过的）。
  [[nodiscard]] std::vector<std::string> server_names() const;

 private:
  http::Transport* transport_;
  std::vector<std::string> so_dirs_;
  std::unordered_map<std::string, std::unique_ptr<MCPClient>> clients_;
};

}  // namespace cppchain::mcp