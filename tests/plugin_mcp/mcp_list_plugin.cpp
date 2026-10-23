// cppchain —— tests/plugin_mcp/mcp_list_plugin.cpp
// MCP 二期 so:// 发现路线的测试插件：dlopen 后 dlsym `mcp_list_<fn>` → 统一 MCP 列表 JSON 字符串。
// 纯 C ABI + 字符串字面量，不链 cppchain 静态库（静态库非 -fPIC 塞进 .so 会 ld 报错，同 mock_tools）。
extern "C" {

// so://mcp_list_plugin:weather —— remote http 服务；meta 带 token（专门验证 strip_meta 剥凭证）。
char const* mcp_list_weather(void) {
  return R"({"mcpServers":[{"name":"weather","type":"remote","url":"http://mcp.test/rpc","transport":"http","meta":{"token":"so-token"},"description":"天气"}]})";
}

// so://mcp_list_plugin:empty —— 合法但零 server。
char const* mcp_list_empty(void) { return R"({"mcpServers":[]})"; }

}  // extern "C"