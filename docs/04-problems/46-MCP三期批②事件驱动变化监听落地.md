# 46 · MCP 三期批②：事件驱动变化监听（长连接收 notifications，非轮询）

> **链路**：P3 批①（problems/45，SSE 单发 + 会话回带）→ 本档 = **P3 批②**，**P3 至此全齐**。
> 用户裁定（2026-10-23）：
>   1. **不要轮询** —— MCP 主动变更要走 server 推 `notifications/tools/list_changed`，
>      轮询只是无 listChanged / 长连接不可用时的退路；
>   2. 长连接挂进**现有 `curl_multi_poll` 事件循环**（0 新增线程）；
>   3. 「是否长连接」的配置由 **provider 承担**（provider 最懂 MCP）：`McpProviderOptions.watch`，
>      **默认空 = 零长连接**，宿主把要点名的 server 列出来即可 —— 不搞单独配置文件。

## 一、实现（传输能力 → 客户端 → provider 装配）

### 1. `http/stream.hpp` —— 可选的长连接能力契约
- `StreamRequest{url,method=GET,headers}` + `StreamCallbacks{on_status/on_body_chunk/on_error/
  on_complete/on_close}`（错误/正常关之后都必跟 `on_close`；取消 → 无回调）。
- `StreamProvider`：**可选接口**，`dynamic_cast` 探测 —— 有则支持长连接，没有就降级/报错，
  不影响单发调用。CurlMultiTransport 与 FakeTransport 都实现。

### 2. `curl_multi_transport.cpp` —— 流挂进现有 multi
- `open_stream`：长连接 GET 复用同一 `Request`/事件循环结构（bridge 把 on_status/on_body_chunk
  转进 `scbs`）；`timeout_ms=0`（不限时，服务端心跳保活）；loop 的 DONE 分支按 `stream` 走流语义
  （CURLE_OK → on_complete → on_close；rc≠OK → on_error → on_close）；取消照旧无回调后摘除。
- **0 新线程**：流只是 multi 里一个常驻 easy handle，由既有单 IO 线程驱动。

### 3. `FakeTransport` —— 可测形态
- `Script` 之外的独立流机制：`open_stream` 记录 stream_requests()；`stream_push`/`stream_close`
  **同步投递**（测试要确定序；真实路径本是异步）。

### 4. `MCPClient`
- `initialize()` 成功后存 `capabilities_`（tools.listChanged 判断靠它），失败即清空（fail-closed）。
- `listen(scbs, token)`：`dynamic_cast<StreamProvider*>`（不支持 → error）→ GET base_url +
  `Accept: text/event-stream` + `mcp-session-id`。

### 5. `ToolRegistry::remove`（刷新要"删下架的"）
- 中段擦除后下标位移 → 整表重建索引（集合小，O(n)）。沿用"先注册后使用"指针纪律。

### 6. `McpListener`（`mcp/listen.{hpp,cpp}`）—— 变化监听句柄
- 内部 `SseAssembler`：**增量事件组帧**（`partial_` 缓冲半行 + `data_buf_` 跨块累积，`data:` 按
  SSE 语义跨行拼接；真实 TCP 会分片——搜 Everything 实测帧格式回放）。
- SSE 帧里 `method == "notifications/tools/list_changed"` → `changes()++` → 触发 `on_change`。
- **线程纪律（死锁教出来的）**：组帧/识别在传输层 IO 线程；**`on_change` 由监听自有的 worker
  线程执行**（见 §三）。

### 7. `mcp_provider` —— 配置由它承担
- `McpProviderOptions{ std::vector<std::string> watch; }`（默认空）。只有 watch 点名才会尝试挂；
  三项规则：
  - 点名了但不在发现列表 → **整体报错**（配置错要响亮）；
  - server **未声明 `tools.listChanged`** → 不挂流（挂了也没推送），结果里放一个 `error` 非空的
    listener 作观察位（"为什么没挂"），不当报错；
  - 真挂：收到通知 → `refresh_server_registry`（**自由函数**）对该 server **整体重建前缀注册**
    （删旧加新，一个语义覆盖 新增/变更/删除；重拉失败保留旧注册、下个通知再试）。
- `McpProviderResult` 增 `listeners`（`unique_ptr<McpListener>`）——**保有结果 = 保有长连接**；
  move-only。

## 二、测试（tests/tool/mcp_listen_test.cpp 新增 7 例）

