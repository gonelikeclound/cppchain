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
| `tools_frontline` | 第一档内置工具：`get_datetime`（当前时间/时区）、`calculator`（精确计算兜底，安全求值非 eval）、`fetch_url`（网页正文提取，补全 搜索→打开详情页 闭环）；离线可跑 | `Calculator` / 自研时间工具 / `WebPageContentExtractor` |
| `tools_files` | 第二档文件工具：`file_read` / `file_write` / `list_dir`（沙箱根目录限定）；**写工具 `read_only=false`**——越界/绝对路径/符号链接逃逸三重拒绝，离线演示副作用标记 | `FileManagementToolkit`（`ReadFileTool`/`WriteFileTool`/`ListDirectoryTool`） |
| `tools_arxiv` | 第三档：`arxiv_search` 论文检索（arXiv Atom API，**内置 XML 解析**，免费无 key）；离线演示查询 URL 构造/解析/空结果回喂 | `ArxivQueryRun` |
| `tools_approval` | M7：真实 `file_write` 副作用审批端到端——名单拒绝/回调按参数放行（只许 `notes/`）/**同 call 崩溃重启重放不重复写盘**，硬断言全部基于真实磁盘状态 | LangGraph interrupt 审批前身 + 幂等约束 |
| `structured_output` | M4：结构化抽取、classify 路由/评判、**两策略遵循度对照**（需 LLM 端点） | `with_structured_output` |
| `state_basics` | M5：状态归约（含**同 id 覆盖=压缩改写**）、快照链与回放、跨线程 Store | State/Reducer、Checkpointer、Store |
| `graph_basics` | M6：线性 workflow、条件路由、循环上限、**中途快照恢复续跑** | LangGraph StateGraph |
| `agent_loop` | M7（真机）：多步工具调用、**超长返回截断**、**副作用审批拒绝**、**LLM 摘要压缩** | `create_agent(model, tools, middleware)` |
| `context_budget` | M7（真机）：用真实 usage 量化「工具输出 token 成本」与「压缩前后历史增长」 | — |
| `side_effect_guard` | M7 收口（离线）：**副作用防重放**——重启后不重复扣款、结果未知拒跑 | 幂等键 + 执行日志 |
| `stale_state` | **状态时效性**（真机 A/B）：无守卫用旧值作答 vs 守卫促使重新取数 | 观测时间戳 + TTL 失效标注 |
| `live_tools` | **真网络工具**（真机）：`get_weather`（Open-Meteo，实时天气+预报）与 `web_search`（火山方舟 Responses · 豆包搜索）；LLM 密钥缺失时退化为工具直连自检 | 真实 provider 工具端到端 |
| `history_agent` | **会话历史**（真机）：同一 thread 多轮续跑历史累积 / 不带 checkpointer 的无状态对照 / 不同 thread 的隔离；探针中间件打印"发往模型的 messages 条数"作机械证明 | `thread_id` + Checkpointer 续跑 |
| `research_chat` | **意图二分连续聊天**（真机）：路由器只分 casual（不带工具）/ research（资料查询，挂全部外部数据源工具 = web_search / get_weather / arxiv_search / fetch_url）；**research 轮必须有一次成功的外部取数** —— 任一外源工具成功均可，整轮零外源才由 harness 兜底强制 web_search；判定遍历注册表整表，加新工具不分类；同 thread 多轮记忆 | Router + Skill、Single-Agent、联网取数兜底 |
| `agent_cli` | **命令行 single/react agent**（真机）：`--mode direct/react/react-trim/react-approve` 同一可执行体切四种策略，同一输入对比效果；`-i` 交互 REPL（同 thread 多轮记忆）/ `-p` 单条 / `-f` 文件批量（`---` 分块、`#` 注释）；每轮可读轨迹 + stats（工具往返/拒绝/压缩/token），`--json` 结构化输出供 jq/diff 脚本对比 | `create_agent` + middleware 组合策略、ReAct |
| `subagent_ask_expert` | **M8 ask_expert（真机）**：父把子任务整体委派给 `make_subagent_tool` 包出的子代理，只回 `answer + stats`；token 会计证明主上下文不膨胀；子代理内部副作用工具仍走它自己的审批（父不豁免） | langgraph subgraph-as-tool、ask_expert |
| `subagent_workers` | **M8 orchestrator-worker（串行版，真机）**：supervisor 用 M4 结构化输出拆任务 → 条件 router 按 results 下标串行点选 worker → finalize 汇总；worker 结果只进 `results` 槽，主线 messages 保持干净 | orchestrator-worker（无 Send 的串行版） |
| `subagent_handoff` | **M8 handoff 简化（真机）**：worker 判领域不符 → 调用保留工具 `call_supervisor` → 包裹层 `Command{goto:["supervisor"]}` 上交 → 主管重派 → redispatch 上限兜底 | LangGraph `Command`(goto)/ handoff |
| `m9_approval` | **M9 审批记忆化（离线）**：同一参数指纹只审一次、键序不影响指纹、只读工具免审；结论由 memoh_hits/denials 打印 | 审批记忆化 |
| `m9_artifact` | **M9 超长结果落盘（离线）**：`ArtifactToolResults` 全文→文件，prompt 只放引用行（8151B≈2698 tok → 67 tok）；含 `Message.artifact` 序列化 | artifact 落盘引用 |
| `m9_hybrid` | **M9 混合召回三路对照（离线）**：keyword 3/5 vs vector 4/5 vs hybrid 4/5，向量补回改写、关键词保持在前 | HybridSelector（混合召回） |
| `m9_budget` | **M9 压缩成本/收益量化（真机）**：`TrimHistory.max_tokens` 阈值触发 + 摘要真实 usage（~360 输出 tok 换 ~10k 输入 tok） | token 预算压缩 |

> 设计原则：**一个 demo 只讲一件事**。曾经的 `message_pipeline`（综合流水线）因与上述 demo 重复约 70% 已删除；
> 真正有意义的"端到端综合"留到 M3/M7 有真实 agent 循环时再写。
>
> `chat_invoke` / `chat_stream` 用 **FakeTransport**（脚本化 SSE 帧）离线运行，不需要网络与 key。
> `chat_live` / `live_tools` 走真实端点，配置来自 `.env`（模板见 `.env.example`）；企业代理环境需 CA bundle
> （本机存在 `~/.geo/corp-ca.pem` 时会自动启用，也可用 `LLM_CA_BUNDLE` 指定）。
>
> 实测结论：库传输层 `HttpRequest` 默认 POST；Open-Meteo 这类只收 GET 的查询接口要在工具里设
> `req.method = "GET"`（见 `live_tools`）。

## 与后续层的衔接

- `messages_tool_flow` 的"极简工具执行器"→ M3 升级为真正的 `ToolNode`（schema/middleware）。
- `messages_serialize` 的落盘形态 → M5 Checkpointer 的状态持久化契约。
- `messages_stream_chunk` 的合并语义 → M2 接真实 `llm.stream()` 时原样复用。
- `examples/` 同时也是 `04-problems/` 各问题解法 demo 的固定落点。