// MCP 二期测试：三路发现（so:// file:// url://）+ 统一列表 schema + 连接管理器 + provider 节点。
// 纪律：发现源三路格式一致（坏格式 fail-closed）；meta/token 只在注入 client 时用，发布面
// （strip_meta）无凭证；provider 两段式 —— 任一 server 失败 → 整体失败、registry 零改动。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/mcp/list_schema.hpp"
#include "cppchain/mcp/manager.hpp"
#include "cppchain/mcp/provider.hpp"

using namespace cppchain;
using namespace cppchain::http;
using namespace cppchain::mcp;
using cppchain::tool::ToolRegistry;
using cppchain::tool::ToolSpec;
using Json = nlohmann::json;

namespace {

// 一次性临时目录（file:// 发现源测试用）；每次写前重建内容，测试进程内复用同一路径。
std::filesystem::path test_dir() {
  static std::filesystem::path const p =
      std::filesystem::temp_directory_path() / "cppchain_mcp_p2_test";
  return p;
}
std::filesystem::path write_list_file(std::string const& content) {
  std::filesystem::create_directories(test_dir());
  auto const f = test_dir() / "servers.json";
  std::ofstream out(f);
  out << content;
  return f;
}

// 一个三路发现的完整测试架：FakeTransport（模拟 MCP server / url:// 源）+ manager。
struct McpP2Harness {
  FakeTransport tx;
  MCPConnectionManager mgr;
  explicit McpP2Harness() : mgr(tx, /*so_dirs=*/{MCP_TEST_SO_DIR}) {}
};

// ---- FakeTransport::Script 构造：字段显式给全，零告警（-Wmissing-field-initializers）----
FakeTransport::Script script_ok(Json body) {
  return FakeTransport::Script{.status = 200, .chunks = {body.dump()}, .error = {},
                               .timeout = false, .delay_ms = 0};
}
FakeTransport::Script script_ok_raw(std::string raw) {
  return FakeTransport::Script{.status = 200, .chunks = {std::move(raw)}, .error = {},
                               .timeout = false, .delay_ms = 0};
}
FakeTransport::Script script_fail(std::string message) {
  return FakeTransport::Script{.status = 0, .chunks = {}, .error = std::move(message),
                               .timeout = false, .delay_ms = 0};
}

// —— MCP JSON-RPC 响应构造（沿用 P1 mcp_client_test 的形状）——
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
Json ok_tools_list(std::vector<Json> tools) { return rpc_result(Json{{"tools", std::move(tools)}}); }
Json tool_spec(std::string name, std::string desc) {
  return Json{{"name", std::move(name)},
              {"description", std::move(desc)},
              {"inputSchema",
               Json{{"type", "object"},
                    {"properties", Json{{"q", Json{{"type", "string"}}}}}}}};
}

McpServerInfo server_info(std::string name, std::string url,
                          Json meta = Json::object()) {
  return McpServerInfo{.name = std::move(name),
                       .type = "remote",
                       .url = std::move(url),
                       .transport = "http",
                       .meta = std::move(meta),
                       .description = {}};
}

// 便利断言：某次请求里出现指定前缀响应头。
bool has_header(HttpRequest const& req, std::string_view prefix) {
  for (std::string const& h : req.headers)
    if (h.starts_with(prefix)) return true;
  return false;
}

}  // namespace

// ================= A. 统一列表 schema（parse_mcp_server_list / strip_meta） =================

// 完整列表轮一遍：本地+远端、transport、meta、description 都解析对；meta 在 parse 层**保留**
//（剥凭证是 strip_meta 的事）。
TEST(McpP2ListSchema, parse_full_list_preserves_meta) {
  Json j = Json{{"mcpServers", Json::array({
       Json{{"name", "weather"},
            {"type", "remote"},
            {"url", "http://mcp.test/rpc"},
            {"transport", "http"},
            {"meta", Json{{"token", "t1"}, {"x-api-key", "k2"}}},
            {"description", "天气服务"}},
       Json{{"name", "local-bot"}, {"type", "local"}, {"transport", "sse"}},
  })}};
  auto r = parse_mcp_server_list(j);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 2u);
  EXPECT_EQ((*r)[0].name, "weather");
  EXPECT_EQ((*r)[0].type, "remote");
  EXPECT_EQ((*r)[0].url, "http://mcp.test/rpc");
  EXPECT_EQ((*r)[0].transport, "http");
  EXPECT_EQ((*r)[0].meta["token"], "t1");          // parse 层不剥
  EXPECT_EQ((*r)[0].description, "天气服务");
  EXPECT_EQ((*r)[1].name, "local-bot");
  EXPECT_EQ((*r)[1].type, "local");                // local 无 url 合法
  EXPECT_EQ((*r)[1].meta, Json::object());          // 缺省空对象
}

