// MCP 三期批②测试：变化监听（长连接收 notifications/tools/list_changed → 重拉 → 重建注册）。
// 链路：FakeTransport 开流（同步投递，确定序）→ MCPClient::listen → provider watch 装配 →
//   收到 list_changed → refresh_server 整体重建该 server 前缀注册（删旧加新覆盖 增/改/删）。
// 能力门：server 未声明 tools.listChanged → 不挂流、留 error 观察位。
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/mcp/listen.hpp"
#include "cppchain/mcp/provider.hpp"
#include "cppchain/tool/registry.hpp"

using namespace cppchain;
using namespace cppchain::http;
using namespace cppchain::mcp;
using cppchain::tool::ToolRegistry;
using cppchain::tool::ToolSpec;
using Json = nlohmann::json;

namespace {

// 只实现 Transport、**不**提供 StreamProvider 的包装：测"传输不支持长连接"的 fail-closed。
struct PlainTransport final : http::Transport {
  http::Transport& inner;
  explicit PlainTransport(http::Transport& t) : inner(t) {}
  void send(http::HttpRequest const& req, http::HttpCallbacks cbs,
            core::CancelToken token) override {
    inner.send(req, std::move(cbs), std::move(token));
  }
};

// 轮询等 predicate 为真（McpListener 的 on_change 在 worker 线程异步跑，断言前要靠它收敛）。
template <typename F>
bool wait_until(F&& pred, int timeout_ms = 2000) {
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

// —— 测试基建（沿用 mcp_discover_test 的形态）——
FakeTransport::Script script_ok(Json body) {
  return FakeTransport::Script{.status = 200, .chunks = {body.dump()}, .headers = {},
                               .error = {}, .timeout = false, .delay_ms = 0};
}
FakeTransport::Script script_with_session(Json body, std::string const& sid) {
  return FakeTransport::Script{.status = 200, .chunks = {body.dump()},
                               .headers = {"mcp-session-id: " + sid}, .error = {},
                               .timeout = false, .delay_ms = 0};
}

std::filesystem::path write_list_file(std::string const& content) {
  auto const p = std::filesystem::temp_directory_path() / "cppchain_mcp_p3_test_servers.json";
  std::ofstream out(p, std::ios::binary);
  out << content;
  return p;
}

Json rpc_result(Json result) {
  return Json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", std::move(result)}};
}
Json ok_initialize(bool list_changed) {
  Json tools_cap = Json::object();
  if (list_changed) tools_cap["listChanged"] = true;
  return rpc_result(Json{{"protocolVersion", "2025-03-26"},
                         {"capabilities", Json{{"tools", tools_cap}}}});
}
Json ok_tools_list(std::vector<Json> tools) { return rpc_result(Json{{"tools", std::move(tools)}}); }
Json tool_spec(std::string name, std::string desc) {
  return Json{{"name", std::move(name)},
              {"description", std::move(desc)},
              {"inputSchema",
               Json{{"type", "object"},
                    {"properties", Json{{"q", Json{{"type", "string"}}}}}}}};
}

// notifications/tools/list_changed 的 SSE 帧（真实 MCP 格式）。
std::string list_changed_sse_frame() {
  return "event: message\n"
         "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\",\"params\":{}}\n\n";
}

// 一个 server（weather）的发现列表；initialize 会带会话头 + listChanged 能力。
std::string weather_list() {
  return R"({"mcpServers":[{"name":"weather","type":"remote","url":"http://mcp.test/rpc"}]})";
}

struct McpP3Harness {
  FakeTransport tx;
  MCPConnectionManager mgr;
  McpP3Harness() : mgr(tx) {}
};

}  // namespace

// ================= A. ToolRegistry::remove（批②新 API）=================

TEST(McpP3Registry, remove_erases_and_reindexes) {
  ToolRegistry reg;
  auto make = [](std::string name) {
    ToolSpec t;
    t.name = std::move(name);
    return t;
  };
  reg.add(make("a"));
  reg.add(make("b"));
  reg.add(make("c"));

  reg.remove("b");  // 中段擦除 → 索引必须重排
  ASSERT_EQ(reg.size(), 2u);
  EXPECT_NE(reg.find("a"), nullptr);
  EXPECT_EQ(reg.find("b"), nullptr);
  ASSERT_NE(reg.find("c"), nullptr);
  EXPECT_EQ(reg.all()[0].name, "a");
  EXPECT_EQ(reg.all()[1].name, "c");  // c 的下标从 2 → 1，查到即为证

  reg.remove("c");
  reg.remove("no_such");  // 缺名无害
  ASSERT_EQ(reg.size(), 1u);
  EXPECT_NE(reg.find("a"), nullptr);
  reg.remove("a");
  EXPECT_TRUE(reg.empty());
}

// ================= B. FakeTransport 流契约（传输层）=================

TEST(McpP3Stream, sync_delivery_and_close_order) {
  FakeTransport tx;
  std::string got;
  int close_calls = 0, complete_calls = 0;
  StreamCallbacks cbs;
  cbs.on_body_chunk = [&](std::string_view b) { got.append(b); };
  cbs.on_complete = [&] { ++complete_calls; };
  cbs.on_close = [&] { ++close_calls; };
  tx.open_stream(StreamRequest{.url = "http://mcp.test/notify", .method = "GET", .headers = {}},
                 std::move(cbs), {});
  ASSERT_EQ(tx.stream_requests().size(), 1u);
  EXPECT_EQ(tx.stream_requests()[0].url, "http://mcp.test/notify");

  tx.stream_push("hello ");
  tx.stream_push("world");
  EXPECT_EQ(got, "hello world");  // 同步投递，确定序
  EXPECT_TRUE(tx.stream_open());
  EXPECT_EQ(close_calls, 0);

  tx.stream_close();
  EXPECT_EQ(complete_calls, 1);
  EXPECT_EQ(close_calls, 1);  // on_complete 之后必有 on_close
  EXPECT_FALSE(tx.stream_open());
}

// ================= C. MCPClient::listen =================

TEST(McpP3Stream, listen_requires_streamprovider) {
  // 只实现 Transport、不开流的包装：listen 必须 fail-closed。
  FakeTransport tx;
  PlainTransport plain(tx);
  MCPClient client(plain, "http://mcp.test/rpc");
  auto r = client.listen(StreamCallbacks{}, {});
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(r.error().find("不支持长连接"), std::string::npos);
}

TEST(McpP3Stream, listen_get_with_session_header) {
  FakeTransport tx;
  MCPClient client(tx, "http://mcp.test/rpc");
  tx.push(script_with_session(ok_initialize(/*list_changed=*/true), "sess-xyz"));
  ASSERT_TRUE(client.initialize().ok());

  auto r = client.listen(StreamCallbacks{}, {});
  ASSERT_TRUE(r.has_value()) << r.error();
  ASSERT_EQ(tx.stream_requests().size(), 1u);
  EXPECT_EQ(tx.stream_requests()[0].method, "GET");
  EXPECT_EQ(tx.stream_requests()[0].url, "http://mcp.test/rpc");
  EXPECT_TRUE(std::any_of(tx.stream_requests()[0].headers.begin(),
                          tx.stream_requests()[0].headers.end(),
                          [](std::string const& h) { return h == "Accept: text/event-stream"; }));
  EXPECT_TRUE(std::any_of(tx.stream_requests()[0].headers.begin(),
                          tx.stream_requests()[0].headers.end(),
                          [](std::string const& h) { return h == "mcp-session-id: sess-xyz"; }));
}

// ================= D. provider watch 全流程（核心）=================

// 收到 list_changed → 重建该 server 前缀注册：新增 C、删除 B、变更 A（描述更新）。
TEST(McpP3Listen, notification_refreshes_server_registry) {
  McpP3Harness h;
  // 脚本序：#1 initialize（带会话 + listChanged）→ #2 首快照（A/B）→ #3 变更后快照（A 改 + C 新）。
  h.tx.push(script_with_session(ok_initialize(/*list_changed=*/true), "sess-abc"));
  h.tx.push(script_ok(ok_tools_list({tool_spec("get_weather", "查天气"),
                                     tool_spec("get_forecast", "看预报")})));
  auto const f = write_list_file(weather_list());

  ToolRegistry reg;
  McpProviderOptions opts{.watch = {"weather"}};
  auto r = mcp_provider(h.mgr, "file://" + f.string(), reg, opts);
  ASSERT_TRUE(r.has_value()) << r.error();
  ASSERT_EQ(reg.size(), 2u);

  // 监听已挂：一个 listener、无 error、活跃收推、流带会话头。
  ASSERT_EQ(r->listeners.size(), 1u);
  McpListener const& ls = *r->listeners[0];
  EXPECT_EQ(ls.server(), "weather");
  EXPECT_TRUE(ls.error().empty()) << ls.error();
  EXPECT_FALSE(ls.closed());
  ASSERT_EQ(h.tx.stream_requests().size(), 1u);
  EXPECT_EQ(h.tx.stream_requests()[0].url, "http://mcp.test/rpc");
  EXPECT_TRUE(std::any_of(h.tx.stream_requests()[0].headers.begin(),
                          h.tx.stream_requests()[0].headers.end(), [](std::string const& x) {
                            return x.starts_with("mcp-session-id:");
                          }));

  // 服务端列表变了 → 推 list_changed（拆两片投递，顺带测 SSE 半帧拼接）。
  // on_change 在 McpListener 的 worker 线程异步跑（IO 线程做同步 client 调用会自我死锁，
  // 见 listen.hpp 头注释）→ 断言前要 wait_until。
  h.tx.push(script_ok(ok_tools_list({tool_spec("get_weather", "查天气（北方区）"),
                                     tool_spec("get_alerts", "气象预警")})));
  std::string const frame = list_changed_sse_frame();
  h.tx.stream_push(frame.substr(0, 24));   // 半帧
  h.tx.stream_push(frame.substr(24));

  EXPECT_TRUE(wait_until([&] { return ls.changes() == 1u; }));
  EXPECT_TRUE(wait_until([&] {
    return reg.size() == 2u && reg.find("weather_get_alerts") != nullptr;
  }));  // worker 重建注册表（单写者/单读者轮询，终态收敛）
  EXPECT_EQ(ls.changes(), 1u);            // 通知计数
  ASSERT_EQ(reg.size(), 2u);              // 整体重建：A 换描述、B 下架、C 新增
  ToolSpec const* wa = reg.find("weather_get_weather");
  ASSERT_NE(wa, nullptr);
  EXPECT_EQ(wa->description, "查天气（北方区）");
  EXPECT_EQ(reg.find("weather_get_forecast"), nullptr);  // 下架的没了
  ASSERT_NE(reg.find("weather_get_alerts"), nullptr);    // 新增的进来
}

// 能力门：server 未声明 listChanged → 不挂流（流空），listener 带 error 观察位，不整体报错。
TEST(McpP3Listen, capability_gate_skips_with_observable_error) {
  McpP3Harness h;
  h.tx.push(script_with_session(ok_initialize(/*list_changed=*/false), "sess-1"));
  h.tx.push(script_ok(ok_tools_list({tool_spec("get_weather", "查天气")})));
  auto const f = write_list_file(weather_list());

  ToolRegistry reg;
  auto r = mcp_provider(h.mgr, "file://" + f.string(), reg,
                        McpProviderOptions{.watch = {"weather"}});
  ASSERT_TRUE(r.has_value()) << r.error();
  ASSERT_EQ(r->listeners.size(), 1u);
  EXPECT_NE(r->listeners[0]->error().find("未声明 tools.listChanged"), std::string::npos);
  EXPECT_TRUE(r->listeners[0]->closed());
  EXPECT_TRUE(h.tx.stream_requests().empty());  // 没开流
  EXPECT_EQ(reg.size(), 1u);                    // 装配照常
}

// watch 点名了但不在发现列表里 → fail-closed 整体报错。
TEST(McpP3Listen, watch_name_not_found_fails_closed) {
  McpP3Harness h;
  h.tx.push(script_with_session(ok_initialize(/*list_changed=*/true), "sess-1"));
  h.tx.push(script_ok(ok_tools_list({tool_spec("get_weather", "查天气")})));
  auto const f = write_list_file(weather_list());
  ToolRegistry reg;
  auto r = mcp_provider(h.mgr, "file://" + f.string(), reg,
                        McpProviderOptions{.watch = {"nope"}});
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(r.error().find("nope"), std::string::npos);
}