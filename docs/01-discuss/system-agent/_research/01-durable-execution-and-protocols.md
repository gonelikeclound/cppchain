# 长任务 / 异步工具调用：持久化执行与协议层调研

> 调研时间：2026-11（本文所有"最新版本"结论均以此时间点为基准）。
> 方法：优先抓取厂商一手文档（多数站点支持在 URL 后加 `.md` 取原始 Markdown），抓不到的明确标注。
> 目标：为 SystemAgent（事件驱动 + 状态机，替代 ReAct 循环）判断"发出去→立刻返回→结果晚些作为事件回来"该借谁的设计。

---

## 0. 一句话结论（先看这五条）

1. **工业界已经收敛到两个正交原语**：一个"外部唤醒凭据"（task token / awakeable / taskId / Signal）＋ 一条"持久日志"（Event History / journal / checkpoint 表）。所有长任务框架都是这两者的组合，只是日志的存储介质和重放粒度不同。
2. **"发出去立刻返回"有三种落地形态**，语义差别巨大：①**回调令牌**（调完立刻返回，外部系统拿 token 回填，如 Temporal Async Activity Completion、Step Functions `.waitForTaskToken`）；②**信箱事件**（向某个持久实体投一条消息，如 Temporal Signal、Azure external event、Cloudflare `sendEvent`）；③**任务句柄轮询**（返回 taskId，客户端 poll，如 MCP Tasks、OpenAI background mode）。
3. **投递语义没有免费午餐**：唯一"步骤级恰好一次"的路径是"状态与结果在同一个数据库事务里落盘"（DBOS 用 Postgres 事务、Azure MSSQL provider）；其余全部退化为"步骤至少一次 + 你自己幂等"。Temporal 官方明确要求 Activity 必须幂等。
4. **协议层的趋势是"传输层不保证持久，持久性上移到任务原语"**：MCP 在 `2026-07-28` 里删掉了 session、删掉了 SSE 断点续传与消息重投（`Last-Event-ID`），长任务被移出 core 变成 `io.modelcontextprotocol/tasks` 扩展，且**只提供轮询**。
5. **最能直接抄的是 Temporal Pattern 3（异步 Activity 完成后回调）+ Restate 的 keyed single-writer**：前者解决"调用立刻返回、结果从外部回填到等待中的续体"，后者解决"同一实体的事件必须串行、不同实体可并行"。

---

## 1. 问题分解：每个系统都要回答的五个问题

把背景里的三个诉求翻译成可比较的坐标系，后面对每家都按这五点填：

| 编号 | 问题 | 为什么关键 |
|---|---|---|
| Q1 | **怎么表达"发出→立刻返回→结果晚回"** | 决定我们的工具调用 API 形状（callback / signal / poll） |
| Q2 | **等待中的续体存在哪、进程重启怎么恢复** | 决定我们是"存 AST/闭包"还是"存事件日志重放" |
| Q3 | **投递语义（至少一次 / 恰好一次）与幂等怎么做** | 决定工具调用的副作用治理边界 |
| Q4 | **有没有线性化 / 顺序保证，按什么 key 排** | 决定状态机的并发模型（per-key 串行是主流答案） |
| Q5 | **代价与限制** | 决定我们不要付哪些成本（尤其"确定性重放 vs LLM 非确定性"） |

**两种实现范式（贯穿全文的分类轴）**

- **重放型（replay）**：不存续体，只存**事件历史**；恢复时把编排函数**从头再跑一遍**，用历史喂给它"你已经执行过这些、结果是这些"。代表：Temporal、Azure Durable Functions。要求编排代码**确定性**。
- **检查点型（journal / checkpoint）**：把每个 step 的**输入输出**落盘，恢复时跳到**最后一个完成的 step** 继续。代表：Restate（journal）、DBOS（Postgres 表）、Inngest（step memoization）、Cloudflare Workflows。

差别在"重放要不要重跑你的代码"：重放型对代码是强约束（LLM 调用必须挪进 Activity）；检查点型约束弱一些，但要求 step 边界清晰、非确定性逻辑必须包在 step 内。

---

## 2. Temporal —— 重放型的参照实现

**Q1 形态**：三种消息原语，语义各有明确取舍。Query = 只读（不进 Event History）；Signal = 异步写、fire-and-forget、无返回值；Update = **同步、可校验、有类型化返回值**的写，且"返回前已写入 Event History"，因此强一致。
来源：https://docs.temporal.io/encyclopedia/workflow-message-passing
官方还专门给了"请求-响应"模式（用 Update 拿同步结果）与"先 Signal 再 Query 轮询"的对比：
来源：https://docs.temporal.io/design-patterns/request-response-via-updates

**Q1 补充：真正对应"长任务异步工具调用"的是 Pattern 3 —— Async Activity Completion**。Activity 调用外部系统，把 **task token** 连同 job ID 存到自己的库里，然后**立刻返回**；外部系统完成后打回调进来，回调端用 token 调 `complete_async_activity`，Workflow "像 Activity 正常返回一样"恢复。
来源：https://docs.temporal.io/design-patterns/delayed-callback

**Q2 状态存放与恢复**：Event History 是 append-only 日志，"由 Temporal Service 持久化，因此应用状态跨崩溃存活"；Worker 收到 Workflow Task 后**重放历史**重建逻辑状态，再继续跑代码。循环是 Workflow Task → Commands → Events → 新 Task。
来源：https://docs.temporal.io/workflow-execution/event ｜ https://docs.temporal.io/encyclopedia/architecture/how-temporal-works