// 坏格式 fail-closed：缺 mcpServers。
TEST(McpP2ListSchema, parse_missing_mcp_servers_fails_closed) {
  auto r = parse_mcp_server_list(Json{{"servers", Json::array()}});
  EXPECT_FALSE(r.has_value());
}
// 坏格式 fail-closed：项缺 name。
TEST(McpP2ListSchema, parse_missing_name_fails_closed) {
  auto r = parse_mcp_server_list(
      Json{{"mcpServers", Json::array({Json{{"type", "remote"}, {"url", "http://x"}}})}});
  EXPECT_FALSE(r.has_value());
}
// 坏格式 fail-closed：type 不在词表。
TEST(McpP2ListSchema, parse_bad_type_fails_closed) {
  auto r = parse_mcp_server_list(
      Json{{"mcpServers", Json::array({Json{{"name", "w"}, {"type", "carrier"}}})}});
  EXPECT_FALSE(r.has_value());
}
// 坏格式 fail-closed：remote 缺 url。
TEST(McpP2ListSchema, parse_remote_missing_url_fails_closed) {
  auto r = parse_mcp_server_list(
      Json{{"mcpServers", Json::array({Json{{"name", "w"}, {"type", "remote"}}})}});
  EXPECT_FALSE(r.has_value());
}
// 坏格式 fail-closed：transport 不在词表。
TEST(McpP2ListSchema, parse_bad_transport_fails_closed) {
  auto r = parse_mcp_server_list(
      Json{{"mcpServers", Json::array({Json{{"name", "w"},
                                            {"type", "remote"},
                                            {"url", "http://x"},
                                            {"transport", "websocket"}}})}});
  EXPECT_FALSE(r.has_value());
}

// strip_meta：凭证剥干净（token/x-api-key 都不在），url/description 保留。
TEST(McpP2ListSchema, strip_meta_removes_credentials) {
  std::vector<McpServerInfo> servers;
  servers.push_back(
      McpServerInfo{.name = "weather", .type = "remote", .url = "http://mcp.test/rpc",
                    .transport = "http", .meta = Json{{"token", "t1"}, {"x-api-key", "k"}},
                    .description = "天气"});
  servers.push_back(McpServerInfo{.name = "local-bot", .type = "local", .url = "",
                                  .transport = "sse", .description = {}});

  Json out = strip_meta(servers);
  ASSERT_TRUE(out.is_array());
  ASSERT_EQ(out.size(), 2u);
  EXPECT_FALSE(out[0].contains("meta"));
  EXPECT_EQ(out[0]["name"], "weather");
  EXPECT_EQ(out[0]["url"], "http://mcp.test/rpc");      // 地址非凭证，保留
  EXPECT_EQ(out[0]["description"], "天气");
  EXPECT_EQ(out[1]["name"], "local-bot");
  EXPECT_FALSE(out[1].contains("url"));                 // 空 url 不发布
}

// ================= B. 连接管理器三路发现 =================

TEST(McpP2Manager, discover_file_happy) {
  McpP2Harness h;
  auto const f =
      write_list_file(R"({"mcpServers":[{"name":"weather","type":"remote","url":"http://mcp.test/rpc","transport":"http","meta":{"token":"file-token"}}]})");
  auto r = h.mgr.discover("file://" + f.string());
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 1u);
  EXPECT_EQ((*r)[0].name, "weather");
  EXPECT_EQ((*r)[0].meta["token"], "file-token");       // 发现层保留，供 client_for 注入
}

TEST(McpP2Manager, discover_file_missing_fails_closed) {
  McpP2Harness h;
  auto r = h.mgr.discover("file:///nonexistent/mcp_servers.json");
  EXPECT_FALSE(r.has_value());
}

TEST(McpP2Manager, discover_file_bad_json_fails_closed) {
  McpP2Harness h;
  auto const f = write_list_file("this is not json");
  auto r = h.mgr.discover("file://" + f.string());
  EXPECT_FALSE(r.has_value());
}

