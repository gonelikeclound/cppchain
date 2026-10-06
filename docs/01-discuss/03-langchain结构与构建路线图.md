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
| **M3** ✅ | **Tool 层**：ToolSchema + bind_tools + ToolMessage 闭环 + ToolNode 等价物 | M1 M2 | 工具调用闭环 demo（tool_calls→ToolMessage 协议正确） | **大批量工具查找**（工具检索组件：关键词起步→embed 预留） |
| **M4** ✅ | **Structured Output / OutputParser** | M2 | schema 驱动输出；可作 route/judge | — |
| **M5** ✅ | **State/Reducer + 内存 Checkpointer + Store 接口** | M1 | `MessagesState`/`add_messages`；thread 快照五方法+内存实现 | 上下文压缩（消息历史状态垫底） |
| **M6** ✅ | **Graph 引擎（极简）**：StateGraph、Node/Edge/ConditionalEdge、顺序 super-step、Command 简版、checkpoint 挂接 | M5 | 确定性 workflow + conditional 路由 demo | — |
| **M7** ✅ | **Agent 循环**：create_agent 等价物（middleware 简版 + 工具路由） | M2 M3 M6 | ReAct 风格可用 agent | **上下文压缩**（hot-path 压缩中间件）；**超长工具返回**（截断/摘要）；副作用可控执行 |
| **M8** ✅ | **Subagent/多智能体**：subgraph-as-tool、orchestrator-worker（串行版）、handoff 简化 | M6 M7 | ask_expert / supervisor demo | **副作用与子代理**（主上下文干净验证 + fork 语义对照） |
| **M9** ✅ | **五大问题集中收官**：每个解法一个 demo + 横向对比结论 | 全部 | `examples/` 沉淀 + `04-problems/` 成稿 | ⭐ 核心主线 |
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
- [x] **M3 完成**：工具层 + 可插拔检索（关键词+CJK 2-gram+IDF）；26 测试 / 9 demo
- [x] **M4 完成**：结构化输出（两策略 + 校验 + 回喂重试；含受控对照实验）
- [x] **M5 完成**：状态归约 + 内存 Checkpointer/Store（116 测试）
- [x] **M6 完成**：极简 Graph 引擎（顺序 super-step + 每步快照 + 中途恢复；126 测试）
- [x] **M7 完成**：Agent 循环 + 三中间件（超长返回/副作用/压缩），真机验证（136 测试 / 14 demo）
- [x] **M8 完成**：子代理/多智能体（subagent-as-tool、orchestrator-worker 串行版、handoff 简化；
  `make_subagent_tool` + `as_agent_node` 零既有接口改动），真机三 demo + 07-cases 单测；
  收口于 `04-problems/09`（副作用隔离 + fork 语义对照）
- [x] **M9 完成**：五大问题集中收官——三中间件做深（token 阈值 `TrimHistory.max_tokens`、
  截断 `max_tokens` 预算模式、审批记忆化 `ToolApproval.memo`）+ 收官件（`HybridSelector` 混合召回、
  `Message::artifact` + `ArtifactToolResults` 落盘）；`04-problems/10` 横向对照总表；
  +24 单测（283 全绿）、4 个 m9_* demo
- [x] **M10**：运行时补全**全部落地**——Streaming / Interrupt / Send / **时间旅行(fork)**
  （见 arch 13 与 `04-problems/13`）

---

## 收官记录（2026-09-26 · M0–M9 主线闭口）

> 本段是完成总结的备查：本轮交付了什么、量化底座是多少、从哪继续。作为 M0–M9 主线的收尾标记。

### 本轮里程碑提交