**Q2 的代价（也是对我们最重要的一条）**：Workflow 代码必须确定性 —— "每次执行都要以相同顺序发出相同的 Workflow API 调用"。官方给的出路写得非常直白：*"要处理 API 调用、**LLM/AI 调用**、数据库查询这类非确定性操作，把它们放进 Activity，Activity 在重放路径之外执行"*。
来源：https://docs.temporal.io/workflow-definition#deterministic-constraints

**Q3 投递语义**：Workflow 层由事件历史串行推进；**Activity 层是至少一次**——"建议 Activity 幂等，这样重试不会产生重复副作用"。消息层两种口径不同：**Update 由服务端按 Update ID 去重**（重试不会跑两次 handler），**Signal 需要你自己在 Workflow 代码里带幂等键去重**。
来源：https://docs.temporal.io/activities ｜ https://docs.temporal.io/handling-messages

**Q4 顺序**："Workflow 每次唤醒时，按消息**到达顺序**处理消息，然后再推进主方法；执行是单线程的"。所以**同一 Workflow Execution 内天然线性化**（但 handler 可交错执行，存在并发陷阱）；跨实例无顺序。Entity Workflow 模式进一步建议"一个实体一个 Workflow、用实体 ID 作 Workflow ID"，把所有状态迁移收进单一实例。
来源：https://docs.temporal.io/handling-messages ｜ https://docs.temporal.io/design-patterns/entity-workflow

**Q5 限制（硬数字）**：Event History 上限 **51,200 events / 50 MB**，10,240 events 或 10 MB 起告警；超 2000 Updates 或 10000 Signals 直接终止；单个 Execution 的 in-flight Activity / Child Workflow / Signal / 取消请求各默认上限 **2000**；单条 Update 参数与返回值受事件大小限制（**约 2 MB**），in-flight Update 上限 10。绕开办法是 Continue-As-New。
来源：https://docs.temporal.io/workflow-execution/limits ｜ https://docs.temporal.io/workflow-execution/event ｜ https://docs.temporal.io/design-patterns/request-response-via-updates

**官方对 AI agent 的叙事**：明确把"Durable Execution"定位为 agent 的运行时——"Workflow 在崩溃、网络超时或**等待人类审批数天**后自动恢复"，并给出 Agent / 管道 / agent 平台 / 训练四类场景，外加 Approval、Entity Workflow、Long-Running Activity（**心跳汇报进度**）等模式，以及十来种 agent 框架的官方集成。
来源：https://docs.temporal.io/ai ｜ https://docs.temporal.io/design-patterns/long-running-activity

---

## 3. Restate —— journal + virtual object（state machine 的最佳模板）

**Q1 形态**：三种协调原语，语义边界清晰 —— **Signal**（按 invocation ID + name，可多次解析，"每次 await 拿下一个解析值"，用于 steering/人类审批）；**Awakeable**（一次性、带生成 ID，可由外部系统 resolve/reject，官方直接类比 **task token**）；**Workflow promise**（同 workflow 内各 handler 共享，一次性）。
来源：https://docs.restate.dev/develop/ts/external-events
三者关键性质：*"Signal 可以在 invocation 开始等待之前或之后到达，Restate 会持久保存每次解析直到被接收"* —— 这就是"不需要同时在线"。

**Q2 状态存放与恢复**：每个 handler 的每个 step 都进 **journal**，"崩溃或失败时重放 journal，跳过已完成 step，从断点继续"；服务本身是**无状态**的，状态与执行历史都在 Restate Server（内嵌 KV），"随请求下发"。FaaS 上还能**挂起（suspend）**函数等待外部事件，"不用为等待时间付费"。
来源：https://docs.restate.dev/foundations/key-concepts

**Q3 投递语义**：官方口径是 invocation 级 **exactly once**——"跟踪每个 invocation 直到完成，确保无论失败都只执行一次"；RPC 层承诺"同一调用不会执行两次（No duplicates）"；外部重复请求用 **idempotency key** 去重，"重复请求返回原结果"。
来源：https://docs.restate.dev/foundations/key-concepts

**Q4 顺序（最值得抄的一条）**：**Virtual Object 按 key 单写者**——"每个 object key 同一时刻最多一个写 handler 运行，**模仿每个 key 一条队列**"，跨 key 并发；Workflow 类型则是"每个 workflow ID 只有一个 run handler（+ 并发 shared handler）"。这正是"状态机 per 实体 + 串行事件"的原生表达。
来源：https://docs.restate.dev/foundations/services

**Q5 代价**：需要跑 Restate Server（单二进制 Rust，可 HA）；journal 与状态都进服务端，服务端成为关键路径；跨系统事务仍需自己用 saga/幂等兜底。

---

## 4. DBOS —— "Postgres 即编排器"，事务边界最干净

**Q1 形态**：`DBOS.send(workflow_id, msg, topic)` / `DBOS.recv(topic, timeout)` 的消息信箱是最贴近"事件回来"的原语；另有 workflow event（工作流发布 KV 供客户端读）与 stream（流式值）。
来源：https://docs.dbos.dev/python/tutorials/workflow-communication

**Q2 状态存放与恢复**：库里只有 Postgres——"annotate 后 DBOS 把 workflow 与 step 的检查点写进 Postgres；崩溃/重启后从**最后一个完成的 step** 恢复"。**没有独立编排服务器**；生产上建议接 Conductor（控制面）做**分布式恢复**（谁崩了把它的 workflow 迁到活着的 executor）。
来源：https://docs.dbos.dev/architecture