// 合法 JSON 但结构错 → 汇合到同一 schema 校验，同样 fail-closed。
TEST(McpP2Manager, discover_file_bad_schema_fails_closed) {
  McpP2Harness h;
  auto const f = write_list_file(R"({"servers":[]})");
  auto r = h.mgr.discover("file://" + f.string());
  EXPECT_FALSE(r.has_value());
}

TEST(McpP2Manager, discover_url_happy_uses_get) {
  McpP2Harness h;
  h.tx.push(script_ok_raw(
      R"({"mcpServers":[{"name":"remote-w","type":"remote","url":"http://mcp.test/rpc"}]})"));
  auto r = h.mgr.discover("url://http://mcp.test/list.json");
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 1u);
  EXPECT_EQ((*r)[0].name, "remote-w");
  ASSERT_FALSE(h.tx.requests().empty());
  EXPECT_EQ(h.tx.requests()[0].method, "GET");          // 发现拉取是 GET
  EXPECT_EQ(h.tx.requests()[0].url, "http://mcp.test/list.json");
}

TEST(McpP2Manager, discover_url_transport_fail_fails_closed) {
  McpP2Harness h;
  h.tx.push(script_fail("connection refused"));
  auto r = h.mgr.discover("url://http://mcp.test/list.json");
  EXPECT_FALSE(r.has_value());
}

TEST(McpP2Manager, discover_url_bad_json_fails_closed) {
  McpP2Harness h;
  h.tx.push(script_ok_raw("<html>not json</html>"));
  auto r = h.mgr.discover("url://http://mcp.test/list.json");
  EXPECT_FALSE(r.has_value());
}

TEST(McpP2Manager, discover_unknown_scheme_fails_closed) {
  McpP2Harness h;
  auto r = h.mgr.discover("ftp://host/list.json");
  EXPECT_FALSE(r.has_value());
}

TEST(McpP2Manager, discover_so_happy_dlopen_real_plugin) {
  McpP2Harness h;
  auto r = h.mgr.discover("so://mcp_list_plugin:weather");
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 1u);
  EXPECT_EQ((*r)[0].name, "weather");
  EXPECT_EQ((*r)[0].meta["token"], "so-token");         // 插件列表带的凭证进发现结果
  EXPECT_EQ((*r)[0].description, "天气");
}

TEST(McpP2Manager, discover_so_empty_list_ok) {
  McpP2Harness h;
  auto r = h.mgr.discover("so://mcp_list_plugin:empty");
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->empty());
}

TEST(McpP2Manager, discover_so_missing_lib_fails_closed) {
  McpP2Harness h;
  auto r = h.mgr.discover("so://no_such_lib_xyz:weather");
  EXPECT_FALSE(r.has_value());
}

TEST(McpP2Manager, discover_so_missing_symbol_fails_closed) {
  McpP2Harness h;
  auto r = h.mgr.discover("so://mcp_list_plugin:no_such_fn");
  EXPECT_FALSE(r.has_value());
}

TEST(McpP2Manager, discover_so_bad_format_fails_closed) {
  McpP2Harness h;
  auto r = h.mgr.discover("so://only_lib_no_fn");
  EXPECT_FALSE(r.has_value());
}

// ================= C. 连接管理器：懒建 + 握手 + 复用 =================

// 首次 client_for 建连握手；二次同信息直接复用同一 client；凭据进请求头。
TEST(McpP2Manager, client_for_lazy_build_reuse_and_auth_headers) {
  McpP2Harness h;
  h.tx.push(script_ok(ok_initialize()));
  McpServerInfo const info =
      server_info("weather", "http://mcp.test/rpc", Json{{"token", "t1"}});

  EXPECT_EQ(h.mgr.connected(), 0u);
  auto c1 = h.mgr.client_for(info);
  ASSERT_TRUE(c1.has_value());
  EXPECT_EQ(h.mgr.connected(), 1u);

  auto c2 = h.mgr.client_for(info);                     // 复用，不重建
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(*c1, *c2);

  EXPECT_EQ(h.mgr.server_names(), std::vector<std::string>{"weather"});
  ASSERT_FALSE(h.tx.requests().empty());
  EXPECT_TRUE(has_header(h.tx.requests()[0], "MCP-Protocol-Version:"));
  EXPECT_TRUE(has_header(h.tx.requests()[0], "Authorization: Bearer t1"));
}