| commit | 内容 |
|--------|------|
| `ea16c69` | **M8** 子代理/多智能体：subagent-as-tool + orchestrator-worker(串行) + handoff（21 文件 / +1894 行） |
| `df05be2` | **M9** 五大问题集中收官：三中间件做深 + 混合检索 + artifact 落盘（31 文件 / +1807 行） |
| `931fab0` | **M10-①** next 升级为包队列契约 + v1/v2 快照兼容 |
| `22b60dc` | **M10-②** 引擎四态包队列 —— Streaming / Interrupt / Send 运行时（+21 测试） |
| `eafb85a` | **M10-③** 引擎事件接入 workflow —— 投影适配器 + interrupt 记录 + 双源互斥（+4 测试） |
| `a73aa42` | **M10-④** 三个纯离线 demo（streaming / interrupt / send） |
| `cda36e9` | **M10-⑤** 收官打勾 —— arch 13 + 验收记录 + README/路线图同步（411 全绿） |
| `7ef02ff` | **M10-⑥** 时间旅行分叉（fork）——`InvokeOptions.fork` + 新线程隔离 + demo（+4 测试，415 全绿） |
| `8ec4128` | **wfr-①** `.wfr/` 元数据目录 —— session 迁移 + 节点诊断补 run/node |
| `da5c9cd` | **wfr-②** trigger 可选 —— 省略 = 宿主手工触发 |
| `c7f9c50` | **wfr-③** import 其他 workflow —— 展开式跨文件复用（+8 测试） |
| `e2c781e` | **wfr-④** 命令行运行器 —— 按名执行 + list/reset/set + `wfr_demo` |
| `053a637` | **wfr-⑤** 调试可视化 —— 单 HTML 调试台（debug_view，+4 测试） |

### 收官日量化底座

| 项 | 数值 |
|----|------|
| 测试 | **283/283 全绿**、零编译告警 |
| demo | **22 个** 可 `./run-demo.sh`（真机 13 / 离线 9） |
| 文档 | 01 讨论 3 · 02 架构 10 · 03 详细设计 6 · 04 问题卡 10 |

### 本轮关键观测数字（真机/确定性可复现）

- **M8 ask_expert**：父轮 input **539 tok**，子代理内部 566 tok 不进父上下文 → 隔离是结构性的
- **M8 orchestrator 串行**：主线 messages 只有 [提问, 汇总]；results 下标对齐 = 执行序
- **M8 handoff**：`call_supervisor`（确定性信号）→ `Command(goto)` 上交 → redispatch 上限兜底
- **M9 artifact**：8151B（≈2698 tok）→ **67 tok** 内联，全文进文件
- **M9 hybrid**：keyword 3/5 → vector/hybrid 4/5（stub 语义轴缺「大盘→行情」是未覆盖项，如实留档）
- **M9 budget（真机）**：8 次压缩、摘要真实 usage 输入 3958/输出 361，估算省 ~10223 tok
  → **~360 输出 token 换 ~10k 输入 token**

### 状态与「从哪继续」

- **主线闭口+推进**：M0–M10 全部 ✅，langchain/langgraph 核心抽象（Message/Model/Tool/State/Graph/
  Agent/多智能体 + 运行时补全）+ 五大问题解法已跑通验证。M10 四项（Streaming/Interrupt/Send/
  **时间旅行 fork**）已落地（`04-problems/13`）。**wfr 系列**跟进（arch 14）：命令行运行器
  （按名执行 + `.wfr/` 目录）+ trigger 可选 + workflow import + 单 HTML 调试台（438 全绿）。
  **真并行**仍押后。
- **暂缓备选**：
  - 运行时剩余：真并行（包/队列已按可并行设计 + 需 barrier）、fork 的分叉点改写（update_state）、
    Send × workflow IR、`c_model` 切流
  - wfr 剩余：cron 常驻调度、调试台大图布局/体验增强
  - M9 开放点：动作级评测集、精确 tokenizer、Journal 持久层、`tool_search` 元工具
- **继续入口**：读本文件「下一步」与「路线图 B1」；增量需求按对应层的 arch/详细设计追加。

### 阅读导航（按需）

| 想看 | 去读 |
|------|------|
| 多智能体设计与落地 | `02-arch-design/09` + `03-detail-design/05` + `04-problems/09` + `examples/subagent_*` |
| 五大问题如何收口 | `02-arch-design/10` + `03-detail-design/06` + `04-problems/10` + `examples/m9_*` |
| 各层抽象（Message/Model/Tool/State/Graph/Agent） | `02-arch-design/01..08` + `04-problems/README` 文件列表 |