**Q3 投递语义（表述最精确的一家）**：
- 步骤 **至少一次**，工作流**结果恰好一次**持久化（"steps get at-least-once guarantees and workflow outcomes are persisted exactly-once"）。
- 靠 **ownership token** 解决僵尸执行：每次写 checkpoint 时在同一事务内校验 token，token 不匹配就 park，等真正 owner 记录的结果可见。
- 消息："`send` 成功后目标 workflow 保证能 `recv` 到"；**从 workflow 内 send 保证恰好一次**，从普通代码 send 需传 `idempotency_key`。
来源：https://docs.dbos.dev/explanations/concurrent-executions ｜ https://docs.dbos.dev/python/tutorials/workflow-communication

**Q4 顺序**：workflow ID 全局唯一且**充当幂等键**；durable queue 提供并发上限与按 key 的并发控制（我们需要的"per-entity 串行"靠 queue 的 key 完成）。
来源：https://docs.dbos.dev/python/tutorials/workflow-tutorial#workflow-ids-and-idempotency ｜ https://docs.dbos.dev/python/tutorials/queue-tutorial

**Q5 代价**：单点变成 Postgres（换来的好处是没有任何新基础设施）；step 开销是"一次 Postgres 写，1–2 ms"，对比 Temporal 的"中央服务异步派发，几十到几百 ms"（此为 DBOS 官方对比，属**厂商自述**，采信需打折）；此外不能随意改 workflow 代码，否则触发 step nondeterminism 错误。
来源：https://docs.dbos.dev/explanations/comparing-temporal

---

## 5. Inngest —— step + `waitForEvent`，最接近"事件驱动 agent"

**Q1 形态**：`step.waitForEvent(name, {event, match, timeout})` 暂停 run 直到**匹配到某类事件**回来；`match` 是 CEL 表达式（如 `data.invoiceId`），**一个事件可同时唤醒多个等待中的 run**；`step.sleep/sleepUntil` 定时；`step.sendEvent` 发事件（fire-and-forget）；`step.invoke` 直接调另一函数。
来源：https://www.inngest.com/docs/durable-execution/primitives/step-waitforevent
官方也给了对照：需要一个 run 被直接寻址、且要求更低恢复延迟时用 `step.waitForSignal`（"resumes one run through a transactional API"）。

**Q2 状态存放与恢复**：**step memoization** —— "每个 step 的结果存一次，handler 重放时直接返回保存值"，run 在等待期间**不占用计算资源**，"waits suspend the function's compute and do not count toward concurrency"。代码在 step 之外的部分每次重放都会重跑，所以"非确定性逻辑（数据库/API 调用）必须放进 `step.run()`"。
来源：https://www.inngest.com/docs/durable-execution/concepts ｜ https://www.inngest.com/docs/durable-execution/durable-workflows

**Q3 投递语义**：**幂等是显式配置项**。事件级：发送时给 event 一个唯一 `id`，"作为幂等键在 24 小时内生效"；函数级：`idempotency` 用 CEL 表达式求值成 key，阻止同一业务键的重复执行。注意事件幂等在 **debounce/batching/函数暂停** 场景会被忽略。
来源：https://www.inngest.com/docs/durable-execution/guides-and-advanced/idempotency
（未在文档中找到"Inngest 整体投递语义 = at-least-once"的第一手明确陈述，故不填此结论。）

**Q4 顺序**：`concurrency: {limit, key}` —— 按 key 表达式分桶限流（"给每个 account 自己的限额"）；等待中的 run 不占 slot，因此"成千上万个 paused run + 10 个并发执行"是正常形态。**未找到按 key 严格 FIFO 的一手承诺**，只承诺排队后按容量开闸。
来源：https://www.inngest.com/docs/durable-execution/flow-control/concurrency

**Q5 代价**：函数托管在你自己的 HTTP `serve()` 端点（或 Connect worker），Inngest 负责协调；step 之外代码会重跑（需自律）；官方自评对比 Temporal："Inngest 保存 step 结果、重放时返回；Temporal 从头重放并受严格确定性规则约束"。
来源：https://www.inngest.com/docs/durable-execution/durable-workflows ｜ https://www.inngest.com/docs/platform-overview

---

## 6. AWS Step Functions —— task token 的教科书

**Q1 形态**：`"Resource": "...:sqs:sendMessage.waitForTaskToken"` + `"TaskToken.$": "$$.Task.Token"`。任务**立刻发出**并把 token 塞进消息里，状态机**暂停**；外部系统处理完后调 `SendTaskSuccess` / `SendTaskFailure` 带上 token，才继续下一个 state。
来源：https://docs.aws.amazon.com/step-functions/latest/dg/connect-to-resource.html

**Q2 状态存放与恢复**：托管服务内部（无需你管理）；暂停时长上限为**执行配额（最长 1 年）**；可用 `HeartbeatSeconds` 配心跳超时，"避免执行卡死"。注意：**task token 超时后会重新生成一个随机 token**，token 只能由同一 AWS 账号内的 principal 使用。
来源：同上（"Configure a Heartbeat Timeout for a Waiting Task" 节）

**Q3 投递语义（官方分得很清）**：
- **Standard Workflow = exactly-once**："任务与状态绝不会运行超过一次，除非你显式写了 `Retry`；适合编排**非幂等**动作"；同名启动会返回幂等响应、不重复启动。
- **Express（异步）= at-least-once**；**Express（同步）= at-most-once**；Express 幂等需自理。
- `.waitForTaskToken` 与 `.sync` **仅 Standard 支持**，Express 只支持 Request-Response。
来源：https://docs.aws.amazon.com/step-functions/latest/dg/choosing-workflow-type.html ｜ https://docs.aws.amazon.com/step-functions/latest/dg/connect-to-resource.html

