# 45 · MCP 三期批①：SSE 单发响应解析 + mcp-session-id 会话回带

> **链路**：MCP P1 client（problems/42）→ P2 三路发现/连接/provider（problems/43）→ 本档 = **P3 批①**
> （43 号校正后的 P3 划界：连接升级，不碰 workflow 节点）。目标：让 P1/P2 那套能**直连真实
> streamable-http 服务器**——官方 Everything 实测钉死的两个缺口：响应体是 SSE 帧、要求会话头。

## 一、问题与方案

### 实测起底（Everything Reference Server 2.0.0，streamableHttp）
| 动作 | 结果 |
|---|---|
| `initialize` | HTTP 200，但 `content-type: text/event-stream`（体为 `event: message / data: {…}` 帧），并下发改 `mcp-session-id` 响应头 |
| `tools/list` 不带会话头 | **HTTP 400** `Server not initialized` |
| `tools/list` 带会话头 | HTTP 200，仍 SSE 帧 |

P1 客户端只认「顶层纯 JSON 对象」、不回带会话头 → 真实服务器全连不上。这正是 P3 批①要补的两半。

### 方案
1. **SSE 单发解析**（`MCPClient::call`）：响应体先按纯 JSON 解析，失败按 SSE 帧抽 `data:` 行
   （可多行、去 `event:`/`id:`）再解析。**只做单发消息**，不开长连。
2. **会话头回带**：`mcp-session-id` 是响应**头**——而传输层（`http_json`）此前根本不把响应头
   上浮给调用方。所以要先打通一条「响应头 → `JsonResponse`」的链路（这是批①唯一的架构触碰点）。
3. **错误增强**：非 2xx + JSON-RPC error 体 → 上浮 RPC 消息（如 `Server not initialized`），
   不再笼统报 `MCP HTTP 400`。

## 二、实现

### 1. 传输层：响应头上浮（一处全链路）
- `HttpCallbacks` 加 `on_response_headers(std::vector<std::string> const&)`（可选实现；不含状态行/空行）。
- `JsonResponse` 加 `headers`；`http_json` 收集进 `Sync` → `out.headers`。
- `CurlMultiTransport`：`header_cb` 采集非状态行，**空行处整组上浮**（重定向块各自 flush）。
- `FakeTransport`：`Script` 加 `headers` 字段，交付时触发回调 → 离线测试可模拟会话头。
  （`Script` 按申明序插入 `headers`，唯一的位置初始化用法 `examples/chat_invoke` 已改指定初始化器。）

### 2. MCPClient（`call` 重构）
- 请求：`session_id_.empty()` ? 不带 : 带 `mcp-session-id: <id>`。
- 响应：先抽会话（`session_id_from_headers`，键名**大小写不敏感**，取 `mcp-session-id` 值去空格），
  任何响应头带即记入——initialize 握手首发。
- 体：`parse_sse_or_json(raw)`——纯 JSON 直接用；否则按 SSE 帧抽 `data:` 行（首个事件解析成功即回）。
- 错误：RPC error 对象（含 400+ 与非 2xx）→ `MCP error code: message`；否则 HTTP 状态码；
  拿不到合法 JSON/SSE → `MCP 响应非 JSON 对象/SSE 帧`。

### 3. 真机发现的兼容坑：`cursor:null`
`tools/list` 原来发 `{"cursor":null}` —— Everything 的严格 JSON Schema 校验直接拒：
`Invalid input: expected string, received null`。MCP 里 cursor 可选 → **省略**（`tools/list` 与
`resources/list` 都改 `Json::object()`）。离线测抓不到，真机一把必现。

## 三、测试（tests/tool/mcp_client_test.cpp 新增 6 例 + 1 断言）

| 测试 | 断言 |
|---|---|
| `sse_single_message_response_parsed` | SSE 帧（`event: message`/`id:`/`data:`，按 Everything 实测格式）→ result 正常解析 |
| `sse_error_frame_surfaces` | SSE 帧内 JSON-RPC error → code+message 上浮 |
| `session_id_roundtrip_sent_on_subsequent_requests` | 握手响应头带会话 → `session_id()` 查到；二请求自动回带 `mcp-session-id` 头 |
| `session_id_key_case_insensitive` | 服务端大写 `Mcp-Session-Id` → 客户端照样拿到 |
| `no_session_header_when_none` | 无会话头 → 请求不带 mcp-session-id |
| `session_required_400_reports_rpc_error` | 400 + RPC error 体 → 上浮 `Server not initialized`/`-32000`（非笼统 400）|
| `list_tools_parses_schema` 补断言 | 请求体**不含 `cursor`**（真机坑锁定）|

## 四、真机验收（官方 Everything · 真实 curl 传输层）

`tmp/mcp_live_probe.cpp`（gitignored 探针，静态链接 CurlMultiTransport + manager + provider，
列表 `tmp/mcp_everything.json` 指向 `http://127.0.0.1:3001/mcp`）：

```
发布面（strip_meta，无 token）：[{"name":"everything",...}]
注册工具 16 个： everything_echo everything_get-annotated-message ... everything_toggle-subscriber-updates
everything_echo(你好 MCP) → {"text":"Echo: 你好 MCP"}
```

- P2 的 provider 对**真实服务器**工作：发现→握手（SSE+会话自动处理）→ 16 个真实工具注册进 registry；
- `everything_echo` handler 经 SSE+会话全链路调通并回文。
- 注：当前 Everything 2.0.0 只暴露这 16 个（`add`/`sample-llm` 不在其列）；能力门控工具需 client
  声明 capabilities——Provider 后续可让 `initialize` 按需声明（batch ② 变化监听时再议）。

## 五、结果

- 全仓 **824 全绿**（818 基线 + 6 新；P1 原 9 + P2 25 + P3 6 = MCP 40 例）。
- 零告警（含顺带清掉的两处存量：`wfr/main.cpp` 的 `run_argv` nodiscard、`McpResult`/`Script`
  指定初始化器尾字段；`WFR_TOOL_LIB_DIRS` 双 -D 重定义是 wfr-㊹ 的 CMake 存量噪音，未动）。

## 六、边界与遗留

- **SSE 单发 vs 长连接**：本档只做单发消息解析；收 `notifications`（tools/changed 推送）的
  长连接 + 变化监听已是 **P3 批②**（见 problems/46，事件驱动而非轮询，P3 全齐）。
- **会话 400 只报错不重握手**：`Server not initialized` 上浮给调用方；自动重握手留待后续。
- 后续押后项：`tools/call isError` 特判、`resources/list` 消费端、P5 local stdio、SSE 心跳重连。