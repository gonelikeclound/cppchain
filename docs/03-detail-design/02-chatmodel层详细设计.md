# 02 · ChatModel 层详细设计（M2 实现）

> 承接 `02-arch-design/02-chatmodel层架构设计.md`。记录实现细节、真机联调发现与修复。

## 1. 实现总览

| 文件 | 职责 |
|---|---|
| `core/cancel.hpp` | `CancelToken`（包 `std::stop_token`；`cancel() const`，句柄语义，拷贝共享状态） |
| `model/error.hpp` | `ModelError`（8 种 Kind）/ `ModelException` / `kind_str` |
| `model/options.hpp` | `CallOptions`（含 `tools` wire 通道与 `extra` 透传）/ `ModelConfig`（含 `ca_info`） |
| `model/task.hpp` | 自研协程 `Task<T>`（惰性、`co_await`、`wait()`）+ `CallbackAwaiter` |
| `model/chat_model.hpp` | `ChatModel` 抽象四能力 + `StreamHandlers` |
| `http/transport.hpp` | 传输抽象 + 线程/取消契约 + `HttpError{message, timeout}` |
| `http/fake_transport.hpp` | 脚本化仿真传输（分片/延迟/错误/超时） |
| `http/curl_multi_transport.{hpp,cpp}` | libcurl multi 单 IO 线程事件循环（pimpl 隔离 curl 头） |
| `model/openai/sse.hpp` | SSE 增量解析（跨包/CRLF/多行 data/[DONE]） |
| `model/openai/wire.hpp` | 请求构造、非流式解析、delta 解析、usage 解析 |
| `model/openai_chat_model.cpp` | 回调内核 + 三个门面 |

## 2. 关键设计

1. **单一真实路径**：`stream_async`（回调内核）是唯一实现；`invoke` / `stream` / `invoke_async` 都是它的封装。
   - `invoke` = 内核 + 信号量等待终态消息
   - `stream` = 内核 + 队列（异步回调 → 同步迭代器），错误经 `ModelException` 抛出
   - `invoke_async` = `CallbackAwaiter` 把内核桥接成 `co_await`
2. **传输注入**：`cppchain_model` **不链接 libcurl**，传输经 `http::Transport` 注入 → 仿真/真实现零改动切换。
3. **取消**：`CancelToken` 由调用方持有；传输契约"取消后不得再触发任何回调"；模型侧在解析失败时 `token.cancel()` 自停并立即上报错误。
4. **超时**：`HttpError.timeout` 显式标记 → `ModelError::Kind::Timeout`（不与应用层网络错误混淆）。
5. **curl_multi 事件循环**：`send()` 加锁入队 + `curl_multi_wakeup` 唤醒；循环内 `perform` → 读 `CURLMSG_DONE` → 扫描并摘除已取消请求；状态码从 header 回调解析（跳过 1xx）；`CURLOPT_TCP_NODELAY` 降流式延迟、`CURLOPT_NOSIGNAL` 保多线程安全。

## 3. 真机联调发现与修复（4 项，全部已修 + 回归测试）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 崩溃：`semaphore: Assertion '__update >= 0'` | 同步投递时 2 个 delta + 1 个 done 的 `release()` 早于消费，`binary_semaphore`（上限 1）溢出 | 换 `counting_semaphore<>` |
| 2 | `finish_reason` / `usage` 拿不到 | `invoke()` 经 `stream()` 生成器转发，而生成器**只传 delta、丢弃终态 Message** | `invoke()` 改为直接建在回调内核上，取权威终态 |
| 3 | DeepSeek reasoner 的推理内容被拆成 150+ 碎块 | `merge_content` 只合并相邻 `TextBlock`，未合并 `ReasoningBlock`（M1 当时的简化） | 相邻同类型块（text / reasoning）都合并 + 回归测试 |
| 4 | SSL 校验失败（企业网络） | `api.deepseek.com` 被企业代理 MITM（证书 issuer = Xiaomi CA），系统 CA store 不含该企业 CA | `HttpRequest.ca_info` + `ModelConfig.ca_info` + demo 读 `LLM_CA_BUNDLE`（本机自动探测 `~/.geo/corp-ca.pem`） |

> 教训：#1/#2/#3 都是**单测不易发现、真链路一跑就现形**的缺陷。#3 尤其典型——仿真脚本一次只发一个完整块时永远不会暴露。

## 4. 真机实测（DeepSeek）

```
流式:      首字延迟 190 ms | 总耗时 783 ms | 59 字符
工具调用:  tool_call: get_weather(call_00_jHpk…) args={"city":"旧金山"}  finish_reason=tool_calls
推理映射:  推理块 1 个 / 769 字符（修复前为 150+ 碎块）→ ReasoningBlock ✓
```

## 5. 验证结论

- `ctest`：**45/45 通过**（message 16 + model 29）
- 7 个 demo 全绿（含 `chat_live` 真机）
- 零编译告警（含 libcurl 传输）

## 6. 待办 / 开放点

1. **多路并发未验证**：事件循环支持并发，但尚未写并发用例（M3 并行工具调用前补）。
2. **超长工具返回观测**（M2 卡位问题）：`chat_live` 已记录首字/总耗时，长输出场景待专项 demo。
3. 事件循环归属（全局单例 vs 每实例）、`Task<T>` 调度策略、`batch` —— 见 arch §10。
4. 代理支持（`CURLOPT_PROXY`）在需要时再补。