**Q4 顺序**：按状态机的显式转移（ASL）确定，无隐式并发；同一 execution 一条历史。

**Q5 代价**：状态机是 JSON DSL，表达能力受限（不能写任意代码）；对"每次工具调用都是动态生成的"agent 场景，ASL 图的**生成复杂度**会成为负担——这是我们认为它不适合直接当 agent 内核的原因。

---

## 7. Azure Durable Functions —— external events + 重放

**Q1 形态**：`context.wait_for_external_event("Approval")` 等待，`raise_event(instance_id, eventName, data)` 从外部投事件。官方强调"**外事件是单向异步操作**，不适合发送方需要同步响应的场景"（要同步就用 sub-orchestration/HTTP 客户端轮询）。
来源：https://learn.microsoft.com/en-us/azure/azure-functions/durable/durable-functions-external-events

**Q2 状态存放与恢复**：event sourcing —— "不存当前状态，而是用 append-only 存储记录全部动作"；`await`/`yield` 时把新动作提交到存储、orchestrator 可被卸载；"有新工作时 orchestrator 醒来并**从头重执行整个函数**，重放时查历史，已执行过的 activity 直接回放结果"。因此"orchestrator 代码必须**确定性**"。
来源：https://learn.microsoft.com/en-us/azure/azure-functions/durable/durable-functions-orchestrations

**Q3 投递语义（明确写了 at-least-once）**："外事件与 Activity Function 一样是 **at-least-once** 投递，重启/扩缩/崩溃等情况可能收到重复事件，**建议事件带唯一 ID 由 orchestrator 自行去重**"；**MSSQL 存储 provider 事务化消费外事件，因此不会重复**（Azure Storage provider 则会）。若指定 instance ID 不存在，事件被**丢弃**。事件名必须两端匹配。
来源：https://learn.microsoft.com/en-us/azure/azure-functions/durable/durable-functions-external-events

**Q4 顺序**：单实例串行推进执行历史；跨实例无顺序；事件按 instance ID 路由。

**Q5 代价**：确定性重放 + 重放副作用（日志会重复打，需 replay-safe logging）；历史无限增长带来存储与重放开销（官方以 Continue-As-New 类机制/定期清理缓解）。

---

## 8. Cloudflare Workflows —— 限制写得最诚实的一家

**Q1 形态**：`step.do` / `step.sleep` / `step.sleepUntil` / **`step.waitForEvent(name, {type, timeout})`**，配合 `instance.sendEvent({type, payload})`（Workers binding）或 HTTP 投递。事件 type 匹配；**可以在 workflow 还没走到 waitForEvent 之前就发事件**——"事件会被缓冲，等 workflow 到达匹配的 waitForEvent 时交付"。`waitForEvent` 默认超时 **24 小时**，超时抛错使实例失败（要容错就 try/catch）。
来源：https://developers.cloudflare.com/workflows/build/events-and-parameters/

**Q2 状态存放**：平台侧持久化每个 step 的结果与实例状态；实例可 sleep 数天到一年。

**Q3 投递语义**：文档给出 step 级重试（每 step 最多 10,000 次），但**未找到"事件投递恰好一次"的一手承诺**；能确认的是事件缓冲与按 type 匹配。

**Q4 顺序**：单实例内 step 串行；无 per-key 排序承诺（并发控制是账号级实例数）。

**Q5 限制（一手数字，适合用来校准我们自己的预算）**：
- 单 step 结果 **1 MiB**、单事件 payload **1 MiB**；单实例可持久化状态 100 MB（Free）/ 1 GB（Paid）
- step 数 1,024（Free）/ 10,000 默认，可调到 25,000；`step.sleep` 上限 **365 天**
- 并发实例 100（Free）/ 50,000（Paid）；**waiting 状态实例不计入并发**——"可以让上百万实例同时 sleep 或等事件"
- 完成实例状态保留 3 天（Free）/ 30 天（Paid）
来源：https://developers.cloudflare.com/workflows/reference/limits/

---

## 9. MCP —— 长任务在协议层的现状（版本必须说清）

**版本基线（重要）**：`2026-07-28` 是 **current** 规范版本；`2025-11-25` 是上一版；更早为 `2025-06-18` / `2025-03-26` / `2024-11-05`。
来源：https://modelcontextprotocol.io/specification/versioning ｜ https://modelcontextprotocol.io/specification/latest

### 9.1 Progress notification（**已定稿，core**，自 `2024-11-05` 起存在）

客户端在 `_meta.progressToken` 里放一个 token，服务端用 `notifications/progress` 回报 `{progress, total?, message?}`；"`progress` 每次通知**必须递增**（即使 total 未知）"，token 必须全局唯一。这是**单向进度上报**，不携带"任务完成后的最终结果"，也不提供任何恢复能力。
来源：https://modelcontextprotocol.io/specification/2026-07-28/basic/utilities/progress ｜ 首版：https://modelcontextprotocol.io/specification/2024-11-05/basic/utilities/progress

### 9.2 Sampling（**已定稿，core**，自 `2024-11-05` 起存在；`2026-07-28` 改由 MRTR 承载）

Sampling 是"服务端请客户端代为调 LLM"。**关键变化**：`2026-07-28` 起，服务端发起的请求（`roots/list`、`sampling/createMessage`、`elicitation/create`）**不再走服务端主动请求**，改为 **Multi Round-Trip Requests（MRTR）**：服务端返回 `resultType: "input_required"` + `inputRequests`，客户端在下一次重试原请求时带 `inputResponses`。官方原话："上一个版本那套服务端主动请求的形式**不再支持**，这是 breaking change"。
来源：https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/mrtr ｜ https://modelcontextprotocol.io/specification/2026-07-28/changelog