| 测试 | 断言 |
|---|---|
| `McpP3Registry.remove_erases_and_reindexes` | 中段擦除后索引重排正确（c 仍可 find）；缺名无害；清空 |
| `McpP3Stream.sync_delivery_and_close_order` | FakeTransport 流同步投递次序；on_complete 之后必有 on_close |
| `McpP3Stream.listen_requires_streamprovider` | 不带流能力的传输 → listen 返回"不支持长连接" |
| `McpP3Stream.listen_get_with_session_header` | listen 发出 GET + `Accept: text/event-stream` + 会话头 |
| `McpP3Listen.notification_refreshes_server_registry` | **全流程**：watch 挂流（流带会话头）→ 推 list_changed（拆半帧投递测拼接）→ worker 重建注册表（A 换描述 / B 下架 / C 新增）；changes=1 |
| `McpP3Listen.capability_gate_skips_with_observable_error` | 未声明 listChanged → 不开流 + error 观察位，装配照常 |
| `McpP3Listen.watch_name_not_found_fails_closed` | watch 点名不在列表 → 整体报错 |

## 三、两颗雷（真机 gdb 挖出，都进了修复与测试）

### 雷 1 —— 悬垂引用 SEGV（provider 内 asbug 的教训）
初版把 `refresh_server` 写成 mcp_provider 局部 lambda 并 `[&registry, &refresh_server]` 捕获，
闭包被存进长寿的 `McpListener`。provider 一返回，这些引用全悬垂；通知到达时 `registry.all()`
迭代到野地址 → SIGSEGV（gdb 栈：`t.name.starts_with(pref)` 里 `t.name` 数据指针成了垃圾
"notifica…"残迹）。
**修**：`refresh_server` 提为匿名命名空间**自由函数**（零捕获）；`on_change` 闭包只按值捕获
**持久指针**（`reg=&registry`、`c=&pool-client`、`prefix` 拷贝）。

### 雷 2 —— IO 线程自我死锁（批② 验收主力挂点）
真机初跑：stream 挂上后 POST 时挂时通。隔离（流层面复现不掉、同 multi 并发 POST 也正常）后
锁定：**通知回调跑在 curl IO 线程上**，回调里 refresh 做同步 `list_tools`（`http_json` 阻塞等
multi 完成），而 multi 的驱动正是 IO 线程 → **线程把自己等死**，之后的任何 POST 全挂。
**修**：组帧/识别留在 IO 线程，`on_change` 全部挪到 `McpListener` 自有 worker 线程
（mutex+cv 单事件队列，串行执行）——refresh 的同步 client 调用不再挡 IO 线程。

两雷的共同教训：**凡回调里要同步往返 client 的，一律不能在传输 IO 线程做；跨寿命闭包只准
捕获持久指针/值的拷贝。**

## 四、真机验收（官方 Everything 2.0.0 · 真实 curl 传输层 · tmp/mcp_live_probe.cpp）

```
== 变化监听 ==
  server=everything  error=""  changes=0  closed=false   ← 长连接挂着，0 线程
  [对照] watch 激活下 everything_echo → Echo: watch 激活下的 echo  ← 无饿死
== 触发 trigger-list-changed ==
  → Sent notifications/tools/list_changed
  推送到达后：changes=1  closed=false                    ← 长连收到 push
  刷新后 everything_echo → Echo: 刷新后仍可用             ← worker 重建无死锁
验收通过：真机收到 list_changed 推送并完成重拉。
```

## 五、结果

- 全仓 **831 全绿**（824 + 新 7；MCP 共 47 例）。
- P3 至此**全阶段完成**：批① SSE 单发 + 会话回带；批② 长连接事件驱动变化监听。
- 零告警（新增代码；本仓其它老目标的重编存量告警未及，与本批无关）。

## 六、边界与遗留

- **worker 单线程串行**：on_change 被 worker 串行执行；宿主业务线程与 worker 并发访问
  registry 时需自备串行（初期面向"agent 空闲等消息"场景已够）。
- **断线不自动重连**：流关闭 → `closed()` 可观察、`changes` 停止增长；自动重挂留后续。
- **心跳/保活依赖服务端**：长连不限时（curl timeout=0），凭 server 心跳判断活；严格断链探测
  留后续。
- **P3 之外仍押后**：`tools/call isError` 特判、`resources/list` 消费端、P5 local stdio、
  SSE 心跳轨道重连。