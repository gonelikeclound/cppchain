# Agent 框架中「事件驱动 / 状态机 / 图编排」的业界现状

> 调研目标：为「事件驱动 + 状态机驱动」的系统级 Agent（SystemAgent）寻找可借鉴形态，替代传统 ReAct 循环。诉求三条：(1) 工具调用立刻返回、结果以事件形式晚到；(2) 某些能力本身是长生命周期状态机；(3) 进程可重启并恢复等待中的续体。
>
> 方法：只取一手来源。官方文档站优先取 `.md` / `llms.txt` / `toc.json` 变体（`curl` 抓取，HTTP 200）；文档缺失处直接读发行包源码（PyPI wheel 内 `.py`）。文档站不可达或路径为站点 fallback 的情况明确标注「未找到一手来源」。凡加载在关键判断上的英文原句用 `` `...` `` 保留。
>
> 覆盖：AutoGen v0.4+（core/agentchat）、Microsoft Agent Framework、AgentScope、Semantic Kernel Process Framework、LangGraph、LlamaIndex Workflows、CrewAI Flows、Google ADK、OpenAI Agents SDK、Anthropic Claude Agent SDK、Pydantic AI / pydantic-graph、Mastra Workflows；另加一节 durable execution 基座（Restate / DBOS）作为旁证。

---

## 0. 先立口径：这个问题的四个隐藏维度

不先定义清楚，「是不是状态机」这种判断会全面失真。后面每个框架都用同一套口径回答。

**维度一：控制流单元是什么。** turn（一次模型往返）/ step（一次函数执行）/ node（图节点）/ executor（图节点 + 邮箱）/ message（消息投递）/ event（事件派发）/ process（进程容器）。这个选择直接决定了「暂停点」落在哪里。

**维度二：「稍后回来」是哪种「回来」。** 这是本次调研最关键的区分，业界四种实现差别极大：

- **A. 续体捕获（continuation capture）**：真正把执行栈留在原地，事件回来从断点那一行继续跑。等价于语言级 coroutine/续体被持久化。
- **B. 快照 + 确定性重放（replay-based durable execution）**：记 journal / checkpoint，重启后**从头重跑**到断点，已记账的步骤用记录值代替而不重做。Temporal、DBOS、Restate、LangGraph、Mastra、LlamaIndex 都在这一档。
- **C. 只支持「审批中断」**：暂停点被限定为「人批准/拒绝」，不是通用异步能力。
- **D. 无**：工具同步阻塞，一次 turn 内等到底。

**维度三：持久化粒度。** 消息历史 → step 输出 → 完整进程上下文（含在途事件与邮箱）。粒度越细，「重启恢复」越接近真续体。

**维度四：确定性路径是否经过模型。** 编排层（分支、循环、重试、状态迁移）是否被强制要求经过一次 LLM 调用。

**贯穿全文的一句判据**：B 档方案成立的前提是「断点之前的代码必须可重复执行，副作用必须幂等或被 memoization 覆盖」。任何把 LLM 调用放在暂停点之前的写法，都会在重放时重付一次模型费用——除非该调用被显式包进可记账单元（LangGraph 的 `@task`、Restate 的 `ctx.run`、Temporal 的 activity）。**这条约束是「长生命周期状态机 + LLM 咨询」真正难的地方，也是本次调研的核心发现。**

---

## 1. AutoGen v0.4+ / Microsoft Agent Framework

AutoGen v0.4 是同代唯一把「actor + 运行时」做成一等公民的框架，其分层是理解后续所有 Microsoft 侧设计的钥匙。

### 1.1 AutoGen（`autogen-core` / `autogen-agentchat`）

**分层**：`autogen-core` 是 actor 运行时层——「a **runtime** environment, which facilitates communication between agents, manages their identities and lifecycles, and enforce security and privacy boundaries」。两种运行时：`SingleThreadedAgentRuntime`（单进程）与 distributed runtime（host servicer + workers + gateways），切换不改变 agent 实现。`autogen-agentchat` 是建立在其上的对话抽象（`AssistantAgent` / `RoundRobinGroupChat` 等）。
来源：https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/architecture.html

1. **控制流单元**：**消息**。agent 是 `RoutedAgent`，用 `@message_handler` 按消息类型路由（`CantHandleException` 表示不接）。消息是纯数据：「Messages are purely data, and should not contain any logic.」
来源：https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/message-and-communication.html
2. **工具/异步与「稍后回来」**：**有真实的发布订阅，但没有「等这个回程结果」的配对原语**。投递分 direct messaging（一对一，需 agent ID）与 broadcast（一对多，不需 ID），broadcast 通过 **topic + subscription** 实现：「In event-driven workflows, agents do not always know who will handle their messages」。所以「工具调用立刻返回、结果晚到」可以表达成：agent 抛出一条消息给某个 topic，谁订阅谁处理，发起方不阻塞——但**回程要靠另一个消息 + 自己维护关联状态**。可用的替代手段是 `ClosureAgent.register_closure(runtime, type, cb, subscriptions=...)` + `ClosureContext`，把结果塞进外部 `asyncio.Queue`（「to collect and process the results programmatically」）。没有 future / promise / `request_info` 这类一次性挂起-恢复对象。
来源：https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/topic-and-subscription.html
3. **持久化与重启恢复**：**只有会话边界级，不是续体级——而且官方自己说破了**。`await assistant_agent.save_state()` 产出形如 `{'type': 'AssistantAgentState', 'version': '1.0.0', 'llm_messages': [...]}` 的字典，`load_state()` 灌回；Team 侧 `TeamState` 存的是各 agent state + 队伍游标（`RoundRobinManagerState = {message_thread, current_turn, next_speaker_index}`）。**最能定性的一句来自官方对 `UserProxyAgent` 的警告**：它「blocks the execution of the team until the user provides feedback ... and put the team in an **unstable state that cannot be saved or resumed**」。因此框架推荐的 HITL 姿势就是「结束本次 run，下一次 `run()` 再喂回去」——**即官方承认「阻塞等待外部输入的中间态」不可持久化**。在途 handler / 未完成的工具调用不在 state 结构里。
来源：https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/state.html
4. **模型在关键路径的次数 / 确定性路径**：**取决于你用哪一层**。`autogen-core` 的 `RoutedAgent` handler 是纯代码，零模型调用，可写完全确定性的消息状态机；`autogen-agentchat` 每个 agent 一次 `on_messages` = 一次模型往返，群聊则是 N 次。官方为确定性控制提供的显式图是 **`GraphFlow` + `DiGraphBuilder`**（`add_node`/`add_edge`，支持顺序 / 并行 fan-out / 条件分支 / 循环），文档定位为「when your task requires **deterministic control**, conditional branching...」——但标注为 **experimental**。这是「确定性核心 + 模型外壳」分层最干净的一例。
来源：https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/graph-flow.html
5. **谁管状态**：**框架管 actor 生命周期与消息路由，模型层状态由 agentchat 序列化**。模型不拥有状态，只看到被塞进 `llm_messages` 的部分。

**定性**：actor + 消息 + topic 订阅是最贴合「事件驱动」的骨架，但**它的时间语义是「不阻塞」，不是「可持久挂起」**——重启恢复只到消息历史。

### 1.2 Microsoft Agent Framework（AutoGen + Semantic Kernel 的后继）

**执行模型**：「A Workflow ties **executors** and **edges** together into a directed graph」，`WorkflowBuilder` 链式构图；执行模型自述为「a **modified Pregel execution model** — a **Bulk Synchronous Parallel (BSP)** approach with superstep-based processing」，且把屏障写成了核心特征：「The most important characteristic is the **synchronization barrier** between supersteps. Triggered executors can run concurrently within a superstep, but the workflow doesn't advance to the next superstep until they finish.」——**与 LangGraph 的 Pregel-BSP 是同一套模型，两家独立收敛。**
来源：https://learn.microsoft.com/en-us/agent-framework/concepts/workflows/builder-and-execution