> 对我们的含义：MRTR 是"服务端**不需要跨请求共享存储**、也不需要粘性负载均衡"才设计出来的——它把"等待中的续体"编码进了**请求参数本身**（`requestState`）。这是"无状态协议上的续体"的一个漂亮样本。

### 9.3 Elicitation（**已定稿**，`2025-06-18` 引入，`2026-07-28` 改为 MRTR 形态）

两种模式：**form**（JSON Schema 约束的结构化输入）与 **URL**（敏感交互跳转到外部 URL，"绝不能经过 MCP 客户端"）；硬性安全约束："服务端**MUST NOT** 用 form 模式索取密码、API key、token、支付凭据"。
来源：https://modelcontextprotocol.io/specification/2026-07-28/client/elicitation ｜ 首版：https://modelcontextprotocol.io/specification/2025-06-18/client/elicitation
`2026-07-28` 删除了 `2025-11-25` 引入的 `notifications/elicitation/complete` 与 `elicitationId`——因为在 MRTR 下"客户端通过重试原请求获知结果"。
来源：https://modelcontextprotocol.io/specification/2026-07-28/changelog

### 9.4 Tasks / async（**先 experimental core，后被降级为扩展**）

- **`2025-11-25`**：Tasks 进入 core，但官方标注 **experimental**——"Tasks 是**持久状态机**，承载请求的执行状态，供请求方轮询与延迟取结果，由接收方生成 task ID"。请求方在 params 里带 `task`（可含 `ttl`）、可要求 `tools/call` 以任务形式执行；服务端返回 `CreateTaskResult`（含 `taskId/pollInterval`），结果通过 **`tasks/result`（会阻塞到终态）** 或 `tasks/get` 轮询取得，`notifications/tasks/status` 为**可选**通知且"请求方 MUST NOT 依赖它"。
  来源：https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/tasks
- **`2026-07-28`**：Tasks **移出 core，成为官方扩展 `io.modelcontextprotocol/tasks`**，重新设计：去掉阻塞的 `tasks/result`、改为 `tasks/get` 轮询 + 新增 `tasks/update`（客户端→服务端**中途输入**）、去掉 `tasks/list`、**允许服务端"未经逐请求 opt-in"直接返回任务句柄**。状态机为 `working / input_required / completed / failed / cancelled`，`input_required` 时 `tasks/get` 返回 `inputRequests`（elicitation 等），客户端用 `tasks/update` 回应；取消是**协作式**的。
  来源：https://modelcontextprotocol.io/specification/2026-07-28/changelog ｜ https://modelcontextprotocol.io/extensions/tasks/overview

**Tasks 扩展自己给出的设计理由（几乎就是我们的问题陈述）**："不是每个工具调用都立刻返回……阻塞会占用连接、受中间层超时限制；**task ID 是持久句柄**，客户端断开或重启后可以继续用同一个 ID 轮询；任务带状态与状态消息；需要输入时转 `input_required`，客户端经 `tasks/update` 回答——**不需要第二条连接，也不需要服务端主动推送**。"并明确适用场景包括"包装已有 job ID 的外部系统"（"创建 job 时返回 task，job 完成时 resolve"）。
来源：https://modelcontextprotocol.io/extensions/tasks/overview

### 9.5 传输层已经不再提供任何持久性保证（关键结论）

`2026-07-28` 明确：**移除协议级 session 与 `Mcp-Session-Id`**（需要跨调用状态的服务端"使用显式、服务端铸造的句柄，作为普通工具参数传递"）；**移除 STREAMABLE HTTP 的 SSE 断点续传与消息重投**（`Last-Event-ID` 与 event ID 全删）——"响应流断了就丢了 in-flight 请求，客户端 MUST 用新请求 ID 重新发起"。
来源：https://modelcontextprotocol.io/specification/2026-07-28/changelog

> 对我们的含义：MCP 的官方立场是**协议不做持久化，持久化靠任务句柄 + 你自己存**。这反过来支持我们的判断——"事件日志 + 续体"必须是我们自己运行时的职责，不能指望协议层。

---

## 10. OpenAI Responses API —— background mode + webhooks

**Q1 形态**：请求体 `background: true`（默认 false），"让响应在后台运行"，立即拿到一个 `response` 对象，用 `GET /responses/{id}` 轮询；`POST /responses/{id}/cancel` **仅对 background 响应可用**。响应 `status` 枚举：`queued / in_progress / completed / failed / cancelled / incomplete`。
来源（一手，官方 OpenAPI 规格，因文档站对本网络 403，改引规格文件）：https://github.com/openai/openai-openapi/blob/master/openapi.yaml
（人类可读版：https://developers.openai.com/api/docs/guides/background —— 本网络返回 403，未读到正文）

**Q1 补充：webhook 是"结果晚些回来"的推送通道**。规格里有 `/webhook_endpoints`（项目级 webhook 端点 CRUD）与 `/webhook_event_types`，事件含 `response.completed / response.failed / response.incomplete`（另有 `response.in_progress`），payload 直接内嵌完整 `response` 对象。
来源：同上 openapi.yaml（`/webhook_endpoints`、`webhook_event_types`、`response_completed` 等定义）

**Q3 投递语义**：webhook 回调明确"返回 200 表示已收到，**非 200 会被重试**"——即**至少一次**，消费端必须幂等（用 `response.id` 去重）。
来源：同上

**Q2/Q4**：状态全在 OpenAI 侧（你只有 response ID + 轮询/推送），无 per-key 顺序语义；`store` 与 `previous_response_id` 决定上下文是否在服务端留存。**未找到**官方对 background 响应保留时长的第一手说明（文档站不可达），故不填具体数字。