// 握手失败：池不保留坏连接，再调重新握手；失败整体报错。
TEST(McpP2Manager, client_for_handshake_failure_not_pooled) {
  McpP2Harness h;
  h.tx.push(script_ok(rpc_error(-32002, "denied")));
  McpServerInfo const info = server_info("weather", "http://mcp.test/rpc");

  auto c1 = h.mgr.client_for(info);
  EXPECT_FALSE(c1.has_value());
  EXPECT_EQ(h.mgr.connected(), 0u);

  // 坏连接不留在池里 → 第二次调用会重新走一遍握手（请求数 2）。
  auto c2 = h.mgr.client_for(info);
  EXPECT_FALSE(c2.has_value());
  EXPECT_EQ(h.tx.requests().size(), 2u);
}

// ================= D. provider 节点：发现 → 建连 → 注册进 tool_registry =================

// 一个 server、两个工具：注册带 server 前缀的全名；发布面 strip_meta（无 token）；handler 可用。
TEST(McpP2Provider, registers_tools_with_server_prefix) {
  McpP2Harness h;
  h.tx.push(script_ok(ok_initialize()));
  h.tx.push(script_ok(ok_tools_list({tool_spec("get_weather", "查天气"),
                                     tool_spec("get_forecast", "看预报")})));
  auto const f = write_list_file(
      R"({"mcpServers":[{"name":"weather","type":"remote","url":"http://mcp.test/rpc","meta":{"token":"t1"}}]})");

  ToolRegistry reg;
  auto r = mcp_provider(h.mgr, "file://" + f.string(), reg);
  ASSERT_TRUE(r.has_value());

  // 发布面：servers 列表无 meta/token，tools 全名按 server 前缀隔开。
  ASSERT_TRUE(r->servers.is_array());
  ASSERT_EQ(r->servers.size(), 1u);
  EXPECT_FALSE(r->servers[0].contains("meta"));
  EXPECT_EQ(r->servers[0].dump().find("t1"), std::string::npos);
  EXPECT_EQ(r->tools, (std::vector<std::string>{"weather_get_weather", "weather_get_forecast"}));

  // 注册表：两个都进，全名带前缀；handler 走 MCP client（脚本里已备 text）。
  ASSERT_EQ(reg.size(), 2u);
  ToolSpec const* s1 = reg.find("weather_get_weather");
  ASSERT_NE(s1, nullptr);
  EXPECT_EQ(s1->description, "查天气");
  EXPECT_TRUE(s1->parameters.is_object());              // inputSchema 直用
  // 调用 handler → MCP tools/call → 返回 content 文本（第三个脚本）。
  h.tx.push(script_ok(rpc_result(Json{{"content",
                                       Json::array({Json{{"type", "text"}, {"text", "晴"}}})}})));
  tool::ToolContext ctx;
  auto res = s1->handler(Json{{"q", "北京"}}, ctx);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ((*res)["text"], "晴");
}

// 两段式：第二个 server 握手失败 → mcp_provider 整体失败，registry 零改动。
TEST(McpP2Provider, second_server_failure_registers_nothing) {
  McpP2Harness h;
  // server A：握手 + tools/list 都好。
  h.tx.push(script_ok(ok_initialize()));
  h.tx.push(script_ok(ok_tools_list({tool_spec("get_weather", "查天气")})));
  // server B：握手失败。
  h.tx.push(script_ok(rpc_error(-32002, "denied")));
  auto const f = write_list_file(
      R"({"mcpServers":[{"name":"a","type":"remote","url":"http://mcp.test/a"},"
      R"({"name":"b","type":"remote","url":"http://mcp.test/b"}]})");

  ToolRegistry reg;
  auto r = mcp_provider(h.mgr, "file://" + f.string(), reg);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(reg.size(), 0u);                            // 第一阶段失败 → 零注册
}

// 工具名清洗：server/tool 名里的怪字符换成 '_'，与 OpenAI 函数名语料对齐。
TEST(McpP2Provider, tool_name_sanitized) {
  McpP2Harness h;
  h.tx.push(script_ok(ok_initialize()));
  h.tx.push(script_ok(ok_tools_list({tool_spec("we ther.api", "奇名工具")})));
  auto const f = write_list_file(
      R"({"mcpServers":[{"name":"wea ther","type":"remote","url":"http://mcp.test/rpc"}]})");

  ToolRegistry reg;
  auto r = mcp_provider(h.mgr, "file://" + f.string(), reg);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(reg.size(), 1u);
  EXPECT_NE(reg.find("wea_ther_we_ther_api"), nullptr);  // 空格/点 → '_'，下划线/横杠保留
}