1. **控制流单元**：**executor**（收输入、产输出的处理单元）+ **edge**；批处理单位是 **superstep**。事件体系完备且带类型判别：`WorkflowOutputEvent`、`AgentResponseEvent`、`SuperStepStartedEvent/CompletedEvent`、`RequestInfoEvent`（「A request is issued」，文档明说批准类工具的 `ToolApprovalRequestContent` 就走它）。
来源：https://learn.microsoft.com/en-us/agent-framework/concepts/workflows/events
2. **工具/异步与「稍后回来」**：**`request_info` / `RequestPort` 是这批框架里语义最细的挂起原语，而且明确「等待不烧 token」**。Python 侧 `await ctx.request_info(request_data=..., response_type=...)` + `@response_handler`（「The framework matches incoming responses to the correct handler based on the **type annotations**」）；C# 侧 `RequestPort.Create<TReq,TResp>("Name")` → 发出 `RequestInfoEvent` → `handle.SendResponseAsync(...)`。语义：「A `RequestPort` defines a **typed request/response channel** between the workflow and the outside world. When an executor reaches a request port, the workflow **pauses and emits an external request event**. The workflow **resumes when an external response is provided**」；Go 侧 `WatchUntilHalt` 还能「return at a pause」，且「A run can **halt while waiting for external input**, not only when its work is complete.」最关键的资源语义：**「Pause for human input or external events without consuming compute or model tokens while waiting」**——这是「工具立刻返回、结果晚到」在成本维度上好过 ADK 的地方（ADK 恢复时还要多一次模型调用）。
来源：https://learn.microsoft.com/en-us/agent-framework/workflows/human-in-the-loop
3. **持久化与重启恢复**：**这批框架里最完整的 checkpoint 语义**。检查点在「the end of each superstep, after all executors in that superstep have completed」，捕获内容明确列出：「All **pending messages** in the workflow for the next superstep」「**Pending requests and responses**」。Python 1.13.0 起额外在首个 superstep 前写 entry checkpoint、在请求回复送达时再写一个，使「the complete workflow run **replayable**」。存储三档：`InMemoryCheckpointStorage` / `FileCheckpointStorage` / `CosmosCheckpointStorage`（跨进程、分布式生产用）。恢复必须保持拓扑与 executor 身份稳定（agent 需固定 `ChatClientAgentOptions.Id`/`Name`），否则 `resume fails`。
来源：https://learn.microsoft.com/en-us/agent-framework/workflows/checkpoints
托管级持久化另有 **Durable Extension**（`agent-framework-durabletask` / Durable Task Scheduler），承诺「Persist state automatically across requests and worker executions / **Resume after failures without losing conversation context or repeating completed work** / Scale across distributed, stateless workers」——把 checkpoint 与「无状态 worker 横向扩展」绑定，这是自研时最易忽略的一层。
来源：https://learn.microsoft.com/en-us/agent-framework/hosting/durable-extension
4. **模型在关键路径**：executor 可以是纯代码 executor（文档示例刻意用「builds a text-processing workflow **without requiring a model or credentials**」），agent 只是 executor 的一种。确定性编排零模型调用。含 agent executor 时，每次 executor 激活 ≈ 1 次模型往返；同一 superstep 内 fan-out 的 N 个 executor 并发 → **N 个并发模型调用后统一等在屏障处**。另注意默认值差异：MAF 的 `Agent`「is **multi-turn by default** and keeps invoking tools until it can return a final answer」，而 AutoGen 的 `AssistantAgent` 是「**single-turn** unless you increase `max_tool_iterations`」——这直接决定「一次调用里有几次模型往返」。
5. **谁管状态**：**框架，且状态是一等公民**。`WorkflowContext.set_state()` / `get_state()` 操作 workflow 级共享状态，并有明确的可见性规则：「The executor that calls `QueueStateUpdateAsync` can read the updated value immediately in the same handler. **Other executors see that update starting in the next superstep**」——即状态更新也是 barrier 语义的一部分。加上「All pending messages」「Pending requests and responses」一并进检查点，这是「框架替模型管状态」的最强形态。
来源：https://learn.microsoft.com/en-us/agent-framework/concepts/workflows/state

**定性**：**actor 式的 pending-message 持久化 + BSP 式 superstep 检查点**，在「进程重启后接着跑」这件事上，是商业框架里语义最完整的。注意它仍是 **B 档（replay）**，不是 A 档（续体）。

---

## 2. AgentScope（v1.0）

1. **控制流单元**：**agent 方法 / pipeline 组合子**。`sequential_pipeline`、`fanout_pipeline` 是函数式与类式两套；`MsgHub` 是「a message hub for broadcasting messages among multiple agents」，且是 **async context manager**（`async with MsgHub(...)`），进程内所有参与者在 hub 作用域内自动广播、无需手工转发，进入时可携带 announcement 广播一次。
来源：https://doc.agentscope.io/tutorial/task_pipeline.html
2. **工具/异步与「稍后回来」**：**`MsgHub` 是广播作用域，不是可持久挂起**。它解决「多 agent 互相看得见」的问题（省掉手工 send/reply），但没有「抛出一个请求、等一个晚到结果、期间进程可以退出」的原语。v1.0 里 `interrupt()` 的语义是 **asyncio cancel**，不是挂起；唯一的「稍后回来」是**重入一次 `await agent(msg)`**（即靠调用方在 run 之间自己续）。
3. **持久化与重启恢复**：**有 state/session 管理，粒度 = 对象快照**。`StateModule` 提供 `register_state` / `state_dict` / `load_state_dict`，支持嵌套序列化与自定义 `custom_to_json`/`custom_from_json`；`JSONSession` 做 session 级落盘。官方定义：「the "state" refers to the agent status in the running application, including its current system prompt, memory, context, equipped tools」。→ **保存的是 agent 的对象状态，不是在途执行的续体**。
来源：https://doc.agentscope.io/tutorial/task_state.html
4. **模型在关键路径**：pipeline / `MsgHub` 层零模型调用（确定性组合）；`ReActAgent` 内部是模型循环。分层与 AutoGen 类似。
5. **谁管状态**：框架管 agent 对象状态与 session；模型不持有。

**关于「actor 化」的一个重要修正**：AgentScope v0.1.x 确实有分布式 actor 模式——`to_dist` 「Using `to_dist` function to run the agent in **distributed mode**!」，`msghub` 记为小写上下文管理器配 `sequential_pipeline`。但**v1.0 稳定文档的搜索索引里 `to_dist` / `distributed` 命中数为 0**，即分布式 actor 模式已从现行文档消失（v0.1.x 文档站 `/v0/en` 已死链，只能引 v0.1.6 仓库 README）。→ **今天看 AgentScope，它已不再是「actor 框架」**，而是一个 async 组合式 agent 库。是否有替代（A2A？）**未找到一手来源**。

**定性**：`MsgHub` 是一个**同步作用域的接收者集合**，比真正的 pub-sub broker 弱一档；持久化面向「会话恢复」，不面向「中断续跑」。

---

## 3. Semantic Kernel Process Framework：它到底是不是状态机？

这是本次调研里最容易被营销词误导的一个，需要拆开看边界。

**文档的自述**：「utilizing an **event-driven model** to manage workflow execution」；核心概念是 Process / Step / Pattern；Pattern 有 Fan In / Fan Out / Cycle / Map Reduce。
来源：https://learn.microsoft.com/en-us/semantic-kernel/frameworks/process/process-framework

**源码才是判据**（PyPI `semantic-kernel==1.46.0`，`semantic_kernel/processes/`）：

1. **控制流单元**：**step + event**，事件是一等类型。`KernelProcessStepContext.emit_event(...)` 是 step 推事件出去的唯一入口；step 之间靠 `KernelProcessEvent` / `ProcessEvent`（带 `namespace` 拼接 id、`visibility` 区分 Internal/Public）挂边。step 由事件触发，不是被顺序调用。
2. **工具/异步与「稍后回来」**：**有，而且是这批框架里少见的「事件循环常驻」形态**。`LocalKernelProcess.start(keep_alive=True)` 会 `asyncio.create_task(internal_execute(...))`，主循环形如 `for _ in range(max_supersteps): ... if not messages_to_process and (not keep_alive or external_event_queue.empty()): break` —— 即**没有内部消息时，只要 keep_alive 就继续等外部事件**；外部可用 `send_message(KernelProcessEvent)` 随时注入。`run_once()` 则是「跑一轮到底」的批处理语义。注意它也用了 **superstep** 这个词。
3. **持久化与重启恢复**：**这里有一个必须讲清的分裂——两套运行时，能力天差地别。**
   - **Local runtime：无持久化。** `local_process.py` 全篇没有 state manager / `save_state`，状态在内存（队列）里，进程死即全丢。
   - **Dapr runtime：真持久化。** `dapr_runtime/actors/step_actor.py` 走 Dapr actor 的 `state_manager`：`try_add_state(ActorStateKeys.StepInfoState...)` + `save_state()`，并把**待处理入站消息**一并落盘（`StepIncomingMessagesState`），激活时 `_on_activate()` 用 `try_get_state` 读回。也就是说「重启恢复等待中的**消息**」在 Dapr 模式下是成立的——但恢复的是**邮箱**，不是函数栈。
   - 结构侧有 `KernelProcessState` / `KernelProcessStepState`（pydantic），但那是**状态容器与元信息**，不是可回放的动作日志。