> 注意：这是"**模型服务内部的长任务**"（一次生成跨多久），不是"**外挂工具的长任务**"——OpenAI 没有提供一个"工具调用发出后由外部系统异步回填"的原语，非内置工具的函数调用仍需你在下一轮把结果塞回 messages。这点与 MCP Tasks 形成互补。

---

## 11. AG-UI —— 事件流协议里"暂停"的做法

**Q1 形态**：AG-UI 把一次交互建模为 **run** + **事件流**（八类事件族，含 lifecycle / text / tool calls / reasoning / state / activity / subagents / passthrough）。它的"中断"不是挂在原地等，而是设计成**结束当前 run、由下一个 run 带上答案**：

> "协议**没有 run 中途的消费者通道**，所以 run 不等待：它**结束**，并说明自己在等什么；从它继续的那个 run 携带答案。"
> 需要外部输入的 run **MUST NOT** 被报告为成功，必须以 `RUN_FINISHED` 的 **interrupt outcome** 结束，携带一个或多个 `Interrupt{id, reason, message?, toolCallId?, responseSchema?}`；继续时在 input 的 **`resume`** 列表里逐条 `{interruptId, status, payload}` 回答，且"**MUST 覆盖被继续的那次 run 的所有 interrupt**（回答或显式 abandoned），省略不等于放弃"。

来源：https://docs.ag-ui.com/spec/1.0/basic/patterns/interrupt-resume

另一种"停在半路"的正规路径是 **frontend tool**：run 以**成功**结束，在 success outcome 的 **`pendingToolCallIds`** 里列出未完成的调用，答案随下一次 input 的 messages 回来。
来源：https://docs.ag-ui.com/spec/1.0/events/tool-calls

**Q2 状态存放与恢复**：**序列化**能力——把完整事件流与事件历史在便携格式间转换，用于"重载/重连后恢复聊天历史与 UI 状态"、"**附加到正在运行的 agent 继续接收事件**"、"历史压缩（compaction）以减小体积而不丢语义"；`RunStarted` 增加 `threadId`、`parentRunId`（同 thread 内分支/时间旅行）、原始 agent input。
来源：https://docs.ag-ui.com/concepts/serialization

**Q3/Q4**：**未找到** AG-UI 对投递语义（at-least-once / exactly-once）与顺序保证的一手承诺。可确认的只有结构约束：事件在 envelope 里带标识符，run 是"closed run"（关闭后只允许迟到的 `RUN_ERROR`），继续必须开新 run。
来源：https://docs.ag-ui.com/spec/1.0/basic/index ｜ https://docs.ag-ui.com/spec/1.0/events/lifecycle

> 对我们的含义：AG-UI 解决的是"**渲染层**如何承受长时间等待和人工介入"，代价是**每次暂停都销毁续体、靠输入把状态重建回来**。若我们的事件日志足够完整，这种"续体即输入"的模式反而更皮实（不需要序列化闭包）。

---

## 12. Anthropic —— Agent SDK / Managed Agents 的现状

**先说结论**：Anthropic 的**模型 API 层没有"异步工具调用"原语**（工具结果必须在下一轮 user 消息里回填）；它的工程解法是**"长生命周期进程 + 会话转录（transcript）"**，而不是 durable execution。

**Q1/Q2（Agent SDK，官方措辞最直白）**：
> "Agent SDK 派生并监管一个 `claude` CLI 子进程，它拥有 shell、工作目录和磁盘上的会话文件。托管它不像托管无状态 API 包装器：**每个运行中的 agent 都是绑定了本地状态的长生命周期进程**。"
> "容器文件系统上默认有三类 agent 状态。**它们都不 survive 容器重启、缩容或换节点。**" 恢复靠把 **transcript** 镜像到 `SessionStore`（对象存储/KV/数据库），再用 `resume`（指定 session ID，需粘性路由"把 session 固定到同一容器，直到被驱逐或容器重启"）或 `continue`。
> 官方排障条目也承认进程会在中途退出（"Claude Code process exits mid-run"）。

来源：https://code.claude.com/docs/en/agent-sdk/hosting ｜ https://code.claude.com/docs/en/agent-sdk/sessions ｜ https://code.claude.com/docs/en/agent-sdk/session-storage

**会话 ≠ 续体**：官方明确"**sessions 持久化的是对话，不是文件系统**"，文件改动要用 file checkpointing 另存。也就是说**中途的工具执行不持久**——重启后只能"重开对话"，不能"从第 3 个工具调用继续"。
来源：https://code.claude.com/docs/en/agent-sdk/sessions

**Q1 补充（让 agent 边跑边被干预）**：Streaming Input 模式让 agent 成为"长生命周期进程"，可接收**排队消息**（按序处理 + 可打断）、处理权限请求、实时中断；single mode 则是一次性 `query()`。这是"不阻塞思考"的另一条路——**输入流不停**，而不是"工具异步返回"。
来源：https://code.claude.com/docs/en/agent-sdk/streaming-vs-single-mode

**Managed Agents（托管形态）**：官方文档索引里存在 **Managed Agents** 一节，含 sessions、session threads、session event stream、event deltas、poll for work（`/api/beta/environments/work/poll`）、pause/unpause deployment、webhooks、scheduled deployments、multiagent orchestration 等页面。
来源（索引）：https://platform.claude.com/docs/en/managed-agents/overview
**必须标注**：本网络访问 `platform.claude.com` 与 `docs.claude.com` 的正文被区域拦截（HTTP 200 但返回 "App unavailable in region"），**以上仅为索引条目，未读到正文，不据此下结论**。

