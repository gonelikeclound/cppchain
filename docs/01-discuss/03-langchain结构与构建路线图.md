# 03 · LangChain/LangGraph 结构分析与构建路线图【定稿】

> 依据官方文档（docs.langchain.com）实地分析，非草稿性转述。结构事实均标注出处文件名，可回查。
> 本文档：**A. 结构分析（参照系）＋ B. 构建路线图定稿**。

---

## A. LangChain / LangGraph 结构分析

### A0. 产品三分（理解现状的总纲）

LangChain 生态已经第三次分层，**三者是堆叠关系**而非并列：

| 层 | 代表 | 提供什么 | 我们参考谁 |
|---|---|---|---|
| **Runtime**（运行时） | LangGraph / Temporal | 持久化执行、Streaming、HITL、Checkpoint | **⭐ 复刻主力**——一切"图引擎"能力在这层 |
| **Framework**（框架） | LangChain | 抽象（Message/Content Blocks/agent loop/middleware）+ 集成 | ⭐ 复刻其抽象 |
| **Harness**（驾驭层） | Deep Agents SDK | 预置工具、提示词、**subagents**（保持主上下文干净） | ⭐ 复刻其"子代理"思想 |

关键事实："LangChain 1.0 构建在 LangGraph 之上"；`create_agent(model, tools, middleware, ...)` 产出的**就是一个 LangGraph graph**，不是独立引擎。

### A1. 核心抽象与依赖关系

```
Message/ContentBlock（无 provider 依赖的领域模型）
   └─ ChatModel 统一接口（invoke/stream/batch/bind_tools/with_structured_output）
        └─ Tool 协议 + ToolSchema + ToolNode（工具调用闭环）
             ├─ Structured Output / OutputParser（schema 驱动输出）
             └─ State + Reducer + Checkpointer/Store 接口（持久化契约）
                  └─ Graph 引擎（Pregel：super-step / Edge / ConditionalEdge / Send / Command）
                       └─ Agent/Harness 循环（create_agent 等价物：middleware + 工具循环）
                            └─ Subagent/多智能体（subgraph-as-tool / orchestrator-worker / handoff）
```

- **Message**：role / content blocks / tool_calls / tool_call_id / id/usage/reasoning；wire 上纯 JSON 序列化；tool_calls 后必须跟 `ToolMessage(tool_call_id=…)` 回填。
- **ChatModel**：统一能力面 `invoke/stream/batch/bind_tools/with_structured_output`；provider 独立成实现类："provider:model" 字符串路由。
- **Tool**：schema 从"函数签名+类型"推导；工具可注入 state/context；**工具可返回 `Command(update, goto)` 回写状态与控制流**（副作用问题绕不开它）。
- **State**：schema + 每键独立 **reducer**（二元 `new=f(left累计, right更新)`，默认 overwrite）；`add_messages` 按 message id 去重追加/覆盖；`MessagesState` 预置单键 state。
- **Graph 执行**：Pregel 消息传递 + 离散 **super-step**；并行节点同 step；`Command = 边+节点更新的融合原语`；`Send(node, state)` 做动态 fan-out（orchestrator-worker/map-reduce）。
- **确定性 vs Agentic**：workflow=预定路径按序跑；agent=代码+工具连续反馈环；LangGraph 价值=**同一图里混编确定性步骤与 LLM 步骤**。
- **Functional API**（`@task/@entrypoint`）：共享同一 Pregel runtime；replay 从 checkpoint 边界重跑、已完成 task 不重算 → **副作用必须包进 task**。
- **Checkpointer（短时）**：`thread_id` 主键；每 super-step 快照 + 细粒度 writes；五方法 `put/put_writes/get_tuple/list/delete_thread`；durability `exit/async/sync`。
- **Store（长期）**：跨线程 kv，`namespace+key+value`；`put/get/delete/search/list_namespaces`，支持语义搜索。
- **Streaming**：底层 `stream_mode`（updates/values/messages/…）→ 上层事件投影；ProtocolEvent 信封 `{seq, method, params}`。
- **Interrupts**：`interrupt(payload)` 暂停→checkpoint→`Command(resume)` 恢复；node **从头重跑→副作用必须幂等**；resume 按 index 匹配，禁止 while 包 interrupt。
- **Subgraphs**：持久化三态 Per-invocation/Per-thread/Stateless；独立 checkpoint namespace；**子代理官方形态 = subagent-as-tool**（ask_expert 模式）。
- **多智能体**：Send fan-out、subgraph-as-tool、`Command(goto, graph=PARENT)` handoff。

### A2. "fork"术语提醒（04-problems 素材）

官方文档里 **"fork" = checkpoint 时间旅行分叉**（任意 checkpoint 上分叉探索不同轨迹），≠ 我们的"子代理委派"。二者语义差异值得 `04-problems/` 写一篇"fork 语义与实现形态"对照。