**它是不是状态机？** —— **它是「事件驱动的 step 数据流 + 每步自带状态存储」，不是「显式状态迁移表的状态机」。** 关键证据有三点：`KernelProcessStep` 的边由事件订阅（`OnEvent` / `OnFunctionResult` / `SendOutputTo`）构成，没有「当前状态 ∈ S，事件 e → 下一状态」的显式迁移定义；没有 `current_state` 这类全局状态变量；Cycle 只是「靠输入输出继续循环」这一 pattern，而非显式自环迁移。文档说 Process 「Stateful: Supports querying information such as tracking status and percent completion, as well as the ability to **pause and resume**」，但**这个 pause/resume 指的是「可以停下来、之后继续喂事件」，不是「快照后从断点续执行」**（Local runtime 无持久化即为反证）。
来源：https://learn.microsoft.com/en-us/semantic-kernel/frameworks/process/process-core-components

4. **模型在关键路径**：**由你决定，可以是 0**。step 调用的是 `KernelFunction`（「including Code-only, API calls, AI Agents, and Human-in-the-loop」），纯代码 step 完全不碰模型；框架不强制任何一步过模型。
5. **谁管状态**：**框架管 step 的 state 容器与（Dapr 下）邮箱**；模型不持有。这一点上 SK Process 其实比多数 agent 框架更接近「编排层不依赖模型」。

**边界总结**：SK Process 提供了「事件驱动 + 常驻事件循环 + 每步状态 + （Dapr 下）邮箱持久化」，缺的是「显式状态机定义」和「续体恢复」。**它是本报告中最接近我们想要形态的一例，但它的 durable 能力绑在 Dapr 上，Local runtime 只是内存版原型。**

---

## 4. LangGraph

**执行模型（一手且明确）**：`Pregel` 是运行时，「combines **actors and channels**」；执行按 **Pregel 算法 / Bulk Synchronous Parallel** 组织，每步三阶段：**Plan**（选出本步要跑的 actor）→ **Execution**（并行执行，此阶段 channel 更新对 actor 不可见）→ **Update**（统一写回 channel），循环直到没有 actor 被选中。channel 有 `LastValue`、`Topic`（PubSub）、`BinaryOperatorAggregate` 等。
来源：https://docs.langchain.com/oss/python/langgraph/pregel

1. **控制流单元**：**superstep / node**。每个 superstep 边界产生一个 checkpoint；「A super-step is a single "tick" of the graph where all nodes scheduled for that step execute (potentially in parallel)」。
来源：https://docs.langchain.com/oss/python/langgraph/checkpointers
2. **工具/异步与「稍后回来」**：**`interrupt()` + `Command(resume=...)`**，但语义必须看仔细。`interrupt()` 在节点内被调用时保存状态、「waits indefinitely until you resume execution」，恢复时用 `Command(resume=value)`，该值成为 `interrupt()` 的返回值。**官方明确的代价是**：「The node **restarts from the beginning** of the node where the `interrupt()` was called when resumed, so any code before the `interrupt()` runs again」。→ **这不是续体捕获，是节点级重放**。同时 `interrupt()` 走 `GraphBubbleUp`，「bypassing both retry policies and error handlers」。另支持 `response_schema`（Pydantic/TypedDict/dataclass）给恢复值做校验，以及并行多 interrupt 的 id→value 映射恢复。
来源：https://docs.langchain.com/oss/python/langgraph/interrupts
3. **持久化与重启恢复**：**这批框架里最工程化的一套，但仍受「重放」约束**。checkpoint 存于 superstep 边界；另有 node（task）级 writes 落 `checkpoint_writes`，因此「if one or more nodes fail at a given superstep, you can restart your graph from the last successful step」，成功的节点不重跑。durability 模式可调（`"exit"` / `"async"` / 同步），文档诚实指出 `"exit"` 下「a crash during the run leaves no record of the thread」、`"async"` 下「there is a small risk that LangGraph does not write checkpoints if the process crashes」，并给出对账建议（在自有 store 里先写 outbox 行以区分「从未接受」与「已接受但丢失」）。
来源：https://docs.langchain.com/oss/python/langgraph/checkpointers
**关键机制（对本设计最有价值）**：官方 durable execution 文档直言「the code does **NOT** resume from the **same line of code** where execution stopped; instead, it will identify an appropriate starting point ... the workflow will **replay all steps** from the starting point」。解药是把副作用/非确定性操作包进 **`task`**：「their results are retrieved from the persistence layer」，Functional API 侧的原话是「because the result of the `writeEssay` task was already saved, the task result will be **loaded from the checkpoint instead of being recomputed**」。Graph API 的 node 内同样可以使用 task。
来源：https://github.com/langchain-ai/langgraph/blob/8c4904bee93cc4124f11132c7c3f4e747dbce3c0/docs/docs/concepts/durable_execution.md 、 https://docs.langchain.com/oss/python/langgraph/functional-api
4. **模型在关键路径**：标准 agent 是「LLM 一次 / 一个 superstep」的循环。但 LangGraph 明确区分两类形态：「Workflows have **predetermined code paths**」vs「Agents are dynamic and define their own processes and tool usage」——**确定性编排可以完全不经过模型**，`ToolNode` 只是可选预制件。
来源：https://docs.langchain.com/oss/python/langgraph/workflows-agents
5. **谁管状态**：**框架管**（typed state + reducers + checkpoint），模型只看到被拼进 prompt 的那部分。reducer 必须是纯函数、「not baked into the persisted writes」——否则重放会产生不同结果。

**定性**：**BSP 图执行 + 可调 durability + 节点级重放**。它是「durable execution」在 agent 框架里落地最完整的一例，也最诚实地说破了「重放」与「续体」的区别。另有一条运维级能力值得抄：`RunControl.request_drain()` + `GraphDrained`，在 superstep 边界优雅停机后 `invoke(None, config)` 续跑。
来源：https://docs.langchain.com/oss/python/langgraph/fault-tolerance

---

## 5. LlamaIndex Workflows

**控制流单元**：**step + typed event**，边由类型注解推导，没有 DAG 对象。「A workflow is an event-driven, step-based way to control the execution flow of an application」。
来源：https://developers.llamaindex.ai/python/llamaagents/workflows/

1. **控制流单元**：step 收到 event、做事、返回另一个 event；分支 = 返回不同 event 类型，循环 = 返回被更早 step 消费的类型，并发 = 返回 `list[Event]` 配一个吃 `list[Event]` 的 step。另有 `ctx.send_event(...)`（立刻发出、可 `return None`，代价「anything already sent stays out even if the step later fails」）与 `ctx.collect_events(...)` 手工 fan-in。
2. **「稍后回来」**：**两档，官方推荐前者**。(a) `InputRequiredEvent` → `HumanResponseEvent`：一个 step 返回 `InputRequiredEvent`，外部 `handler.stream_events()` 看到后 `await handler.send_event(HumanResponseEvent(...))`，由另一个 step 消费。这是「step 立刻返回、晚到事件把它续上」。(b) `ctx.wait_for_event(...)`：单 step 内等待，但官方自己警告「`wait_for_event` **replays all code preceding it**」「Any code before the `wait_for_event` call must be safe to repeat」，因此「the event-based approach with separate steps is **generally recommended**」。
3. **持久化与重启恢复**：**官方正式文档化为「可恢复」，且把边界写到了字面上**。`Context.to_dict()` / `Context.from_dict()` / `run(ctx=...)` 三件套，捕获范围是「the events **still in flight** plus the state store」——即含在途事件与 fan-in 缓冲，不只是 store。诚实条款逐条：`This survives a restart, but not a crash during the run`；`A step that was mid-execution when you captured the state is **rewound and runs again from the top**`；`Resume is **at-least-once**, and step side effects need to be safe to repeat`；`There is **no built-in checkpointer** to enable`（要自己订阅 `stream_events(expose_internal=True)` 里的 `StepStateChanged` + `StepState.NOT_RUNNING` 来落盘）；`Workflow instance attributes are not part of the snapshot`；快照是 JSON 序列化，不可序列化对象要放 `Resource`（「resolved once, cached on the workflow, and never serialized」）。另有 DBOS runtime 插件把 step 迁移 journal 到数据库，其代价是同一句「replay-based, steps may execute more than once」。
来源：https://developers.llamaindex.ai/python/llamaagents/workflows/durable_workflows/
**一个会咬人的实现细节**：跨进程 HITL 的官方手册要求先 `await handler.cancel_run()` 再在新进程 `run(ctx=...)`，理由是「That avoids leaving the in-memory run **waiting for an answer that will be delivered to the restored run**」。→ 凡是「把活着的 run 迁移到磁盘再接管」的方案，都必须显式处理**双活**（旧 run 还在等一个永远不会来的事件）。这对 SystemAgent 的进程迁移是同一条约束。
4. **模型在关键路径**：**框架不表态**。step 是普通 async 函数，LLM 只是里面的一次 `await`；没有 Temporal 式 workflow/activity 二分，默认 runtime 也不要求 step 确定性（只有 DBOS 插件因 journal replay 才引入版本风险）。
5. **谁管状态**：框架管 run 级执行状态（在途事件、fan-in 缓冲、typed store、事件流）；**完全不管模型状态**——workflows 文档里没有 conversation/message history 抽象。