**`pause_turn`**：检索可见 `stop_reason: pause_turn` 出现在官方"Stop reasons and fallback"页（用于服务端工具长时运行后要求继续该轮），但该页正文同样被拦截，**未读到一手正文，仅作存在性提示**。
来源（未验证正文）：https://platform.claude.com/docs/en/build-with-claude/handling-stop-reasons

---

## 13. 横向对比表

| 系统 | Q1 异步形态 | Q2 续体/状态在哪 | Q3 投递语义 | Q4 顺序/线性化 | Q5 主要代价 |
|---|---|---|---|---|---|
| Temporal | Signal / Update / **Async Activity Completion(task token)** | Event History（服务端），**重放**工作流代码 | Workflow 串行；**Activity 至少一次**，需幂等；Update 按 Update ID 去重，Signal 自建幂等键 | 同一 Workflow ID 单线程、按到达顺序 | 确定性重放（LLM 必须进 Activity）；51,200 events / 50 MB 上限 |
| Restate | Signal / **Awakeable(≈task token)** / Workflow promise | journal + 内嵌 KV，服务端；服务无状态，FaaS 可 suspend | invocation exactly-once；idempotency key 去重 | **Virtual Object 按 key 单写者（每 key 一条队列）** | 需跑 Restate Server，服务端在关键路径 |
| DBOS | `send`/`recv` 信箱、workflow event | **Postgres 表（checkpoint）**，从最后完成的 step 恢复 | **step 至少一次 + 结果恰好一次**；ownership token 防僵尸；send 内传 idempotency_key | workflow ID 唯一=幂等键；queue 按 key 限并发 | 依赖 Postgres；改代码触发 nondeterminism 错误 |
| Inngest | **`step.waitForEvent`（按 data 字段 match，可一事件唤醒多 run）** | step 结果 memoization；等待不占算力 | 幂等是显式配置（event id 24h / function CEL key）；整体语义未见一手声明 | concurrency key 分桶；无严格 FIFO 承诺 | 函数托管在你的 HTTP 端点上；step 外代码会重跑 |
| AWS Step Funct. | **`.waitForTaskToken` + SendTaskSuccess/Failure** | 托管内部；暂停至配额（≤1 年），可配 HeartbeatSeconds | **Standard 恰好一次**；Express 至少一次/至多一次；token 重发会换新 | ASL 显式转移 | 状态机 DSL 表达力有限，动态工具图生成麻烦 |
| Azure Durable | `wait_for_external_event` / `raise_event`（单向异步） | **event sourcing 历史**，**从头重执行**；必须确定性 | **外事件至少一次**，需自带去重 ID；MSSQL provider 事务化=无重复；无实例则丢弃 | 单实例串行 | 重放副作用（日志重复）、历史增长 |
| Cloudflare WF | **`step.waitForEvent` + `instance.sendEvent`（可提前发、缓冲交付）** | 平台侧 step 结果持久化 | 每 step 重试（≤10,000）；事件恰好一次**未见一手声明** | 单实例串行 | 1 MiB/step、1 MiB/event、10k step、保留 30 天 |
| MCP | **progress（已定稿）**；**Tasks 扩展（`2026-07-28`，轮询 + `tasks/update`）** | **协议不持久**；task ID 是持久句柄，服务端自存 | 未见一手投递语义承诺；取消是协作式 | 无 | 传输层已删 session 与 SSE 续传；Tasks 仅扩展、非 core |
| OpenAI Responses | **`background: true` + 轮询 + webhook** | OpenAI 侧（response ID） | webhook **非 200 会重试 → 至少一次** | 无 per-key 顺序 | 只覆盖"模型侧长任务"，非"外挂工具异步回填" |
| AG-UI | **interrupt 结束 run + resume 列表**；frontend tool 用 `pendingToolCallIds` | **事件流序列化**（恢复/分支/压缩） | 未见一手承诺 | 未见一手承诺 | 每次暂停销毁续体，靠输入重建 |
| Anthropic | Streaming Input（可排队/打断）；Managed Agents 有 poll/webhook（正文未读到） | **本地磁盘 transcript**（不 survive 重启）；`SessionStore` 镜像 + resume | 未见一手承诺 | 粘性路由把 session 固定到容器 | 长生命周期进程绑本地状态；工具执行不持久 |

---

## 14. 共性结论（写给我们自己的 ADR 用）

1. **"续体"几乎没人真的序列化闭包**。能序列化的是**数据**：事件日志（Temporal/Azure）、step 结果表（DBOS/Inngest/Cloudflare）、journal（Restate）。差别只是"恢复时要不要重放代码"。
2. **重放对 LLM 是硬伤，但已被官方点名解决**：Temporal 直接写"LLM/AI 调用放 Activity"，Azure 要求 orchestrator 确定性。**结论：凡是会调 LLM 的步骤，必须作为"有副作用的 step"被记录，不能进可重放主体。**
3. **"恰好一次"只在事务边界内成立**。DBOS（Postgres 事务）与 Azure MSSQL provider 是唯二敢说"不重复"的实现；其余全是"至少一次 + 幂等键"。幂等键的粒度有三种：请求级（Restate idempotency key）、事件级（Inngest event id）、函数/工作流级（DBOS workflow ID、Temporal Update ID）。
4. **per-key 单写者是共识答案**：Restate virtual object（每 key 一条队列）、Temporal 一个实体一个 Workflow、DBOS queue key、Inngest concurrency key。**跨 key 并行、同 key 串行**——这正是"打电话"这类长生命周期状态机需要的模型。
5. **等待必须"卸载"**：主流实现都把"等待中"从计算资源上摘下来（Restate suspend、Inngest waits 不占并发、Cloudflare waiting 不计并发、Temporal Activity 只留 `ActivityTaskScheduled` 一条事件）。**"等待不花钱/不占槽"是设计长任务系统的第一条性能预算。**
6. **协议层的方向是"句柄化"而非"会话化"**：MCP `2026-07-28` 删 session、删 SSE 续传，改为"服务端铸造的显式句柄"；Tasks 用 taskId 续命；Cloudflare 用 workflow instance ID；OpenAI 用 response ID。**"一次连接"不再是状态载体。**

