// cppchain —— examples/mcp_provider/main.cpp
// MCP 二期离线 demo（P2：三路发现 + 连接管理器 + provider 节点）。
// 全程无网络：MCP server 用 ScriptedMcpServer 内嵌模拟（按 JSON-RPC method 应答），
// 发现列表用 tmp/ 下的 file:// 源 + so:// 测试插件（MCP_DEMO_SO_DIR 注入）。
// 看点：
//   1. 三条发现路线产**同一**列表 schema；meta(token) 只在连 server 时注入；
//   2. provider 把 server 暴露的工具注册进 ToolRegistry（全名 server_tool，前缀隔开）；
//   3. 发布面（results/state 可进）经 strip_meta —— token 不外泄；
//   4. 注册后的工具 handler 直接调 MCP server，返回 content 文本。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cppchain/http/transport.hpp"
#include "cppchain/mcp/manager.hpp"
#include "cppchain/mcp/provider.hpp"
#include "cppchain/tool/registry.hpp"

using namespace cppchain;
using namespace cppchain::http;
using namespace cppchain::mcp;
using cppchain::tool::ToolContext;
using cppchain::tool::ToolRegistry;
using cppchain::tool::ToolSpec;
using Json = nlohmann::json;

namespace {

// 内嵌假 MCP server：按 JSON-RPC method 应答（initialize / tools/list / tools/call）。
// 响应内容固定（确定性），每次请求推进工具调用语义。实现 http::Transport 接口即可，
// 与真实 server 的差别只剩"内容是写死的"。
struct ScriptedMcpServer final : http::Transport {
  std::vector<std::string> request_log;  // 每个请求 body（展示用）
  std::vector<std::vector<std::string>> header_log;  // 每个请求 headers（凭证核对用）

  void send(HttpRequest const& req, HttpCallbacks cbs, core::CancelToken /*token*/) override {
    request_log.push_back(req.body);
    header_log.push_back(req.headers);

    // 发现列表端点 GET（空/non-JSON body）→ 返回同一份 mcpServers 列表（url:// 路线的"同上列表"）。
    static std::string const kDiscoveryList =
        R"({"mcpServers":[{"name":"weather","type":"remote","url":"http://mcp.test/rpc",)"
        R"("transport":"http","meta":{"token":"demo-token"},"description":"演示天气服务"}]})";

    Json j = Json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    long const id = (j.is_object() && j.contains("id")) ? j["id"].get<long>() : 1;
    std::string resp;

    std::string method;
    if (j.is_discarded() || !j.is_object()) {
      resp = kDiscoveryList;                              // GET 发现列表
    } else {
      method = j.value("method", "");
      if (method == "initialize") {
        resp = (Json{{"jsonrpc", "2.0"}, {"id", id},
                     {"result", Json{{"protocolVersion", "2025-03-26"},
                                     {"capabilities", Json{{"tools", Json::object()}}}}}})
                   .dump();
      } else if (method == "tools/list") {
        // 两个演示工具：查天气 / 看预报。
        std::vector<Json> tools;
        tools.push_back(Json{{"name", "get_weather"},
                             {"description", "查当前天气"},
                             {"inputSchema",
                              Json{{"type", "object"},
                                   {"properties", Json{{"city", Json{{"type", "string"}}}}}}}});
        tools.push_back(Json{{"name", "get_forecast"},
                             {"description", "看未来数日预报"},
                             {"inputSchema",
                              Json{{"type", "object"},
                                   {"properties", Json{{"city", Json{{"type", "string"}}}}}}}});
        resp = (Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json{{"tools", tools}}}}).dump();
      } else if (method == "tools/call") {
        std::string const name = j["params"]["name"];
        std::string const city =
            j["params"]["arguments"].value("city", "未知");
        std::string const text =
            (name == "get_weather")   ? ("[" + city + "] 晴，23°C，微风")
            : (name == "get_forecast") ? ("[" + city + "] 明日起三天小雨转多云")
                                       : ("未知工具 " + name);
        resp = (Json{{"jsonrpc", "2.0"}, {"id", id},
                     {"result", Json{{"content",
                                      Json::array({Json{{"type", "text"}, {"text", text}}})}}}})
                   .dump();
      } else {
        resp = (Json{{"jsonrpc", "2.0"}, {"id", id},
                     {"error", Json{{"code", -32601}, {"message", "method not found"}}}})
                   .dump();
      }
    }
    if (cbs.on_status) cbs.on_status(200);
    if (cbs.on_body_chunk) cbs.on_body_chunk(resp);
    if (cbs.on_complete) cbs.on_complete();
  }
};

