# 42 · MCP 首期：client 库（HTTP/JSON-RPC，P1）落地

> **链路**：MCP 方案评审（arch-design 25 规划，见 problems/43）→ P1 落地本档。
> 目标：仓库首个 MCP 层——`src/mcp/` 的 `MCPClient`，用仓库现成 HTTP 基建
> （`http::Transport` + `tool::http_json`）打通 MCP 的 initialize / tools/list / tools/call / resources/list。

## 一、问题与方案

### 用户诉求（方案评审拍板）
1. MCP 独立（不依赖 workflow——client 只依赖 http + nlohmann）；
2. provider 节点（tool_registry 扩展，P2）；
3. 接口变化监听（P3）；
4. 与 workflow 对接（make_mcp_tool 桥）。

### 首期范围（本档 = P1）
- `MCPClient`（`include/cppchain/mcp/client.hpp` + `src/mcp/client.cpp`）：JSON-RPC 2.0 over HTTP（同步）。
- `make_mcp_tool(client, info)`：MCP 工具 → ToolSpec（handler 里 call_tool，web_search 同构）。
- 9 个离线单测（FakeTransport mock MCP server）。

## 二、实现

### 1. `MCPClient`（P1 核心）
- **依赖纪律**：只 include `http/transport.hpp`（接口）+ `tool/http_json.hpp`（同步桥）+ nlohmann；不链 libcurl、不碰 workflow——同 web_search/weather 的分层约束（真传输注入、测试注 FakeTransport）。
- **JSON-RPC 封装**：`call(method, params)` → POST `{jsonrpc, id, method, params}` → `tool::http_json` → 解析 `result` / `error`（fail-closed：HTTP 非 2xx、非 JSON 对象、缺 result、JSON-RPC error 全部归 error）。
- **协议头**：`MCP-Protocol-Version: 2025-03-26`、`Content-Type`、`Accept`（含 event-stream 预留 SSE）、可选自定义（auth）。
- **方法**：`initialize()`（握手）、`list_tools()`（解析 tools → McpToolInfo{name,description,inputSchema}）、`call_tool(name,args)`、`list_resources()`（预留打通）。

### 2. `make_mcp_tool`
- `spec.parameters` 直用 MCP `inputSchema`（已是 JSON Schema；缺 type/properties 补空壳）。
- handler：`client->call_tool` → 取 result 的 text content（MCP `content:[{type:"text"}]` 拼段）→ 返回 `{"text":...}`；无 text → 取 `structuredContent`；再无 → 原样 result。read_only 由调用方定（默认 true）。
- 与 `make_web_search_tool` 同构：外部 API → handler → 注册表 → workflow 消费。

## 三、测试（tests/tool/mcp_client_test.cpp 9 例）

| 测试 | 断言 |
|---|---|
| `initialize_handshake_and_headers` | capabilities 解析；请求带 MCP 版本头 + Bearer + Content-Type |
| `list_tools_parses_schema` | tools 数组 → name/description/inputSchema（city.type==string）|
| `call_tool_returns_text_content` | content[0].text 取到 |
| `make_mcp_tool_handler_calls_client` | handler 调 client → {"text":...}；schema 直用 |
| `jsonrpc_error_surfaces` | error 含 code+message |
| `http_non_2xx_surfaces` | error 含状态码 |
| `transport_failure_surfaces` | 传输层错误透传 |
| `missing_result_surfaces` | 缺 result 报错 |
| `make_mcp_tool_structured_content` | structuredContent 槽返回 |

**测试逼出的修正**：`McpResult` 字段顺序（指定初始化器须按声明序）；MCP result 真实形态是 `{content, structuredContent}` 包装——结构化取 `structuredContent` 而非顶层。

## 四、边界

- **同步单发**：P1 每次请求独立 `http_json`（无连接复用/无 SSE 长连）。连接池/SSE 推送是 P3。
- **resources/list** 只打通方法，消费端（模型读资源）后续期接。
- **tools/call 的 isError**：本档未特判（MCP server 可返回 isError:true 表示工具级失败）——P2 工具桥时可补。

## 五、结果

- 全仓 **790 全绿**（781 基线 + 9 新）。
- 仓库首个 MCP 层就位：client 库 + 工具桥 + 离线测试，P2（tool_registry 加 `type:"mcp"` 源）可直接接入。