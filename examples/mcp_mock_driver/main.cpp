// cppchain —— examples/mcp_mock_driver/main.cpp
// MCP 全链路验证器：对着 examples/mcp_mock 真网络跑完整链路（真实 curl 传输层）。
// 覆盖：url:// 发现 → provider 握手/list/注册 → watch 长连监听（真 curl SSE）→
//   server 推 tools/list_changed → worker 重拉 → 注册表整体重建（新增/删除都验）。
// 判定：任一断言失败 → 打印 FAIL 并 exit != 0（供 verify.sh 传播）。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "cppchain/http/curl_multi_transport.hpp"
#include "cppchain/mcp/manager.hpp"
#include "cppchain/mcp/provider.hpp"
#include "cppchain/tool/registry.hpp"

using namespace cppchain;
using cppchain::tool::ToolContext;
using cppchain::tool::ToolRegistry;
using cppchain::tool::ToolSpec;
using namespace cppchain::mcp;
using Json = nlohmann::json;

namespace {

int g_fails = 0;
void check(bool ok, std::string const& what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fails;
}

template <typename F>
bool wait_until(F&& pred, int timeout_ms = 3000) {
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  int port = 3100;
  if (argc >= 2) port = std::atoi(argv[1]);
  std::string const base = "http://127.0.0.1:" + std::to_string(port);
  std::printf("== MCP 全链路验证（mock @ %s）==\n", base.c_str());

  http::CurlMultiTransport tx;  // 真实 libcurl 传输层
  MCPConnectionManager mgr(tx);
  ToolRegistry reg;

  // 1) url:// 发现（mock 的 /mcp.json 自指代）→ provider 装配 + watch。
  auto r = mcp_provider(mgr, "url://" + base + "/mcp.json", reg,
                        McpProviderOptions{.watch = {"mock"}});
  check(r.has_value(), "provider（url:// 发现 + 握手 + list + 注册）");
  if (!r) { std::printf("  provider 错误：%s\n", r.error().c_str()); return 2; }

  check(r->listeners.size() == 1u, "watch 挂上 mock（变化监听句柄唯一）");
  McpListener& ls = *r->listeners[0];
  check(ls.error().empty(), "listener 无 error（长连接已挂）");

  std::vector<std::string> const expect_init = {"mock_echo", "mock_add",
                                                "mock_add_tool", "mock_remove_tool"};
  std::printf("  初始注册 %zu：", reg.size());
  for (tool::ToolSpec const& t : reg.all()) std::printf(" %s", t.name.c_str());
  std::printf("\n");
  check(reg.size() == 4u && reg.find("mock_echo") != nullptr, "初始 4 个工具就位");

  // 2) 基本调用：add(10,32) → 42。
  tool::ToolContext ctx;
  if (ToolSpec const* s = reg.find("mock_add")) {
    auto out = s->handler(Json{{"a", 10}, {"b", 32}}, ctx);
    check(out && (*out)["text"] == "42", "mock_add(10,32) → 42");
  } else { check(false, "mock_add 已注册"); }

  // 3) 消息 notify：add_tool 新增 report → server 推 list_changed → worker 重建 → 新工具可调。
  if (ToolSpec const* s = reg.find("mock_add_tool")) {
    auto out = s->handler(Json{{"name", "report"}, {"description", "验证新增"}}, ctx);
    check(out.has_value(), "add_tool(report) 调用成功");
  } else { check(false, "mock_add_tool 已注册"); }

  check(wait_until([&] { return ls.changes() >= 1u; }), "收到 list_changed 通知（长连接收推）");
  check(wait_until([&] {
          return reg.size() == 5u && reg.find("mock_report") != nullptr;
        }),
        "worker 重拉并新增 mock_report（注册表 4→5）");
  if (ToolSpec const* s = reg.find("mock_report")) {
    auto out = s->handler(Json{{"q", "x"}}, ctx);
    check(out.has_value(), "新增的 mock_report 可直接调用");
  } else { check(false, "mock_report 已就位"); }

  // 4) 删除 notify：remove_tool(report) → 再推一次 → 注册表回 4。
  if (ToolSpec const* s = reg.find("mock_remove_tool")) {
    auto out = s->handler(Json{{"name", "report"}}, ctx);
    check(out.has_value(), "remove_tool(report) 调用成功");
  } else { check(false, "mock_remove_tool 已注册"); }
  check(wait_until([&] { return ls.changes() >= 2u; }), "第二次收到 list_changed");
  check(wait_until([&] { return reg.size() == 4u && reg.find("mock_report") == nullptr; }),
        "worker 重建移除 mock_report（注册表 5→4）");

  // 5) 重建不伤旧工具：echo 仍在且可用。
  if (ToolSpec const* s = reg.find("mock_echo")) {
    auto out = s->handler(Json{{"message", "全链路 OK"}}, ctx);
    check(out && (*out)["text"] == "Echo: 全链路 OK", "往返重建后 mock_echo 仍可用");
  } else { check(false, "mock_echo 存活"); }

  std::printf("\n== %s ==\n", g_fails == 0 ? "✅ MCP 全链路验证通过" : "❌ 有失败项");
  return g_fails == 0 ? 0 : 1;
}