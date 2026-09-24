# examples/ —— 用法 demo 索引

面向各层的练习场。每个 demo 转译自 langchain 的对应概念/示例，专注一件事。

## 运行方式

```bash
./run-demo.sh                  # 编译并跑全部 demo
./run-demo.sh tools_retrieval  # 只跑指定的（可多个）
./run-demo.sh --list           # 列出可用 demo
./run-demo.sh -n chat_invoke   # -n/--no-build：跳过编译
```

脚本会自动 source `~/opt/gcc16/env.sh`（编译与运行都需要：部分 demo 依赖 GCC 16
libstdc++ 的 `GLIBCXX_3.4.35` 符号，系统 libstdc++ 14 不够），并在**仓库根目录**下运行各 demo
——`chat_live` 靠这里找 `.env`，`messages_serialize` 的产物落在 `demo-out/`（已 gitignore）。

等价的手动方式：`source ~/opt/gcc16/env.sh && cmake --build --preset dev && ./build/dev/examples/<名字>`

| demo | 演示什么 | langchain 参照 |
|---|---|---|
| `messages_basic` | 消息类型族：System/Human/AI/Tool/Chat + 内容块（text/reasoning）+ `role`/`text()` 访问 | [Messages] 概念：`SystemMessage`/`HumanMessage`/content blocks |
| `messages_serialize` | 规范 JSON round-trip；整条会话落盘/读回；单条 `model_dump()` | `messages_to_dict` / `messages_from_dict`、「state 必须可 JSON 序列化」契约 |
| `messages_tool_flow` | 工具调用消息协议闭环：tool_calls → 执行 → `ToolMessage` 回填 → `validate_sequence` 强制不变式 | [Function calling]：`bind_tools` → `ToolNode` → `ToolMessage(tool_call_id=…)` |
| `messages_stream_chunk` | 流式帧按序 `operator+` 合并；args 分片累积；`finalize_chunk` 收敛 | `AIMessageChunk.__add__` / `_merge_tool_call_chunks` |
| `chat_invoke` | M2：同步 `invoke` / 协程 `co_await invoke_async` / 错误是常态（expected）/ 实际请求形状 | `model.invoke` / `await model.ainvoke` |
| `chat_stream` | M2：同步流式迭代、回调式 `stream_async`（delta/tool args 分片）、**中途取消后不再回调** | `model.stream` / 回调式流 + 取消 |
| `chat_live` | **真机联调**（DeepSeek）：流式首字延迟、tools wire 通道、`deepseek-reasoner` 推理内容映射 | 真实 provider 端到端 |
| `tools_basic` | M3：工具定义 → 绑定 → 模型发起 → ToolNode 执行 → ToolMessage 回填 → 最终答复；含**错误回喂** | `@tool` / `bind_tools` / `ToolNode` |
| `tools_retrieval` | M3：**大批量工具查找**量化——体积压缩比 + precision/recall（含 IDF 前后对比） | 工具检索（langchain 生态的应用层职责） |
| `tools_retrieval_vector` | **关键词 vs 向量对照实验**（需向量端点）：跨语言/同义改写下 2/8 → 8/8 | embedding 检索（语义召回） |
| `structured_output` | M4：结构化抽取、classify 路由/评判、**两策略遵循度对照**（需 LLM 端点） | `with_structured_output` |
| `state_basics` | M5：状态归约（含**同 id 覆盖=压缩改写**）、快照链与回放、跨线程 Store | State/Reducer、Checkpointer、Store |
| `graph_basics` | M6：线性 workflow、条件路由、循环上限、**中途快照恢复续跑** | LangGraph StateGraph |
| `agent_loop` | M7（真机）：多步工具调用、**超长返回截断**、**副作用审批拒绝**、**LLM 摘要压缩** | `create_agent(model, tools, middleware)` |

> 设计原则：**一个 demo 只讲一件事**。曾经的 `message_pipeline`（综合流水线）因与上述 demo 重复约 70% 已删除；
> 真正有意义的"端到端综合"留到 M3/M7 有真实 agent 循环时再写。
>
> `chat_invoke` / `chat_stream` 用 **FakeTransport**（脚本化 SSE 帧）离线运行，不需要网络与 key。
> `chat_live` 走真实端点，配置来自 `.env`（模板见 `.env.example`）；企业代理环境需 CA bundle
> （本机存在 `~/.geo/corp-ca.pem` 时会自动启用，也可用 `LLM_CA_BUNDLE` 指定）。

## 与后续层的衔接

- `messages_tool_flow` 的"极简工具执行器"→ M3 升级为真正的 `ToolNode`（schema/middleware）。
- `messages_serialize` 的落盘形态 → M5 Checkpointer 的状态持久化契约。
- `messages_stream_chunk` 的合并语义 → M2 接真实 `llm.stream()` 时原样复用。
- `examples/` 同时也是 `04-problems/` 各问题解法 demo 的固定落点。