**定性**：**「事件就是边、step 就是迁移」的最纯粹实现**，把 checkpoint 写成你可以自己实现的 20 行循环，并坦白「跑到一半的 step 会重头再来」。

---

## 6. CrewAI Flows

**控制流单元**：装饰器标记的普通 Python 方法 + 返回值驱动的事件。「Built on an **event-driven model**」；`@start()` 标入口（可 `@start("label")` 门控，且「All satisfied `@start()` methods will execute (**often in parallel**) when the Flow begins or **resumes**」）；`@listen()` 「will be executed when the specified task **emits an output**」；`@router()` 返回 `Literal[...]` 标签分支；`or_` / `and_` 做多源聚合。**没有显式 `emit()` API——发射物就是方法返回值。**
来源：https://docs.crewai.com/en/concepts/flows.md

1. **控制流单元**：被装饰的 flow 方法（本质是「函数即节点、返回值即事件」）。
2. **工具/异步与「稍后回来」**：**有，但只服务人类反馈**。`@human_feedback` 走自定义 `HumanFeedbackProvider` 时 `raise HumanFeedbackPending(context=..., callback_info=...)`，此时「The flow framework **automatically persists state**」；之后 `ReviewFlow.from_pending(flow_id)` → `flow.resume(feedback)` 续跑。**任意工具调用 / AI 调用没有通用的 pending 原语**；`kickoff_async` 只是同进程并发，不是挂起后跨进程恢复。
来源：https://docs.crewai.com/en/learn/human-feedback-in-flows.md
3. **持久化与重启恢复**：**两套系统**。`@persist`（默认 `SQLiteFlowPersistence`，类级 = 每个方法执行后存）；恢复语义被刻意区分开：`kickoff(inputs={"id": <uuid>})` = **resume**（「continue writing under the same `flow_uuid`. The history extends」）vs `kickoff(restore_from_state_id=<uuid>)` = **fork**（「hydrate the new run's state from it, and assign a **fresh** `state.id`」）。另有 `CheckpointConfig` + `Crew.from_checkpoint(...)`，快照「the full state of the crew, flow, or agent — configuration, agent memory and knowledge sources, task progress, intermediate outputs, internal state and attributes」，且明确列出**不恢复**的东西：lambda/闭包/bound method/`functools.partial`、callable guardrails、`@before_kickoff`/`@after_kickoff`。两者与 `from_checkpoint` 互斥。
来源：https://docs.crewai.com/en/guides/flows/mastering-flow-state.md 、 https://docs.crewai.com/en/concepts/checkpointing.md
4. **模型在关键路径**：**确定性路径是默认写法**。Flow 方法是普通 Python，不调 LLM 就零调用；Crew 路径必然过模型（`Crew(planning=True)` 会把全量 Crew 数据送给 planner；`PlanningConfig` 的 `"low"` 档是「a heuristic check with **no extra LLM call**」）。
5. **谁管状态**：**框架**（`self.state` + UUID + SQLite/LanceDB）。

**定性**：**「用户写的 Python 事件流 + 框架给的状态容器」**，resume/fork 语义分得比多数框架清楚；但「稍后回来」只有人工审批这一条通道，**没有工具级的异步原语**。

---

## 7. Google ADK

1. **控制流单元**：**template workflow agents**（`SequentialAgent` / `ParallelAgent` / `LoopAgent`）+ **Event**。三者「determine the execution sequence according to their type ... **without consulting an AI model** for assistance with the orchestration. This approach results in **deterministic and predictable** execution patterns」。**换代提示**：ADK 2.0（Python/Go）起 template workflow 被 **graph-based workflows** 取代。
来源：https://google.github.io/adk-docs/agents/workflow-agents/
2. **工具/异步与「稍后回来」**：**`LongRunningFunctionTool` 是这批框架里最明确的「工具立刻返回、结果晚到」原语**。模型调用后你的函数只负责发起 + 可选返回 initial result（如 operation id），框架把它包进 `FunctionResponse` 回给模型，然后「the agent run is **ended / paused**」；客户端轮询进度后把 intermediate/final response 发回，进入**下一次 run**。边界写得很硬：「designed to help you start and *manage* long running tasks ... but **not perform** the actual, long task」——真长任务要单独起服务。另有人工确认通道（`FunctionTool(..., require_confirmation=True)`，走 `adk_request_confirmation` 的 `FunctionCall`）。**注意代价**：恢复时框架要把 `FunctionResponse` 再交给 LLM「to generate a user friendly message」——**每次恢复都多一次模型调用**。
来源：https://google.github.io/adk-docs/tools-custom/function-tools/
3. **持久化与重启恢复**：两层。(a) `SessionService` 存 event 历史 + `state`；`InMemorySessionService` 明确「All conversation data is **lost** if the application restarts」，生产要 `DatabaseSessionService`。(b) **崩溃恢复**：`App(..., resumability_config=ResumabilityConfig(is_resumable=True))`（Python ≥1.16.0），靠 `invocation_id` 续跑：「The Resume feature works by logging completed Agent workflow tasks, including incremental steps using **Events and Event Actions**」；SequentialAgent 读 `current_sub_agent`、LoopAgent 读 `current_sub_agent`+`times_looped`、ParallelAgent 判断哪些子 agent 已完成。**诚实条款**：「the Tools in an agent are run **at least once, and may run more than once** when resuming」；恢复前不得修改工作流；Custom Agent 必须自己实现 `BaseAgentState` + 按 step 存 checkpoint + `end_of_agent=True`；Web UI/CLI 不支持恢复。
来源：https://google.github.io/adk-docs/runtime/resume/
4. **模型在关键路径**：**二分最清晰**——编排层（Sequential/Parallel/Loop、graph）零模型调用且确定性；只有 `LlmAgent` 是模型路径。
5. **谁管状态**：**框架（SessionService）**，且 state 有作用域前缀：无前缀=本 session、`user:`=跨该用户所有 session、`app:`=跨全应用、`temp:`=单次 invocation。写入经 `EventActions.state_delta` 提交。

**定性**：**event-loop 运行时 + 分区状态存储 + 唯一官方「工具主动挂起本次 run」原语**；同时它是把「确定性编排 / 模型决策」分工写得最明白的一例。但「恢复」仍是重放，且恢复要多付一次模型调用。

---

## 8. OpenAI Agents SDK

