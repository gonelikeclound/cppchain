# examples/ —— 用法 demo 索引

面向 **M1 Message 层** 的练习场。每个 demo 转译自 langchain 的对应概念/示例，专注一件事。
构建：`cmake --build --preset dev`；运行：`./build/dev/examples/<名字>`。

| demo | 演示什么 | langchain 参照 |
|---|---|---|
| `messages_basic` | 消息类型族：System/Human/AI/Tool/Chat + 内容块（text/reasoning）+ `role`/`text()` 访问 | [Messages] 概念：`SystemMessage`/`HumanMessage`/content blocks |
| `messages_serialize` | 规范 JSON round-trip；整条会话落盘/读回；单条 `model_dump()` | `messages_to_dict` / `messages_from_dict`、「state 必须可 JSON 序列化」契约 |
| `messages_tool_flow` | 工具调用消息协议闭环：tool_calls → 执行 → `ToolMessage` 回填 → `validate_sequence` 强制不变式 | [Function calling]：`bind_tools` → `ToolNode` → `ToolMessage(tool_call_id=…)` |
| `messages_stream_chunk` | 流式帧按序 `operator+` 合并；args 分片累积；`finalize_chunk` 收敛 | `AIMessageChunk.__add__` / `_merge_tool_call_chunks` |
| `chat_invoke` | M2：同步 `invoke` / 协程 `co_await invoke_async` / 错误是常态（expected）/ 实际请求形状 | `model.invoke` / `await model.ainvoke` |
| `chat_stream` | M2：同步流式迭代、回调式 `stream_async`（delta/tool args 分片）、**中途取消后不再回调** | `model.stream` / 回调式流 + 取消 |

> 设计原则：**一个 demo 只讲一件事**。曾经的 `message_pipeline`（综合流水线）因与上述 demo 重复约 70% 已删除；
> 真正有意义的"端到端综合"留到 M3/M7 有真实 agent 循环时再写。
>
> M2 的 `chat_*` demo 默认用 **FakeTransport**（脚本化 SSE 帧）离线运行，不需要网络与 API key；
> 接真实端点（DeepSeek）的 `chat_live` 随 curl_multi 传输一起加入。

## 与后续层的衔接

- `messages_tool_flow` 的"极简工具执行器"→ M3 升级为真正的 `ToolNode`（schema/middleware）。
- `messages_serialize` 的落盘形态 → M5 Checkpointer 的状态持久化契约。
- `messages_stream_chunk` 的合并语义 → M2 接真实 `llm.stream()` 时原样复用。
- `examples/` 同时也是 `04-problems/` 各问题解法 demo 的固定落点。