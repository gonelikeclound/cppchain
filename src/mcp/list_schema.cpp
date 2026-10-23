// cppchain —— mcp/list_schema.cpp
// 统一 MCP 服务发现列表解析。三路（so:// / file:// / url://）都产同一 JSON 结构，
// 这里把它们收口成同一种解析（坏格式 fail-closed）。
#include "cppchain/mcp/list_schema.hpp"

namespace cppchain::mcp {

using Json = nlohmann::json;

std::expected<std::vector<McpServerInfo>, std::string> parse_mcp_server_list(Json const& j) {
  if (!j.is_object() || !j.contains("mcpServers") || !j["mcpServers"].is_array())
    return std::unexpected("MCP 列表缺 mcpServers 数组（三路发现源格式必须一致）");
  std::vector<McpServerInfo> out;
  for (Json const& s : j["mcpServers"]) {
    if (!s.is_object()) return std::unexpected("MCP 列表项非对象");
    McpServerInfo info;
    info.name = s.value("name", "");
    info.type = s.value("type", "");
    info.url = s.value("url", "");
    info.transport = s.value("transport", "http");
    info.description = s.value("description", "");
    if (s.contains("meta") && s["meta"].is_object()) info.meta = s["meta"];

    // 字段校验（fail-closed）：name/type 必填；type 词表；remote 必带 url。
    if (info.name.empty()) return std::unexpected("MCP 列表项缺 name");
    if (info.type != "local" && info.type != "remote")
      return std::unexpected("MCP 列表项 '" + info.name + "' 的 type='" + info.type +
                             "' 不在词表里（local/remote）");
    if (info.type == "remote" && info.url.empty())
      return std::unexpected("MCP 列表项 '" + info.name + "'（remote）缺 url");
    if (info.transport != "http" && info.transport != "sse")
      return std::unexpected("MCP 列表项 '" + info.name + "' 的 transport='" + info.transport +
                             "' 不在词表里（http/sse）");
    out.push_back(std::move(info));
  }
  return out;
}

nlohmann::json strip_meta(std::vector<McpServerInfo> const& servers) {
  Json out = Json::array();
  for (McpServerInfo const& s : servers) {
    Json item = Json{{"name", s.name}, {"type", s.type},
                     {"transport", s.transport}};
    if (!s.url.empty()) item["url"] = s.url;
    if (!s.description.empty()) item["description"] = s.description;
    // 刻意不带 meta —— 凭证只活在连接管理器/MCPClient，不进 workflow 状态。
    out.push_back(std::move(item));
  }
  return out;
}

}  // namespace cppchain::mcp