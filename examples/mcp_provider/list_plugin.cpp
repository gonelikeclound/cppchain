// cppchain —— examples/mcp_provider/list_plugin.cpp
// so:// 发现路线的 demo 插件：dlsym `mcp_list_weather` → 统一 MCP 列表 JSON 字符串。
// 纯 C ABI + 字符串字面量，不链 cppchain（同 tests/plugin_mcp 纪律）。
extern "C" {

// so://mcp_weather_list:weather —— 与 file:// 演示源同一份列表内容，展示"三路同 schema"。
char const* mcp_list_weather(void) {
  return R"({"mcpServers":[{"name":"weather","type":"remote","url":"http://mcp.test/rpc","transport":"http","meta":{"token":"demo-token"},"description":"演示天气服务"}]})";
}

}  // extern "C"