---

## B. 构建路线图【定稿】

### B0. 已对齐的四个取舍决策

| 决策 | 结论 | 含义 |
|---|---|---|
| Graph 运行时深度 | **极简起步** | M6 先顺序执行 + 内存 checkpoint；Streaming/Interrupt/Send 押后补全（M10） |
| Provider 策略 | **只做 OpenAI 兼容** | M2 只实现一个 OpenAI 兼容 adapter；"provider:model" 预留扩展点 |
| 持久化 | **先内存版** | 五方法接口定死；SQLite 后续换实现不换接口 |
| 问题卡位 | **五大问题全选** | 全部进各里程碑完成标准，集中验证在 M9 |

### B1. 里程碑

| 里程碑 | 构建内容 | 依赖 | 完成标准（含卡位） | 挂靠问题 |
|---|---|---|---|---|
| **M0** ✅ | 技术底座：GCC16/CMake/nlohmann/libcurl | — | `src/smoke` 冒烟绿 | — |
| **M1** ✅ | 基础抽象：**Message + ContentBlock + JSON 序列化契约** | nlohmann | Message 类型族 + 内容块 + tool_calls/tool_call_id；JSON round-trip 测试（16 项） | 契约底层（一切层共用） |
| **M2** ✅ | **ChatModel 抽象 + OpenAI 兼容 Provider** + **流式**（std::generator） | M1+libcurl | `invoke/stream/bind_tools/with_structured_output` 接口 + 真实/仿真 LLM 跑通 | 超长工具返回（流式长输出预研） |
| **M3** | **Tool 层**：ToolSchema + bind_tools + ToolMessage 闭环 + ToolNode 等价物 | M1 M2 | 工具调用闭环 demo（tool_calls→ToolMessage 协议正确） | **大批量工具查找**（工具检索组件：关键词起步→embed 预留） |
| **M4** | **Structured Output / OutputParser** | M2 | schema 驱动输出；可作 route/judge | — |
| **M5** | **State/Reducer + 内存 Checkpointer + Store 接口** | M1 | `MessagesState`/`add_messages`；thread 快照五方法+内存实现 | 上下文压缩（消息历史状态垫底） |
| **M6** | **Graph 引擎（极简）**：StateGraph、Node/Edge/ConditionalEdge、顺序 super-step、Command 简版、checkpoint 挂接 | M5 | 确定性 workflow + conditional 路由 demo | — |
| **M7** | **Agent 循环**：create_agent 等价物（middleware 简版 + 工具路由） | M2 M3 M6 | ReAct 风格可用 agent | **上下文压缩**（hot-path 压缩中间件）；**超长工具返回**（截断/摘要）；副作用可控执行 |
| **M8** | **Subagent/多智能体**：subgraph-as-tool、orchestrator-worker（串行版）、handoff 简化 | M6 M7 | ask_expert / supervisor demo | **副作用与子代理**（主上下文干净验证 + fork 语义对照） |
| **M9** | **五大问题集中收官**：每个解法一个 demo + 横向对比结论 | 全部 | `examples/` 沉淀 + `04-problems/` 成稿 | ⭐ 核心主线 |
| **M10** | **运行时补全**（视 M6 表现取舍）：Streaming / Interrupt / Send 动态 fan-out / 时间旅行 | M6 M7 | 补全后可支撑更复杂的 agent 实验 | 子代理并行 |

### B2. 简化取舍点（已答）

1. ✅ **Graph 做多深**：极简起步。顺序执行 + 内存 checkpoint + 混编先落地；Streaming/Interrupt/Send 进 M10。
2. ✅ **Provider**：只做 OpenAI 兼容（OpenAI/DeepSeek/Ollama/vLLM 通吃），"provider:model" 预留在 Model 工厂。
3. ✅ **持久化**：内存版先行，接口定死后 SQLite 换实现。
4. ✅ **Functional API**：押后；与 M10 的运行时补全一并再评估（C++ 里 `@task/@entrypoint` 的可重放语义是否值得复刻，作为讨论题）。

### B3. 工作方式

- 每个里程碑：进 `02-arch-design/` 定架构 → `03-detail-design/` 定细节 → `examples/` 放 demo。
- 问题卡位触发时：解法实验先放 `examples/`，结论沉淀 `04-problems/`。
- 顺序建议：M0→M9 串行推进，M10 作为弹性阶段。

---

## 下一步

- [x] 路线图定稿
- [x] **M1 完成**：Message + ContentBlock + JSON 序列化契约（14/14 测试绿，demo 验证）
- [x] **M2 完成**：ChatModel + OpenAI 兼容 + curl_multi 传输 + 协程糖；DeepSeek 真机联调通过（45 测试 / 7 demo）
- [ ] **开始 M3**：Tool 层（ToolSchema + bind_tools + ToolMessage 闭环 + ToolNode）