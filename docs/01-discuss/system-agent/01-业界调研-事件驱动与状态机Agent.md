# 01 · 业界调研：事件驱动与状态机驱动 Agent

> 调研日期：2026-10-29
> 方法：四线并行取一手资料（协议规范 / 厂商文档 / 开源源码文档 / arXiv），共约 150 个来源，400+ 条结论。
> 一手笔记见 `_research/01`~`_research/04`；与 DSH 现状的对照见 `_research/05`。
> 本文只写**判断**，细节与逐条引用留在原始笔记里。

---

## 0. 结论先行

1. **"工具调用立刻返回、结果以事件形式晚到"已经被彻底解决，不必自研。** 至少有五种成熟形态
   （Awakeable/task token、`request_info`+resume、`interrupt()`、`suspend()/resume()`、long-running tool），
   差别只在"等待期间花不花钱"。**选型硬条件应是"等待不烧 token、不占并发槽"。**
2. **"重启后恢复等待中的续体"——全行业没有人真的做。** 所有框架的恢复语义都是**重放**，
   能序列化的只是**数据**（事件日志 / step 结果表 / journal），不是闭包。AutoGen 官方直接承认：
   阻塞等待外部输入会把团队置于「**cannot be saved or resumed**」的状态。
3. **电话这类状态机，工业界的答案非常一致：状态机留在平台侧，业务侧只收事件 + 用聚合根 id 反查状态。**
   SIP/Android Telecom 只"上报事件"；Twilio 把 TwiML 文档当状态、`action` URL 当转移、`StatusCallbackEvent` 当事件推送，
   业务侧是无状态 webhook handler + 外部状态存储，聚合根是 `callSid`。**这直接印证了本专题的前提。**
4. **学术侧已经在转向，且增益可测**：Agent-SAMA 把屏幕当状态、动作当转移（FSM），AndroidWorld 63.7%；
   SPlanner 用 EFSM 生成执行路径，**+28.8pp**。但两者都**非持久、不可重放**，绝对水平仍不足（30%+ 失败）。
5. **真正的空白是一个三合一原语**：**显式迁移表 × 可记账的 LLM 咨询 × 咨询等待期进程可死**。
   现有框架普遍要求你在这三点里**至少放弃一个**（放弃确定性 / 放弃挂起 / 放弃幂等）。
6. **最强的单点参照是 Microsoft Agent Framework**：superstep 图 + 可为纯代码的 executor + 一等状态
   + pending request/response 进检查点 + "等待不烧 token"。它三条全沾到，**唯独没有"状态机"这层定义**
   （没有迁移表，转移逻辑散落在各 executor 的 `@handler` 里，无法静态校验、无法穷举）。
   → **SA 要补的正是这层定义。**

---

## 1. 先把"三类诉求"拆成可回答的问题

笼统讨论"事件驱动"会让四件不同的事糊在一起。拆开之后，业界的答案泾渭分明：

| 诉求 | 精确的提问 | 业界现状 |
|---|---|---|
| **A 异步** | 调用发出后立刻返回，结果晚到，靠什么原语？ | ✅ 已解决（5 种形态），按"等待期成本"选型 |
| **B 状态机** | 长生命周期能力（通话）的 N 个状态由谁持有？ | ✅ 有稳定范式（平台持有 + 事件 + 聚合根） |
| **C 恢复** | 进程被杀，等待中的续体怎么接上？ | ⚠️ 只解决到"重放 + 记账"，续体无人捕获 |
| **D 咨询** | 模型只在声明的接缝上被调用一次，能否记账？ | ❌ 空白（有状态机的没咨询原语，有咨询的没迁移表） |

**这个拆法本身就是一个结论**：SA 的难度不在 A 和 B（有现成范式），而在 C 和 D 的交界处。

---

## 2. 线一：持久化执行基座 —— 等待必须"卸载"

Temporal / Restate / DBOS / Inngest / Step Functions / Azure Durable / Cloudflare Workflows 七家，
形态各异但收敛到同一组工程规律：