---

## 15. 对 SystemAgent 的可借鉴点

1. **【借鉴 Async Activity Completion】工具调用协议加"令牌回填"两段式**：发出调用时生成 `call_id + token`，立即返回 `pending` 事件；结果回来时以 `tool_result(token, payload)` 事件回填到**等待中的续体**。对应缺口："工具调用必须阻塞思考"，且我们需要一个**与具体框架无关的唤醒凭据**。
   参考：https://docs.temporal.io/design-patterns/delayed-callback

2. **【借鉴 Restate awakeable】令牌必须"先到先存"**：结果早于等待到达时不能丢，要落进 per-token 信箱，等续体来取。对应缺口：事件驱动系统里"结果比等待先到"是常态，不能靠内存里的 promise 兜。
   参考：https://docs.restate.dev/develop/ts/external-events

3. **【借鉴 Virtual Object 单写者】状态机实例按 key 串行、跨 key 并行**：每个"打电话"会话一个 key，同一 key 的事件严格串行消费，跨 key 自由并发。对应缺口：我们需要既保证"拨号→振铃→接通"的顺序，又不让不同会话互相阻塞。
   参考：https://docs.restate.dev/foundations/services

4. **【借鉴 DBOS 事务边界】把"状态迁移 + 结果落盘"放进同一个 DB 事务**：这正是我们能做到"步骤级恰好一次"的唯一免费路径；同时用 ownership token/fencing 防僵尸执行。对应缺口：进程被杀重启时，状态与副作用可能不一致。
   参考：https://docs.dbos.dev/explanations/concurrent-executions

5. **【借鉴 Temporal 确定性的反面】显式区分"纯判定层"与"副作用 step 层"**：事件驱动的状态机决策必须是纯函数（可重放、可回测），所有 LLM 调用与工具执行都是被记录的 step。对应缺口："LLM 非确定性"与"重放"天然冲突，混在一层就永远无法恢复。
   参考：https://docs.temporal.io/workflow-definition#deterministic-constraints

6. **【借鉴 Inngest waitForEvent 的 match】等待条件写成"事件模式 + 匹配表达式 + 超时"声明式三元组**，而不是代码里的分支；一个事件可同时唤醒多个等待者。对应缺口：我们的等待目前是隐式的，无法审计"到底在等什么、等多久、谁来满足"。
   参考：https://www.inngest.com/docs/durable-execution/primitives/step-waitforevent

7. **【借鉴 Cloudflare 的预算表】先给等待、载荷、步数、保留期定硬上限**：1 MiB/事件、10k step、30 天保留、waiting 不占并发——这些数字直接决定存储与恢复成本。对应缺口：我们没有"事件日志膨胀"的治理口径（参考 Temporal 51,200 事件/50 MB 的终止线）。
   参考：https://developers.cloudflare.com/workflows/reference/limits/ ｜ https://docs.temporal.io/workflow-execution/limits

8. **【借鉴 MCP Tasks 与 AG-UI 的对立设计】对外暴露"任务句柄"、对内保留"续体即输入"两条腿**：MCP 用 taskId + 轮询解决跨连接存活（适合工具服务侧）；AG-UI 用"结束 run + resume 列表"解决渲染层恢复（续体由输入重建）。我们对外用前者，对内可用后者降低序列化难度。对应缺口：跨进程/跨设备恢复时，我们没有既省事又不丢语义的表示法。
   参考：https://modelcontextprotocol.io/extensions/tasks/overview ｜ https://docs.ag-ui.com/spec/1.0/basic/patterns/interrupt-resume

---

## 附：未能取得一手来源的项（明确留空，勿推测）

- **OpenAI background 响应的保留时长 / 并发配额**：`developers.openai.com` 与 `platform.openai.com` 对本网络均返回 403，仅从官方 OpenAPI 规格确认字段与枚举。
- **Anthropic Managed Agents 的正文机制**（session event stream、poll for work、webhooks 的具体语义）：`platform.claude.com` 正文被区域拦截（返回 "App unavailable in region"），只有官方 `llms.txt` 索引条目可证其存在。
- **Anthropic `pause_turn` 的完整语义**：同上，仅有检索证据。
- **Inngest 整体投递语义是否 at-least-once**：官方文档未见明确陈述。
- **AG-UI 的投递语义与顺序保证**：1.0 规范页未做此类承诺。
- **云厂商（Step Functions / Cloudflare）事件投递是否恰好一次**：仅 Azure 明确写了 at-least-once。

---

*一手文档抓取方式备忘（便于复核）：Temporal `docs.temporal.io/<path>.md`；Restate `docs.restate.dev/<path>.md`；DBOS `docs.dbos.dev/<path>.md`；Inngest `www.inngest.com/docs-markdown/<path>`；AWS `docs.aws.amazon.com/.../<page>.md`；Azure 需 `-H "Accept: text/markdown"`；Cloudflare `developers.cloudflare.com/workflows/.../index.md`；MCP / AG-UI 直接加 `.md`。*