1. **控制流单元**：**没有声明式控制流单元**。控制流 = 一个固定 agent loop + 三个分支：「We call the LLM for the current agent... If the runner classifies the LLM's output as final output, the loop ends... If the LLM requests a handoff, we update the current agent and input, and re-run the loop. If the LLM produces tool calls, we run those tool calls, append the results, and re-run the loop.」终局判定是「it produces text output with the desired type, and there are **no tool calls**」；超 `max_turns` 抛 `MaxTurnsExceeded`。
来源：https://openai.github.io/openai-agents-python/running_agents/
2. **工具/异步与「稍后回来」**：**有，且是「可序列化暂停」做得最干净的一档**：工具声明 `needs_approval=True`（或传 async 判定函数；参数无法安全解析时 **fail closed** 走人工），命中后「execution pauses, and `RunResult.interruptions` ... contains `ToolApprovalItem` entries」；恢复走 `result.to_state()` → `state.approve(interruption)` / `state.reject(...)` → `Runner.run(agent, state)`；`always_approve` / `always_reject` 可 sticky 且随 `RunState` 存活。另有 `RunState.add_input()` 在两次 model call 之间注入新输入，以及 `@tool(timeout=..., timeout_behavior=...)`（仅 async handler）。**边界**：所有 pending 语义都绑定在**审批中断**上，**没有** ADK `LongRunningFunctionTool` 那种「工具主动挂起」。
来源：https://openai.github.io/openai-agents-python/human_in_the_loop/
3. **持久化与重启恢复**：**`RunState` 是一等公民**：「`RunState` is designed to be **durable**. Use `state.to_json()` or `state.to_string()` to store pending work in a database or queue and recreate it later」，`RunState.from_json/from_string` 还原。携带审批决定、pending 工具调用与参数、sticky 决定、guardrail 结果、`pending_input`、handoff 后的当前 agent。**幂等对账做得最细**：若 crash 后 Session 写盘失败，用同一 `RunState` + 原 session backend/id 重试，SDK 核对历史尾巴，「**does not execute completed tools, tool guardrails, hooks, or handoffs again**」；不匹配则 fail closed；终局输出已被接受后才写失败，则把 `RunState` 标记为不可恢复。安全边界也写明：`from_json`「do **not** authenticate the snapshot」，只能从可信存储反序列化。会话历史另有 `Session`（`SQLiteSession`/`RedisSession`/`SQLAlchemySession` 等）或 server-managed `conversation_id` / `previous_response_id`。
来源：https://openai.github.io/openai-agents-python/sessions/
4. **模型在关键路径**：**框架内没有确定性编排层**——每次 `Runner.run` 都是 model→(tool|handoff)→model。确定性只能由宿主程序顺序多次调用拼出来。
5. **谁管状态**：**三层，边界最清楚**：① `RunContextWrapper`（进程内）；② `Session` 或 server-managed response id（会话历史）；③ `RunState`（**暂停中的执行，由应用负责序列化与存储**）。模型只看到 Responses API 的 input items。

**定性**：**「固定 while 循环 + 可序列化 `RunState`」的极简主义**：没有编排 DSL，但把「暂停—存盘—跨进程恢复—幂等对账」做成了第一等公民。**它对「恢复」的定义是「不要重复已完成的工具」，这是 A 档思维在 B 档框架里能做到的最好近似。**

---

## 9. Anthropic Claude Agent SDK

1. **控制流单元**：**没有声明式单元**，控制流是 Claude Code 的 agentic loop 本身（Receive → Evaluate → Execute tools → Repeat，直到产生无工具调用的回复）。对 SDK 使用者真正的「控制流钩子」是 **hooks**：文档定义「A **hook** is a Python function that the Claude Code *application* (*not* Claude) invokes at specific points of the Claude agent loop. Hooks can provide **deterministic** processing and automated feedback for Claude」。
来源：https://code.claude.com/docs/en/agent-sdk/hooks
2. **工具/异步与「稍后回来」**：**唯一提供「把半个工具调用留在 transcript 里、退出进程、之后 resume 重放」的框架**。机制是 `PreToolUse` hook 返回 `permissionDecision: "defer"`：本轮以 `stop_reason: "tool_deferred"` 结束，**待执行的工具调用保留在 transcript 中**；外部拿到答案后 `--resume <session-id>`，同一工具调用**再次触发** `PreToolUse`，这次 `"allow"` + `updatedInput` 携带答案才真正执行。文档强调「There is **no timeout or retry limit**. The session remains on disk until you resume it」。**限制很硬**：只在非交互 `-p` 模式生效；**只在单轮只有一个工具调用时生效**（批内多调用会被忽略并告警，理由是「resume can only re-run one tool」）；工具消失时首条 result 为 `stop_reason: "tool_deferred_unavailable"`。另有 `can_use_tool` 回调，但那是进程内异步回调、不是持久挂起。
3. **持久化与重启恢复**：**会话级极完备，工具级只有 `defer` 一条路**。SDK 自动把 transcript 写到 `~/.claude/projects/<encoded-cwd>/*.jsonl`；恢复三入口：`continue_conversation=True`/`continue: true`（取最近一个）、`resume=<session_id>`（指定）、`fork_session(session_id, ...)`（复制历史开新会话）。可配 `session_store` 适配器镜像到外部存储。**关键边界**：「Sessions persist the **conversation**, not the filesystem」——文件回滚要靠另一套 file checkpointing。另：`defer` 是在工具**执行前**拦截，**没有**「工具跑到一半被杀、从工具内部继续」的能力（未找到一手来源）。
来源：https://code.claude.com/docs/en/agent-sdk/sessions
4. **模型在关键路径**：主循环**必然**过模型，不可改；确定性只存在于 hooks + permissions 两层（纯代码、零模型调用）。权限判定本身是固定 6 步序列（hooks → deny 规则 → ask 规则 → permission mode → allow 规则 → `can_use_tool`）。
5. **谁管状态**：**SDK/CLI 进程拥有 transcript 文件**，应用只持有 `session_id`；上下文压缩（auto-compaction）也归 SDK，`PreCompact`/`PostCompact` hook 可介入。

**定性**：**「把一个成熟 CLI 当库用」**——主循环不可改，但用 hooks 在生命周期点位注入确定性代码，并用 `defer` 做出别家都没有的「工具调用级持久挂起」。它的限制（单工具单轮、非交互模式）恰好说明这个能力有多难做。

---

## 10. 另两个真正与「事件/状态机」相关的

### 10.1 Pydantic AI / pydantic-graph