// 写一个 file:// 发现源（含 token 凭证），返回其路径。落 tmp/，跟着仓库走。
std::string write_list_file() {
  std::filesystem::create_directories("tmp");
  std::string const p = "tmp/mcp_servers.json";
  std::ofstream out(p);
  out << R"({"mcpServers":[{"name":"weather","type":"remote","url":"http://mcp.test/rpc",)"
         R"("transport":"http","meta":{"token":"demo-token"},"description":"演示天气服务"}]})";
  return p;
}

void show_discovery(MCPConnectionManager& mgr, std::string_view from,
                    std::string_view title) {
  std::printf("\n[%s] %s\n", std::string(title).c_str(), std::string(from).c_str());
  auto servers = mgr.discover(from);
  if (!servers) {
    std::printf("  发现失败：%s\n", servers.error().c_str());
    return;
  }
  for (McpServerInfo const& s : *servers)
    std::printf("  + %-10s type=%s transport=%s url=%s  meta=%s\n", s.name.c_str(),
                s.type.c_str(), s.transport.c_str(), s.url.c_str(), s.meta.dump().c_str());
}

}  // namespace

int main() {
  ScriptedMcpServer mcp_server;                                // 假 MCP server（离线）
  MCPConnectionManager mgr(mcp_server, /*so_dirs=*/{MCP_DEMO_SO_DIR});

  std::printf("== MCP 二期：三路发现 → provider 注册 → 调用（全部离线）==\n");

  // 1. 三条发现路线：产同一列表 schema；meta 在发现层保留（供连 server 注入）。
  show_discovery(mgr, "file://" + write_list_file(), "file:// 发现源");
  show_discovery(mgr, "so://mcp_weather_list:weather", "so:// 发现源");
  show_discovery(mgr, "url://http://mcp.test/list.json", "url:// 发现源（同上列表）");

  // 2. provider：从 file:// 源装配进 ToolRegistry（用 ScriptedMcpServer 做握手/list/call）。
  std::printf("\n== provider 节点：file:// 源 → tool_registry ==\n");
  ToolRegistry registry;
  auto pr = mcp_provider(mgr, "file://" + write_list_file(), registry);
  if (!pr) {
    std::printf("  provider 失败：%s\n", pr.error().c_str());
    return 1;
  }

  std::printf("  发布面 servers（strip_meta，token 已剥）：\n");
  std::printf("    %s\n", pr->servers.dump(2).c_str());
  std::printf("  注册工具：");
  for (size_t i = 0; i < pr->tools.size(); ++i)
    std::printf("%s%s", i ? ", " : "", pr->tools[i].c_str());
  std::printf("\n");

  // 3. 直接调注册好的工具 handler —— 走到 MCP server 的 tools/call。
  std::printf("\n== 调用注册工具（handler → MCP tools/call）==\n");
  tool::ToolContext ctx;
  if (ToolSpec const* s = registry.find("weather_get_weather")) {
    auto r = s->handler(Json{{"city", "上海"}}, ctx);
    if (r) std::printf("  weather_get_weather(上海) → %s\n", r->dump().c_str());
  }
  if (ToolSpec const* s = registry.find("weather_get_forecast")) {
    auto r = s->handler(Json{{"city", "杭州"}}, ctx);
    if (r) std::printf("  weather_get_forecast(杭州) → %s\n", r->dump().c_str());
  }

  std::printf("\n== 凭证纪律核对 ==\n");
  bool saw_bearer = false;
  for (auto const& hs : mcp_server.header_log)
    for (std::string const& h : hs)
      if (h.starts_with("Authorization: Bearer demo-token")) saw_bearer = true;
  std::printf("  initialize 请求头注入 Bearer demo-token：%s\n",
             saw_bearer ? "确认（meta 只活在客户端/连接层）" : "未注入!?");
  std::printf("  发布面里搜不到 \"demo-token\"：%s\n",
             pr->servers.dump().find("demo-token") == std::string::npos ? "确认" : "泄漏!!");

  std::printf("\n完成。\n");
  return 0;
}