**① 外围唤醒凭据是统一抽象。** Temporal 的 Async Activity Completion（task token）、
Step Functions 的 `.waitForTaskToken` + `SendTaskSuccess`、Restate 的 **Awakeable**，
本质是同一件东西：*一个一次性的、可跨进程兑现的凭据*。
→ 见 [Temporal 延迟回调](https://docs.temporal.io/design-patterns/delayed-callback)、[Restate services](https://docs.restate.dev/foundations/services)

**② 结果可能比等待先到，凭据必须"先到先存"。** Restate awakeable 的信箱语义解决这件事。
在事件驱动系统里这是常态，不能靠内存 promise 兜。

**③ 三种落地形态语义差别很大，不能混用。** ①**回调令牌**（Temporal Async Activity Completion、
Step Functions `.waitForTaskToken`）②**信箱事件**（Temporal Signal、Azure external event、Cloudflare `sendEvent`）
③**句柄轮询**（MCP Tasks、OpenAI `background:true`）。
→ SA 的取舍：**以①为主干**（延迟最低、可精确兑现、无需调用方轮询），**以②为补充**（解决"结果早于等待到达"），
**不把③当主路径**（它把延迟与服务成本推给调用方，只在跨信任边界/跨设备时才值这个价）。

**④ per-key 单写者是共识答案。** Restate virtual object（每 key 一条队列）、Temporal 一个实体一个 Workflow、
DBOS queue key、Inngest concurrency key —— **跨 key 并行、同 key 串行**。
这正是"一台通话状态机"需要的模型。

**⑤ 等待必须从计算资源上摘下来。** Restate suspend、Inngest 等待不占并发、Cloudflare waiting 不计并发、
Temporal 只留一条 `ActivityTaskScheduled` 事件。**"等不花钱、不占槽"是长任务系统的第一条性能预算。**

**⑥ 恰好一次只在事务边界内成立。** 只有 DBOS（Postgres 事务）与 Azure MSSQL provider 敢说"不重复"，
其余全是"**至少一次 + 幂等键**"。幂等键粒度分三级：请求级 / 事件级 / 工作流级。

**⑦ 确定性重放与 LLM 天然冲突，且已被官方点名。** Temporal 的做法是"LLM 调用放 Activity，
在重放路径之外执行"。→ **决策层必须是纯函数，所有 LLM 调用与工具执行都是被记账的 step。**
（[Temporal 确定性约束](https://docs.temporal.io/workflow-definition#deterministic-constraints)、
[DBOS 并发执行与 fencing](https://docs.dbos.dev/explanations/concurrent-executions)）

**协议层正在"句柄化"而非"会话化"**：MCP 新版删掉 session 与 SSE 续传，改为服务端铸造的显式句柄；
MCP Tasks 降级为**扩展且只提供轮询**（[MCP Tasks 扩展](https://modelcontextprotocol.io/extensions/tasks/overview)）。
**结论：持久化是运行时自己的责任，不要指望协议层。**

---

## 3. 线二：Agent 框架 —— 三条诉求各有归属，合成原语缺失

横向对照（完整 13×6 表见 `_research/02`）：

| 框架 | 控制流单元 | 异步工具原语 | 重启恢复 | 等待烧 token | 显式迁移表 |
|---|---|---|---|---|---|
| **Microsoft Agent Framework** | executor/edge，批=superstep（Pregel/BSP + 同步屏障） | `request_info` + `@response_handler` | superstep 检查点（含 pending msg **与** pending request） | **否**（官方明示） | ❌ |
| AutoGen v0.4+ | message（`RoutedAgent`） | topic 广播，**无回程配对** | `save_state`，会话边界级；**阻塞等待态不可恢复** | 否 | ✅（core 层可写） |
| LangGraph | superstep/node | `interrupt()` + `Command(resume=)` | node 级 checkpoint，**该 node 从头重跑** | 否 | ✅ |
| Google ADK | workflow agent（Seq/Par/Loop） | `LongRunningFunctionTool` | `ResumabilityConfig`；**恢复多付 1 次模型调用** | **是** | ✅（最强表述） |
| SK Process Framework | step + event（常驻事件循环） | `emit_event` 注入 | Local 无持久化；Dapr 经 actor 持久化 step 状态 + 入站队列 | 否 | ⚠️ 事件数据流，无迁移表 |
| Mastra / Pydantic AI / CrewAI Flow | step/node（图即状态机） | `suspend()/resume()`、deferred tools | 各有快照，但**resume 会重新进入同一 execute** | 否 | ✅ |
| Claude Agent SDK / OpenAI Agents SDK | 固定 agent loop | `defer` / `needs_approval` | 对话 jsonl + resume | 是 | ❌ |
| AgentScope / LlamaIndex WF | agent 方法 / step+typed event | ❌ / `InputRequiredEvent` | 对象态 / `Context.to_dict()`（含在途事件） | — | ✅/组合层 |

**三条硬结论：**

- **"编排层零模型调用"已是行业共识**（ADK 说得最硬：deterministic and predictable without consulting a model；
  LangGraph 官方二分 `Workflows` vs `Agents`）。→ 继续在每次跃迁前问模型，是逆着所有框架走。
- **恢复粒度决定成败**：只存"当前状态 + 对话历史"的方案（AutoGen `save_state`、AgentScope `StateModule`）
  **重启后接不上**；能接上的都存了"**在途事件队列 + 待兑现请求**"（MAF 检查点同时含 pending messages 与
  pending requests，是目前最完整的商业实现
  ——[MAF checkpoints](https://learn.microsoft.com/en-us/agent-framework/workflows/checkpoints)；
  LlamaIndex 的 `to_dict()` 明确含在途事件，且官方文档是**最诚实的恢复边界说明**：
  at-least-once、半途 step 从头重跑、无内置 checkpointer
  ——[LlamaIndex durable workflows](https://developers.llamaindex.ai/python/llamaagents/workflows/durable_workflows/)；
  SK Process 只有 Dapr runtime 才存"step 状态 + 入站消息队列"）。
- **有状态机的没咨询原语，有咨询的没迁移表**。最接近的 MAF 缺的正是"迁移表"这一层——
  这层缺失导致**无法静态校验、无法可视化、无法穷举状态空间**。
- ⚠️ **别被营销词误导**：**Semantic Kernel 的 Process Framework 不是状态机**，它是
  "事件驱动 step 数据流 + 每步状态容器"，没有"当前状态 ∈ S、事件 e → 下一状态"的迁移表；
  且 Local runtime **无持久化**。这是本轮调研中最容易被名字骗过去的一处。

---

## 4. 线三：电话与 OS Agent —— 现实系统怎么做，研究到哪一步

### 4.1 电话：状态机在平台侧，业务侧只收事件

- **协议层只"上报事件"**：RFC 3261 的 INVITE 事务有 `Calling/Trying→Proceeding→Completed→Confirmed→Terminated`，
  超时与错误都是"inform TU"（[RFC 3261](https://www.rfc-editor.org/rfc/rfc3261.html)）。
- **移动 OS 把它做成公共 API 契约**：Android `Connection`/`Call` 用枚举 + `registerCallback` 暴露，
  `getState()` 已废弃并迁到回调快照 `Call.Details.getState()` —— **状态随事件送达**
  （[Call](https://developer.android.com/reference/android/telecom/Call)、[Connection](https://developer.android.com/reference/android/telecom/Connection)）。
- **规模化平台的范式**：TwiML 文档≈状态，`action` URL≈转移，`StatusCallbackEvent`≈异步 HTTP POST 事件，
  终态语义分离（busy/no-answer/failed/canceled），聚合根 `callSid`，状态存业务侧数据库
  （[Call resource](https://www.twilio.com/docs/voice/api/call-resource)、[Dial](https://www.twilio.com/docs/voice/twiml/dial)、[Media Streams](https://www.twilio.com/docs/voice/media-streams/websocket-messages)）。
- **中断是可恢复的**：LiveKit `resume_false_interruption` 在误打断后**从中断处续播**；
  Pipecat `InterruptionFrame` 取消 in-flight LLM/TTS 但保留 `interruptible=False` 的帧
  （[LiveKit turns](https://docs.livekit.io/agents/build/turns/)、[Pipecat interruptions](https://docs.pipecat.ai/pipecat/fundamentals/interruptions)）。
- **长连接寿命必须显式建模**：Gemini Live 连接约 10 分钟、断连即会话终止，为此提供 `sessionResumption`（token 2h）、
  断前 `GoAway`；异步结果何时注入由调用方声明（`NON_BLOCKING` + `scheduling: INTERRUPT|WHEN_IDLE|SILENT`）
  （[Live sessions](https://ai.google.dev/gemini-api/docs/live-session)、[Live tools](https://ai.google.dev/gemini-api/docs/live-tools)）。
- **制度层面禁止"后台挂着一个等着"**：Android 12+ 后台不能起 FGS（否则 `ForegroundServiceStartNotAllowedException`），
  豁免几乎都是"收到某个事件"
  （[FGS 后台启动限制](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start)）。
- **平台把"能力暴露"本身做成了异步签名**：Android 16+ 的 `AppFunctions` 用 `suspend fun` + 预定义异常 +
  OS 索引的 schema 描述 app 能力，官方自我定位接近"MCP 工具"（[AppFunctions](https://developer.android.com/ai/appfunctions)）。
  → **系统能力的调用契约在签名层面就是异步的**，这是 SA 采用事件形态的又一个制度性依据。

### 4.2 OS Agent：仍是线性 ReAct，但已出现状态机转向

- 主流（AndroidWorld / OSWorld / Mobile-Agent / AppAgent / AutoDroid）几乎全是 ReAct 式线性循环。
- **论文自己点名的缺陷**：agent「remain **fundamentally reactive**... lack a structured representation of the
  app navigation flow, limiting ... detect unexpected execution results, and recover from errors」
  （[Agent-SAMA](https://arxiv.org/abs/2505.23596)）。
- **已有关键反例（增益可测）**：
  - **Agent-SAMA**：屏幕当状态、动作当转移，AndroidWorld 63.7%、恢复率 71.9%（同上）
  - **SPlanner**：EFSM 生成执行路径 + LLM 做 executor，**+28.8pp**（[SPlanner](https://arxiv.org/abs/2505.14141)）
  - **AgentProg**：程序化上下文 + belief state 对抗上下文爆炸
- **明确的研究空白（未找到）**：把"OS 级异步事件（通知/来电/权限弹窗/系统对话框）作为一等驱动源"的论文；
  OS agent 领域的 durable execution / checkpointer。最接近的工业实践是 LiveKit/Gemini 的**会话恢复**，
  恢复的是"播放位置/会话"，**不是被挂起的工具调用续体**。

---

## 5. 线四：形式化基础 —— 为什么 statechart 而不是"一个 while 循环里的 dict"

| 概念 | 适合表达 | 在 SA 中的角色 |
|---|---|---|
| **Statechart（Harel）** | 层次、正交并发、历史状态、状态级超时、广播事件 | **架构骨架**：一能力一图；状态配置即权威状态 |
| **扁平 FSM** | 小而封闭的协议（<20 状态） | 叶子节点；也是 LLM 输出最合适的**受限目标** |
| **Behavior Tree** | 单次决策内的技能组合、可插拔、反应式重评估 | 状态内的执行器（进入某状态后怎么做完） |
| **Actor** | mailbox 串行 → **彻底消除实例内并发**；监督/重启 | **运行时模型**：一实例 = 一 actor |
| **Saga** | 长事务补偿、pivot 提交点 | **副作用治理**：工具声明 `{idempotent, compensable, compensator}` |
| **事件溯源** | `state = fold(events)`，恢复路径=运行路径 | **持久化与审计**；对话历史只是它的一个投影 |
| **Workflow Engine** | 重试/超时/定时器/可视化运维 | 可选执行底座（与 statechart 重叠，需分层或二选一） |
| **ReAct loop** | 探索性、一次性、无需审计 | **被替代对象**；可降级为 `Exploring` 状态内的有界兜底 |

三个关键论断：

1. **层次化的语义压缩效果是硬的**：扁平 FSM 的状态数是各特征状态的**乘积**，statechart 是**和**。
   一通电话的"通话阶段 × 音频通道 × 用户打断 × 网络质量"用扁平 FSM 是乘积爆炸，用正交区域是四个独立区域。
2. **事件日志是唯一真相，LLM 对话历史不能充当状态。** ReAct 的根本错误是把"记忆"当"状态"——
   `state = fold(events)` 才能保证恢复路径等于运行路径（[SCXML](https://www.w3.org/TR/scxml/)、[Harel 1987](https://doi.org/10.1016/0167-6423(87)90035-9)）。
3. **LLM 只是 policy 的一个实现。** POMDP 里 belief update 与 policy 可分离；部署态 LLM agent 是
   "history-conditioned policy，没有显式 belief 状态"——这正是我们要用显式状态补上的东西。

**"LLM + 状态机"已有研究谱系，不是空白**（下述 arXiv 编号本轮均经 API 逐条校验）：

- [StateFlow](https://arxiv.org/abs/2403.11322)：状态驱动的工作流求解，含状态依赖任务上 **+13~28pp，成本降到 1/5~1/3**
- [HEXIS](https://arxiv.org/abs/2609.30123)：**把 agent skill 编译成扩展有限状态机（EFSM）**，
  静态检查 + trace 重放验收，**+16.2pp 且 token 降 38~89%** —— 这是"LLM 生成受限状态机"最接近的一例
- 另有 Stagent（校验合法 FSM trace，0.963）、MetaAgent（自动生成 FSM 拓扑）、MetaGPT 的 SOP 化、AWM、SMoT
- [Belief-State Engine](https://arxiv.org/abs/2609.10036)：明确断言"部署态 LLM agent 是 history-conditioned policy、
  无显式 belief"，修法是把 belief 放到 LLM 之外、只暴露后验 —— **这从理论上支持"状态不该住在模型里"**

**但没有任何工作把"显式迁移表 + 可记账的 LLM 咨询"合成到同一原语里。**

⚠️ **同时要诚实标注三个检索缺口**（`_research/04` 已逐条写明"未找到"）：
① 层次 + 并发状态图 + 快照 + Saga 的 LLM 系统性论文；
② 用 actor mailbox 串行化解决 agent 事件乱序的论文；
③ Saga/补偿与 agent 副作用回滚的正式论文。
→ **这三块没有论文背书，只能靠工程论证与可执行验收来立信**。不要把"看起来合理"当成"有研究支持"。

---

## 6. 跨线共性：六条被反复验证的工程规律

这六条不是我的发明，是四条独立证据线各自收敛到同一处：

1. **等待必须卸载**（等不花钱、不占槽）。成本模型应以**事件数**计，不以**时长**计。
2. **续体不可序列化，能序列化的只有数据**。所以"恢复"的工程唯一解法是
   *把非确定性操作包进记账单元，重放时用记录值代替重算*（`@task` / `ctx.run` / activity 三条同构）。
3. **per-key 单写者**。跨 key 并行、同 key 串行；乱序问题从全局降为边界问题。
4. **至少一次 + 幂等键是现实**，exactly-once 只在事务边界内。契约里就该写 at-least-once。
5. **事件日志是唯一真相，对话历史只是投影**。恢复 = 重放，而非从栈帧恢复。
6. **协议在句柄化**。连接不再是状态载体；持久身份是 `run_id` / `callSid` 这类聚合根。

---

## 7. 真正的空白：三选二到处都有，三选三没有

把上面的证据拍在一起，空白的具体形状就清楚了。**现有框架要求你在三点中至少放弃一个：**

- **放弃确定性**：让模型决定下一步迁移 → 退回 ReAct（LangGraph Agents、ADK LlmAgent、Claude SDK）
- **放弃挂起**：把咨询放在同步路径里，一次 turn 等到底 → 语音场景直接不可用
- **放弃幂等**：接受 resume 时重问一次 LLM、重付一次费用 → Mastra 的 resume 会重新进入同一 `execute`，
  ADK 恢复多付一次模型调用

**SA 的命题就是三个都要。** 具体地，缺的原语是：

> 一个**显式迁移表**（当前状态 + 事件 → 下一状态，纯函数、可回放、可静态校验），
> 其中某些边标注为 **gate**；穿过 gate 时发起一次**可作为记账单元重放的 LLM 咨询**；
> 咨询等待期间进程可以死，重启后凭账本继续，**不重问**。

这正好把三件事钉在同一个对象上，而这件事**没有任何框架做过**。

另有两个次级空白（连讨论都很少）：
- **OS 级异步事件作为一等驱动源**（研究侧未找到）
- **观测归约**：事件 → 上下文的事前准入策略（业界只有事后压缩/截断）

---

## 8. 反面证据（诚实记录）

不写这些，方案就是自我说服：

1. **平台厂商自己劝你先别上结构。** LiveKit 原文：每种模式都会"adds complexity, latency, or context management overhead.
   **Split workflows only when you encounter a concrete limitation**"——触发条件是指令膨胀/工具权限冲突/需多轮结构化收集/需回退。
   → 状态机是"遇到具体限制才付的成本"，在语音这种延迟极敏感场景，过早引入会吃掉延迟预算
   （[LiveKit workflows](https://docs.livekit.io/agents/logic/workflows.md)）。
2. **"文档即状态"很脆。** Twilio 官方警告：控制流跳转后，前面的 verb 可能**永久不可达**
   （`<Say>` → `<Redirect>` → `<Say>`，第二个 `<Say>` 不可达）。把状态编码进可执行文档流，
   一旦跳转就丢隐式顺序假设——**这是状态机刚性的典型代价**（[TwiML](https://www.twilio.com/docs/voice/twiml)）。
3. **正确状态机的成本极高。** 仅一个 INVITE 就有 client/server 两套事务状态机、`Timer A/B/D/G/H/I`、
   forking 产生多个 early dialog，RFC 3261 正文一万五千行；业界普遍用 Twilio/LiveKit 把这层**封掉**而非让业务直连。
4. **学术侧的状态机方案仍不可靠**：Agent-SAMA 的 FSM 是**实时推断、非持久、不可重放**的，
   仍有约 36% 失败；SPlanner 63.8% 同样远未可用，且额外引入 EFSM 构建与多 agent 协作开销。

**对我们的含义**：SA 必须是**可选的结构强度**，而不是全局替换。
默认仍允许 `Exploring` 状态里跑有界 ReAct；只有当(能力长生命周期 / 需要审计 / 需要回归 / 事件并发)任一成立时才上状态机。

---

## 9. 对 SystemAgent 的设计约束（十二条，合并四线后去重）

| # | 约束 | 验收口径 |
|---|---|---|
| C1 | **状态机支持层次 + 正交区域 + 历史状态** | 全局 `Cancel`/`AuthExpired`/`BudgetExceeded` 只在根定义一次即作用于所有子状态；"等用户输入"与"等工具结果"可同时活跃且不产生复合状态名 |
| C2 | **一实例 = 一 actor，mailbox 串行** | 注入 1000 个乱序事件后终态唯一；同实例处理函数永不并发 |
| C3 | **迁移函数是纯函数**，不含 I/O 与 LLM | 任意事件序列可纯回放且终态唯一；生产日志可直接转回归测试集 |
| C4 | **事件日志是唯一真相**，`state = fold(events)` + 快照 | `kill -9` 后仅凭"快照+事件"恢复的终态与对照组**逐字段相等** |
| C5 | **工具调用一律"立即返回 receipt，结果以事件晚到"** | 重复结果被守卫丢弃并记 `DUPLICATE_DROPPED`；陈旧结果被状态守卫拒绝；工具 p99 **不计入**跃迁延迟 |
| C6 | **等待态卸载**（等不烧 token、不占槽） | 一通 10 分钟通话的模型调用数 ≤ 咨询 gate 数，与通话时长无关 |
| C7 | **LLM 只出现在显式 gate 上，输出被约束为有限动作集或状态机片段** | CI 断言单次流程 LLM 调用数 `k ≤ K_max` 且与步骤数 `N` **解耦**；未通过确定性校验的输出不能触发转移 |
| C8 | **咨询是记账单元**：输入输出进账本，恢复时取记录值不重问 | 在 gate 等待时 `kill -9`，重启后同一 gate **只产生一次**模型调用（账本命中） |
| C9 | **LLM 的输入是事件日志的投影**，禁止投喂原始对话历史 | 构造超长历史时请求 token 有界；投影函数有独立单测；可证明 LLM 未见原始 history |
| C10 | **每个工具声明 `{idempotent, compensable, compensator}`；不可补偿动作前必须有显式 `pivot`** | 静态检查能枚举所有 `compensable=false` 动作，且证明每条到达路径都过某个 `pivot` |
| C11 | **`Compensating` 是可持久化子状态机，不是 catch 块里的循环** | 补偿执行一半崩溃，重启后从中断步骤继续，已完成步骤不重复 |
| C12 | **恢复与回归只依赖事件重放**，禁止优雅关闭钩子 | crash-only 测试在任意随机步数 `kill -9` 均恢复一致；状态图静态检查"不可达状态=0、无未声明死锁" |

> **最小充分集**（若需裁剪）：**C2 / C3 / C4 / C6 / C7 / C8**。
> 分别锁住：事件序确定性、可回放、可恢复、成本可控（等不烧钱）、模型不外溢、咨询不重复付费。
> 这六条是"事件驱动 + 状态机驱动"成立的下限；其余是治理与副作用正确性。

---

## 10. 对照 DSH 现状（详见 `_research/05`）

| 能力 | DSH 现状 | 结论 |
|---|---|---|
| 句柄 + 完成通知 | ✅ `ctx.jobs`（立刻返回 id、终态通知、`reported` 抑制重复） | 直接用 |
| 外部事件 → 唤醒空闲 agent | ✅ 已实现（busy→inject / idle→followup，`maxConsecutiveWakes=3`） | **唯一生产者是 job 终态，需扩展** |
| 事件 → 对话的准入层 | ✅ `send(target × wakeup)` + `agent/inbox/spliced` | 观测归约应落在这里 |
| 持久化定时事件 + 冷恢复 | ✅ `schedule`（状态写 session 日志、live 时补跑） | **续体账本的现成范式** |
| 进程重启后续体存活 | ❌ 明确 deferred（"survival across runtime restarts requires a separate durable-job design"） | **G2 缺口** |
| 细粒度语义事件 | ❌ 只有终态；`readOutput` 是字节流 | **G1 缺口** |
| 显式状态机 | ❌ 状态在模型上下文里 | **G3 缺口** |
| 咨询 gate | ❌ 二元：模型全程在环 or 完全不在环 | **G4 缺口** |

还有一个来自 DSH 的**实证**，几乎是本专题最好的论据——`2026-08-11` 那条 Agent Note 记录的失败：

> "the model starts a long command, tells the user it started it, ends its turn,
> and the command finishes into an inbox nobody will claim."

这就是"用 ReAct 循环承载异步任务"的必然结果：**模型的 turn 结束了，任务的状态却还在飞**。
DSH 的修复是"唤醒空闲者"，但那只补了投递，没有补状态。

---

## 11. 调研边界（哪些没读到一手）

- `platform.claude.com` / `developers.openai.com` 对本网络 403/区域拦截：Anthropic Managed Agents 正文、
  `pause_turn` 语义、OpenAI background 保留时长未读到一手正文（OpenAI 侧改引官方 OpenAPI 规格）。
- Inngest 整体投递语义、AG-UI 顺序保证、Cloudflare 事件是否恰好一次：官方无陈述，**未推测**。
- 搜索中出现的若干 2026 年 NeurIPS 条目（"Trust, but Verify"、"AndroidReality"、"AndroidFlux"）
  无法通过 arXiv API 核实，已按"不编造"原则**排除**，未进入任何结论。
- Apple `AppIntents` 参数解析细节未逐页核实。

---

## 12. 现在需要你拍板的三点（带证据的版本）

1. **SA 的定位：`带记账咨询的 statechart 运行时`**，还是"一个更聪明的 agent loop"？
   证据强烈支持前者：所有框架都已经收敛到"编排层零模型调用"，继续让模型驱动每一次跃迁是逆行业趋势。
2. **对 C/C++ 之外的代价**：SA 要不要强制"工具一律异步"（C5）？
   激进方案是全异步（连 `read_file` 也返回 receipt），能统一语义但会显著抬高小工具的复杂度。
   我倾向：**默认同步，长任务与状态机能力强制异步**，并在契约里显式标注。
3. **v1 的范围**：先做"（A 异步 + C 恢复）"还是先做"（B 状态机 + D 咨询）"？
   我倾向先做 **B+D 的最小闭环**（一通假电话的状态机 + 两个 gate），因为 A、C 有大量现成范式可抄，
   而 B+D 才是真正的空白，也是"能不能立住"的判据。

---

## 附：四线一手笔记索引

| 笔记 | 内容 | 篇幅 |
|---|---|---|
| `_research/01-durable-execution-and-protocols.md` | Temporal / Restate / DBOS / Inngest / SFN / Azure / Cloudflare / MCP / Responses API / AG-UI / Anthropic | 366 行 |
| `_research/02-agent-framework-forms.md` | AutoGen / MAF / AgentScope / SK Process / LangGraph / LlamaIndex / CrewAI / ADK / OpenAI SDK / Claude SDK / Pydantic AI / Mastra | 360 行 |
| `_research/03-telephony-and-os-agents.md` | SIP / Android Telecom / Twilio / LiveKit / Pipecat / Gemini Live；AndroidWorld / Agent-SAMA / SPlanner / AppFunctions | 268 行 |
| `_research/04-formal-foundations.md` | Harel / SCXML / XState / BT / Actor / Saga / 事件溯源 / POMDP / LLM+FSM 论文 | 376 行 |
| `_research/05-dsh-baseline-gap.md` | DSH 既有原语与六个缺口（本地源码取证） | 123 行 |