1. **控制流单元**：**图节点**，边由类型注解推导。官方自述「pydantic-graph — an async graph and **state machine** library for Python where nodes and edges are defined using **type hints**」；老 API 是 `BaseNode.run() -> NextNode`（`End(value)` 收尾），新 API 是 `GraphBuilder` + `@g.step` + 显式 `g.add(...)`/`g.join(...)`。state 是**用户自己 new 出来再传进去的可变对象**（`graph.run(state=state)`），不是框架内部持有的。官方还主动劝退：「Don't use a nail gun unless you need a nail gun」。
来源：https://pydantic.dev/docs/ai/graph/graph/ 、 https://pydantic.dev/docs/ai/graph/builder/
2. **「稍后回来」**：**deferred tools** 是通用原语——tool body `raise CallDeferred`（或 `ApprovalRequired`），两种解析：inline（`HandleDeferredToolCalls` capability 内解，单次 run 内继续）或 stop-the-world（run **结束**、把 `DeferredToolRequests` 当 output 返回，调用方拿「原 message history + `DeferredToolResults`」起**全新 run**）。边界明确：「That follow-up is a **separate agent run** with its own `run_id` (do not reuse the paused run's); keep pause/resume correlation via `conversation_id`」。另有 `BackgroundTools`（工具后台跑、结果作为 follow-up message 回灌）。**graph 层没有 wait-for-event**，只有 `graph.iter()` 手工步进（人当调度器）。
来源：https://pydantic.dev/docs/ai/tools-toolsets/deferred-tools/
3. **持久化与重启恢复**：**唯一一个官方明确写「图状态不做快照」的框架**：「**Graph state is not snapshotted**」「Neither the graph builder API nor the original Graph API snapshots graph state; `pydantic_graph.persistence` was **removed in V2**, because of the complexity of achieving consistent snapshotting with parallel execution」。durable execution 全部外挂给引擎（Temporal / DBOS / Prefect / Restate / AWS Lambda / Absurd / pgtask / Airflow），核心声明是「A durable engine journals the agent's model requests and the tool calls it wraps, and a restarted run **replays the completed ones from the journal** and retries the one in flight」——**「哪些调用不是 durable」是引擎相关的**：「Which tool calls (function tools, MCP servers) an engine wraps for you ... **differ per engine**」。官方还立了一句极好的切分：「**Durability is not storage**」——保住一次 run 不中断是引擎的事，把 run 存下来/续跑/从早期 step fork 是 `StepPersistence`（快照）的事，两者不必绑在一起。Temporal 页的 not-durable 直引：「if an activity fails part-way through it is **restarted from the beginning**」；`@on_event` 监听器跑在 workflow code 里「so they re-run on every workflow replay and must be deterministic」。DBOS 页：「Custom tool functions and event stream handlers ... are **not automatically wrapped**」，且「DBOS requires durable steps to start in a **deterministic order**」。
来源：https://pydantic.dev/docs/ai/capabilities/durable_execution/overview/ 、 https://pydantic.dev/docs/ai/capabilities/durable_execution/temporal/ 、 https://pydantic.dev/docs/ai/capabilities/durable_execution/dbos/
4. **模型在关键路径**：唯一做了显式「确定 vs 非确定」二分的框架，**但二分发生在引擎层（Temporal 的 workflow/activity、DBOS 的 workflow/step），不在 pydantic-graph 层**。graph 对 LLM 零假设。
5. **谁管状态**：三分：graph state（用户的、框架不快照）、deps（用户的、Temporal 下需可序列化）、message history（框架生产、持久化归用户、`StepPersistence` 按 step 落库）。

**定性**：**唯一把「原生状态机」和「崩溃存活」切成两层卖的框架**，而且切得很诚实：`StepPersistence` 的边界写明「A step that was in flight is **not retried**: its tool calls show up as started effects in the ledger, and **you decide** whether redoing them is safe」、「a tool with a started record and no terminal update should be treated as `unknown_after_crash`: the side effect **may or may not** have happened」。

### 10.2 Mastra Workflows（TypeScript）

1. **控制流单元**：**step（`createStep`）+ 显式链式 DSL**。`.then()` / `.branch()` / `.parallel()` / `.dountil()` / `.dowhile()` / `.foreach()` / `.map()` / `.sleep()` / `.sleepUntil()`，末尾 `.commit()` 固化定义（「Creating a run on a workflow with uncommitted step changes throws an error」）。与 LlamaIndex 相反：这是**显式构图 + 显式边**，图结构本身（`serializedStepGraph`）进快照，因此改图会让老 run 失效。
来源：https://mastra.ai/docs/workflows/overview
2. **工具/异步与「稍后回来」**：**`suspend()` / `resume()` 是真原语，且被快照支撑**。step 的 `execute` 里 `return await suspend({...})`，run 返回 `result.status === 'suspended'` 并给出被挂起 step 的**路径**；恢复 `run.resume({ step, resumeData })`（只有一个挂起点时可省略 `step`），**同一 `execute` 重新进入**，这次拿到 `resumeData` 与当初的 `suspendData`。`resume()` 可从任何地方调用（HTTP endpoint、`setTimeout`、人在环 UI）——**进程不用挂着等**，这正是「立刻返回、晚点回来」的形态。另一档 `.sleep()`/`.sleepUntil()` 是 workflow 级暂停（状态 `waiting` 而非 `suspended`）。
来源：https://mastra.ai/docs/workflows/suspend-and-resume
3. **持久化与重启恢复**：**快照内容清单被逐项列出**：`The current state of each step` / `The outputs of completed steps` / `The execution path taken` / `Any suspended steps and their metadata` / `The remaining retry attempts`。落 `workflow_snapshots` 表（默认 libSQL，可换 Postgres/MongoDB/DynamoDB 等），「persist across deployments and application restarts」。**崩溃恢复的真实机制与边界写得极诚实**：执行期间持 storage lease 并后台续租，`restart()` 从 last active step 续跑；但「The lease is **advisory** and doesn't fence writes. If a process stops renewing its lease but keeps executing a step ... recovery can **run that step a second time**」。另有 `run.timeTravel({ step, ... })` 从任意 step 重放（图变了就报错且「the stored snapshot is left unchanged」）。
来源：https://mastra.ai/docs/workflows/snapshots
**跨进程恢复默认是关的**：内置引擎只在挂起点写快照，真跨进程要靠外挂 Inngest，且「`retries` **defaults to 0**」——「Set retries to let a run survive a failed request to your app, such as a process restart, redeploy, or out-of-memory crash」。→ 不显式设 retry 就等于**没有**崩溃恢复。这与「快照已落库 = 已持久」的直觉相反，是选型时最容易漏的一处。
4. **模型在关键路径**：**无约束、也不区分模型调用与其他 I/O**。step 里可直接跑 agent/tool；文档没有 Temporal 那种确定性二分，也**没有**要求 step 幂等（幂等性只以「lease 可能让 step 跑两次」「retry 会重跑 step」的形式隐含存在）。
5. **谁管状态**：**管得最多**——step I/O、workflow shared state（`State persists across the entire workflow run, including suspend/resume cycles`）、执行所有权（storage lease）三层都在框架里。

**定性**：**把「运行状态」做成完整、可枚举、可落库、可时间旅行的序列化对象**，但恢复粒度仍只到 step；而且因为它**不 memoize 模型调用**，「suspend 前先问一次 LLM」在 resume 时会重问一次（同一个 `execute` 重新进入）。

---

## 11. 旁证：durable execution 基座怎么看同一个问题

不是为了抄它们的 API，而是因为它们把这套问题研究了十年，术语最准。

- **Restate**：三种「持久协调原语」——**Signal**（invocation id + name，可多次解析，用于 agent steering / human approvals）、**Awakeable**（一次性 token，外部系统回调）、**Workflow promise**。关键句：「All three **survive retries and process restarts**. When an invocation has nothing else to do while waiting, Restate can **suspend** it and resume it when the next result arrives.」副作用走 `ctx.run`：「Restate uses an execution log to **replay** operations after failures and suspensions. Non-deterministic operations ... must be wrapped to ensure deterministic replay」；另有按 invocation ID 播种的确定性 UUID/随机数/时间。→ **这是「工具立刻返回、结果晚到、进程可退」在工业界的标准答案形态**。
来源：https://docs.restate.dev/develop/python/external-events/ 、 https://docs.restate.dev/develop/python/durable-steps/
- **DBOS**：「If a workflow is interrupted for any reason, DBOS automatically recovers its execution **from the last completed step**」；workflow 必须确定性、非确定性操作放 step；**durable sleep / durable timeout**（「they are stored in the database and persist across restarts」）；并发要 `DBOS.reserve_step` 以**确定顺序**占位，否则 replay 会乱；非确定性检测会抛 `DBOSStepNondeterminismError`。
来源：https://docs.dbos.dev/python/tutorials/workflow-tutorial

**两点必须记住的行业共识**：
1. **没有一家用「续体捕获」**。Restate/Temporal/DBOS/LangGraph/Mastra/LlamaIndex 全是「确定性重放 + 副作用记账」。LLM 调用要想不被重复计费，就必须被包进记账单元（`ctx.run` / activity / `@task`）——包不住就只能接受重复。
2. **「durability」和「storage」是两件事**（Pydantic AI 把这句写成了 pull-quote）：保住一次执行不中断 vs 把执行存下来、续跑、fork，是两个可以分开买的能力。

---

## 横向对照表

> 「异步工具原语」列回答的是「工具调用能不能立刻返回、结果晚到」以及**用的是什么机制**。「持久化恢复」列标注**恢复粒度**与**是否真能跨重启**。术语：`重放` = 从头/从 checkpoint 重跑到断点、已记账步骤不重做。

| 框架 | 控制流单元 | 异步工具 / 「稍后回来」原语 | 持久化恢复 | 模型在关键路径 | 支持确定性状态机 |
|---|---|---|---|---|---|
| **AutoGen v0.4+** | message（`RoutedAgent` + `@message_handler`） | topic/subscription 广播，**发起方不阻塞**；只有 `ClosureAgent` 回调塞队列，**无「等回程结果」的配对原语** | `save_state`/`load_state` + `TeamState`（含队伍游标），**会话边界级**；官方明说阻塞等待外部输入时「**cannot be saved or resumed**」 | core=0（纯代码），agentchat=每 turn 1 次 | ✅ core 层可写纯消息状态机 |
| **Microsoft Agent Framework** | executor + edge，批=superstep（**Pregel/BSP + 同步屏障**） | **`ctx.request_info()` + `@response_handler` / `RequestPort`**：paused 后 emit 外部请求事件，回复到达才 resume；**「without consuming compute or model tokens while waiting」** | superstep 末检查点，含**下一轮待投递消息 + pending requests/responses**；InMemory/File/Cosmos 三档；需稳定 executor/agent Id | executor 可为纯代码（示例「without requiring a model」） | ✅ |
| **AgentScope** | agent 方法 / pipeline；`MsgHub` 广播**作用域** | ❌ 无持久挂起；`interrupt()` = asyncio cancel；只能重入 `await agent(msg)` | `StateModule.state_dict/load_state_dict` + `JSONSession`，**agent 对象状态级**（**v1.0 已无 `to_dist` 分布式 actor**） | pipeline=0，`ReActAgent`=模型循环 | ✅ 组合层确定 |
| **SK Process Framework** | **step + event**，常驻事件循环（`keep_alive`） | `send_message`/`emit_event` 注入事件、循环等外部事件；**不阻塞发起方** | **分裂**：Local runtime 无持久化；Dapr runtime 经 actor `state_manager` 持久化**step 状态 + 入站消息队列** | 可 0（`KernelFunction` 可为纯代码） | ⚠️ 事件驱动数据流 + 每步状态，**无显式迁移表** |
| **LangGraph** | superstep / node（Pregel-BSP，actor+channel） | `interrupt()` + `Command(resume=)`；**恢复时该 node 从头重跑**；`@task` 提供 memoization | superstep checkpoint + node 级 writes → 从最后成功步恢复；durability 模式可调（`exit`/`async`） | 每 superstep 1 次；workflow 形态可 0 | ✅（`Workflows` vs `Agents` 官方二分） |
| **LlamaIndex Workflows** | step + typed event（边由注解推导） | `InputRequiredEvent`/`HumanResponseEvent`（推荐）；`ctx.wait_for_event` 有「重放前序代码」警告 | `Context.to_dict/from_dict` + `run(ctx=)`，**含在途事件与 fan-in 缓冲**；**at-least-once，半途 step 从头跑**；无内置 checkpointer | 框架不表态 | ✅（step 即迁移） |
| **CrewAI Flows** | 装饰器方法 + 返回值即事件（`@start`/`@listen`/`@router`） | **仅人工反馈**：`HumanFeedbackPending` → `from_pending()` → `resume()`；工具/AI 无通用挂起 | `@persist`（resume vs fork 语义分明）+ `CheckpointConfig`（整套对象图，含不支持项清单） | Flow=0，Crew=模型 | ✅（Flow 就是 Python） |
| **Google ADK** | template workflow agent（Seq/Par/Loop）+ Event | **`LongRunningFunctionTool`**：run 结束、客户端回 `function_response` 起新 run（**恢复多付 1 次模型调用**） | `SessionService`（有 `DatabaseSessionService`）+ `ResumabilityConfig(is_resumable=True)` + `invocation_id`；**工具 at least once、可能多次** | 编排=0（「without consulting an AI model」），`LlmAgent`=模型 | ✅ 最强表述 |
| **OpenAI Agents SDK** | 固定 agent loop（final/handoff/tool 三分支） | 审批中断：`needs_approval` → `RunResult.interruptions` → `state.approve()`/`reject()`；**无「工具主动挂起」** | `RunState.to_json/from_json`（**durable by design**）+ `Session`；crash 后对账且**不重跑已完成工具** | 每 loop 1 次，无确定性层 | ❌（无编排原语） |
| **Claude Agent SDK** | 无（Claude Code agent loop；hooks 是注入点） | **`permissionDecision: "defer"` → `stop_reason: tool_deferred`**，工具调用留在 transcript，`--resume` 后重放该调用；**仅非交互 + 单工具单轮** | `~/.claude/projects/<cwd>/*.jsonl` + `resume`/`continue`/`fork_session`；**只存对话不存文件系统** | 主循环必过模型；hooks/permissions=0 | ❌（主循环不可改） |
| **Pydantic AI / pydantic-graph** | node（`BaseNode.run()` 或 `@g.step`，边由类型注解） | **deferred tools**（`CallDeferred`）：stop-the-world = run 结束、用原 history 起**新 run**；或 inline handler；`BackgroundTools` | **图状态明确不快照**（`V2` 删了 `pydantic_graph.persistence`）；durable 全靠外挂引擎（Temporal/DBOS/…）；`StepPersistence` 按 step 落 message history | graph=0；agent loop=1/step | ✅（图即状态机；但无原生 durable） |
| **Mastra Workflows** | step（`createStep`）+ 链式 DSL（显式边） | **`suspend()` / `resume()`**：step 内挂起、快照落库、进程可退、任意来源 resume；**同一 execute 重新进入** | `workflow_snapshots` 表（含已完成 step 输出 + 挂起元数据 + 剩余重试）；lease **advisory**，step 可能跑两次 | 无约束 | ✅（图即状态机） |
| *旁证* Restate | durable handler + journal | **Signal / Awakeable / Workflow promise**，全部「survive process restarts」+ 可 suspend | execution log 重放；`ctx.run` 记账副作用 | 不适用 | 不适用（基座） |

---

## 结论：业界是否已经解决这个问题

诚实地说：**三个诉求里，第一个已解决、第二个部分解决、第三个只在「重放」意义下解决。「长生命周期状态机 + LLM 咨询」的结合仍是空白。**

### 已经解决的部分

1. **「工具调用立刻返回、结果以事件形式晚到」——原语齐备，且有五种成熟形态，但要按「等待期成本」挑。** MAF `request_info`/`RequestPort`（暂停 + emit 外部请求，明确「**without consuming compute or model tokens while waiting**」）、Mastra `suspend()/resume()`（step 内挂起、任意来源唤醒、进程可退）、Claude SDK `defer`（工具调用级留在 transcript）、LangGraph `interrupt()+Command(resume=)`（节点级）、ADK `LongRunningFunctionTool`（run 结束、客户端回调再起——**但恢复要多付一次模型调用**）。Restate 的 Awakeable 是这些形态在基座层的统一抽象。**这个诉求不需要自研，选一个语义即可；但「等待期间不烧 token」应当作为硬性选型条件。**
2. **「确定性路径不经过模型」——已被证明是正确设计，且多家明确表态。** ADK 说得最硬（「without consulting an AI model ... deterministic and predictable」），LangGraph 有 `Workflows` vs `Agents` 二分，CrewAI 的 Flow 本身就是 Python，SK Process 的 `KernelFunction` 可为纯代码。**「编排层零模型调用」已是行业共识。**
3. **「图/事件编排的可观测与可调试」——彻底解决。** superstep 事件、`StepStateChanged`、`SuperStepCompletedEvent`、tracing 都已是标配。

### 部分解决的部分

4. **「进程可重启并恢复等待中的……东西」——能恢复「消息/step 输出」，恢复不了「续体」。** 全部框架的恢复语义都是**重放（replay）**，并且每家都自己承认会被重放坑：LangGraph「the code does **NOT** resume from the same line of code」、LlamaIndex「step is **rewound and runs again from the top**」、ADK「Tools ... run **at least once, and may run more than once**」、Mastra「recovery can **run that step a second time**」、Pydantic/Temporal「activity ... is **restarted from the beginning**」。**没有任何一家提供续体捕获。** 反证最强的一句来自 AutoGen 官方：阻塞等待外部输入会把团队置于「an unstable state that **cannot be saved or resumed**」——它干脆承认这个中间态不可持久化。**未找到一手来源**：任何 agent 框架把「持久化 coroutine / 续体」列为能力。

   好消息是存在**一条可行的近似路径**：把「非确定性操作」包进记账单元，让重放时用记录值代替重算。LangGraph 的 `@task`、Restate 的 `ctx.run`、Temporal 的 activity、Pydantic AI 的 `StepPersistence` 都是这条路。**这是「恢复等待中的续体」在工程上唯一被验证过的实现方式**，代价是必须显式划分「确定性壳 / 记账内核」。

### 真正的空白

5. **「长生命周期状态机 + LLM 咨询」没有任何框架给出原语。** 拆开看：
   - **有状态机的**（Pydantic AI graph、Mastra、CrewAI Flow、SK Process）→ **没有 LLM 咨询原语**，也没有「咨询点可回放」的记账机制。Mastra 甚至因为 resume 会**重新进入同一 `execute`**，导致「suspend 前先问 LLM」在 resume 时**必然重问一次**。
   - **有 LLM 咨询的**（ADK `LongRunningFunctionTool`、OpenAI 审批、Claude `defer`）→ **状态机是模板化的**（Sequential/Parallel/Loop 或固定 while loop），表达不了「我这个能力有 7 个业务状态、每个状态可能向 LLM 咨询、咨询完回到原状态继续」。
   - **「两者都有但不在同一个东西里」的最接近一例是 MAF**：superstep 图 + executor（可为纯代码）+ `ctx.set_state/get_state` 一等公民状态 + pending request/response 进检查点 + 「等待不烧 token」——三条诉求它**全部**沾到了。它缺的不是状态，而是**「状态机」这一层定义**：没有「当前状态 ∈ S、事件 e → 下一状态」的显式迁移表，状态迁移逻辑散落在各 executor 的 `@handler` 里，因此无法静态校验、无法对状态空间做可视化或穷举。次一例是 SK Process + Dapr（常驻事件循环 + 每步状态 + 邮箱持久化），但 durable 能力绑死 Dapr 运行时，同样没有显式迁移表。
6. **空白的具体形状**：缺一个**「状态机迁移是确定性的、LLM 咨询是可记账的副作用、咨询等待期间进程可以死」**的合成原语。现有框架要求你在以下三点中至少放弃一个：
   - 放弃确定性（让模型决定下一步迁移，退回 ReAct/agent loop）；
   - 放弃挂起（把 LLM 咨询放在同步路径里，一次 turn 等到底）；
   - 放弃幂等（接受 resume 时重问一次 LLM、重付一次费用，或自己写咨询服务/缓存）。

### 对 SystemAgent 设计的五条含义

1. **控制流单元选「superstep + 事件」而不是「turn」。** LangGraph、MAF、SK Process 三家独立收敛到 superstep，说明这是事件驱动图编排的稳定解；而 turn 粒度无法表达「一次 tick 内多事件并发、下一个 tick 才可见」。
2. **「工具立刻返回」用 Awakeable/Signal 形态（id + 一次性兑现），不要用「广播 + 自己维护关联」。** AutoGen 的 topic 广播缺回程配对、AgentScope 的 `MsgHub` 是作用域，两者都需要你自己维护 correlation，这正是 ADK、Mastra、Claude SDK、Restate 都改用「显式 id + 一次性 resolve」的原因。
3. **LLM 咨询必须做成「可记账的副作用」，且放在确定性壳之外。** 具体做法：状态迁移（当前状态 + 事件 → 下一状态）用纯代码/handler 表达；LLM 只作为某个状态内的「咨询活动」，其输入输出进 journal；resume 时咨询结果用记录值而不重新调用。这是把 LangGraph `@task` / Restate `ctx.run` 的思路抬到状态机层。
4. **持久化粒度为「状态机定义 + 当前状态 + 在途事件队列 + 活动结果账本」，四者缺一不可。** 对照证据：MAF 把 pending messages 与 pending requests 一起存进 checkpoint、SK 的 Dapr 模式存入站消息队列、LlamaIndex 的 `to_dict()` 明确含「events still in flight」。只存「当前状态 + 对话历史」的方案（如 AutoGen `save_state`、AgentScope `StateModule`）重启后接不上。
5. **把「重启恢复」的语义定义为 at-least-once 并写进契约，而不是假装 exactly-once。** 五家框架全部承认会被重放坑，且都要求副作用幂等（或用 `idempotency key`）。SystemAgent 若要做「进程可重启并恢复等待中的续体」，应当在工具契约层强制要求：可重入声明、幂等键、或显式的「不确定副作用」标记（Pydantic AI 的 `unknown_after_crash` 与 `started but no terminal update` 是现成的好词汇）。

---

## 一手来源清单

**AutoGen**：https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/architecture.html ｜ .../core-concepts/topic-and-subscription.html ｜ .../framework/message-and-communication.html ｜ https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/state.html

**Microsoft Agent Framework**：https://learn.microsoft.com/en-us/agent-framework/concepts/workflows/builder-and-execution ｜ .../concepts/workflows/events ｜ https://learn.microsoft.com/en-us/agent-framework/workflows/checkpoints ｜ https://learn.microsoft.com/en-us/agent-framework/workflows/human-in-the-loop

**AgentScope**：https://doc.agentscope.io/tutorial/task_pipeline.html ｜ https://doc.agentscope.io/tutorial/task_state.html

**Semantic Kernel Process**：https://learn.microsoft.com/en-us/semantic-kernel/frameworks/process/process-framework ｜ .../process-core-components ｜ 源码 `semantic-kernel==1.46.0`（PyPI wheel）内 `semantic_kernel/processes/{local_runtime/local_process.py, kernel_process/kernel_process_step_context.py, dapr_runtime/actors/step_actor.py, dapr_runtime/actors/process_actor.py}`

**LangGraph**：https://docs.langchain.com/oss/python/langgraph/pregel ｜ .../checkpointers ｜ .../interrupts ｜ .../fault-tolerance ｜ .../functional-api ｜ .../workflows-agents ｜ https://github.com/langchain-ai/langgraph/blob/8c4904bee93cc4124f11132c7c3f4e747dbce3c0/docs/docs/concepts/durable_execution.md

**LlamaIndex Workflows**：https://developers.llamaindex.ai/python/llamaagents/workflows/ ｜ .../workflows/durable_workflows/ ｜ .../workflows/human_in_the_loop/

**CrewAI**：https://docs.crewai.com/en/concepts/flows.md ｜ https://docs.crewai.com/en/learn/human-feedback-in-flows.md ｜ https://docs.crewai.com/en/guides/flows/mastering-flow-state.md ｜ https://docs.crewai.com/en/concepts/checkpointing.md

**Google ADK**：https://google.github.io/adk-docs/agents/workflow-agents/ ｜ .../runtime/resume/ ｜ .../tools-custom/function-tools/ ｜ .../events/ ｜ .../sessions/

**OpenAI Agents SDK**：https://openai.github.io/openai-agents-python/running_agents/ ｜ .../human_in_the_loop/ ｜ .../sessions/ ｜ .../results/

**Claude Agent SDK**：https://code.claude.com/docs/en/agent-sdk/hooks ｜ .../agent-sdk/sessions ｜ https://docs.claude.com/en/api/agent-sdk/overview

**Pydantic AI**：https://pydantic.dev/docs/ai/graph/graph/ ｜ .../graph/builder/ ｜ .../capabilities/durable_execution/overview/ ｜ .../durable_execution/temporal/ ｜ .../durable_execution/dbos/ ｜ .../tools-toolsets/deferred-tools/

**Mastra**：https://mastra.ai/docs/workflows/overview ｜ .../workflows/suspend-and-resume ｜ .../workflows/snapshots

**Durable 基座**：https://docs.restate.dev/develop/python/external-events/ ｜ .../develop/python/durable-steps/ ｜ https://docs.dbos.dev/python/tutorials/workflow-tutorial

---

## 附：本次调研中「未能证实」或「路径已变迁」的点（避免以讹传讹）

- **AutoGen**：`autogen-core` 运行时在途消息的持久化能力——**未找到一手来源**；只有 agentchat 的 `save_state`（会话/队伍游标）有文档。另：v0.4 那篇 "reimagined" 架构博客在 `microsoft.github.io/autogen/stable/blog/...` 下已 404（仅第三方转载可用）；`core-concepts/agent-and-agent-runtime.html` 不存在（真名 `architecture.html`），`framework/event.html` 从未存在。
- **Microsoft Agent Framework**：执行模型页不在 `/workflows/execution-model`（404），实际在 `concepts/workflows/builder-and-execution`（靠 `toc.json` 才定位到）；AutoGen `TeamState` 与 MAF checkpoint 之间的映射关系——**未证实**。
- **AgentScope**：`MsgHub` 之外是否存在「可跨进程挂起等待某事件」的原语——**未找到一手来源**。v0.1.x 文档站（`doc.agentscope.io/v0/en` 等）全部 404，只剩 `to_dist`/`msghub` 的 v0.1.6 仓库 README 可引；v1.0 searchindex 中 `to_dist`/`distributed` 命中数 0。
- **Google ADK**：旧路径 `/agents/workflow-agents/{sequential,parallel,loop}.md` 与 `/sessions/session-services.md` 均 404，正确路径是 `*-agents/index.md`；且 ADK 2.0 起 template workflow 已被 **graph-based workflows** 取代（有 `/graphs/` 系列页面），本报告仍按 template 形态描述，因为它是「确定性编排」最清晰的表述来源。
- **CrewAI**：`/concepts/persist.md`、`/concepts/human-feedback.md`、`/concepts/router.md` 三个路径返回 200 但内容是文档首页（**站点 fallback，非真实页面**）；实际内容在 `/concepts/flows.md` 与 `/learn/human-feedback-in-flows.md`。
- **Claude Agent SDK**：文档已整体迁至 `code.claude.com/docs/en/...`，旧的 `docs.claude.com/en/api/agent-sdk/...` 会 302；「工具跑到一半被 kill 后从工具内部继续（保留部分副作用）」——**未找到一手来源**，`defer` 是在执行前拦截。
- **Pydantic AI**：不存在名为 `StateMachine` 的类；`/graph/beta/` 现在 302 到 `/graph/builder/`，即 `GraphBuilder`。`llms.txt` 的 durable execution 章节**未出现 Inngest**（列的是 Temporal/DBOS/Prefect/Restate/AWS Lambda/Absurd/pgtask/Airflow），故「Inngest 集成」记为**未找到一手来源**。
- **Mastra**：内置引擎是否「每完成一个 step 就写一次快照」有两处文档口径不一致（`snapshots` 页说「whenever a workflow is suspended」，`time-travel` 页又依赖已完成 step 的快照）——**未能对齐，标记为未证实**。
