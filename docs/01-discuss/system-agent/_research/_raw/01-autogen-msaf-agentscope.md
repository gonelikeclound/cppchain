# AutoGen / MS Agent Framework / AgentScope —— 控制流·异步·持久化 一手笔记

采集方式：`curl` + html→text 重抽取；AgentScope 用 sphinx `_sources/*.rst.txt` 干净源。仅列 HTTP 200 的 URL。

---

## AutoGen v0.4+（autogen-core / autogen-agentchat；stable 文档，py 包 0.4.x–0.7.x 期）
**来源**:
- https://microsoft.github.io/autogen/stable/ （200）
- https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/architecture.html （200，页标题 "Agent Runtime Environments"）
- https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/agent-and-multi-agent-application.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/agent-identity-and-lifecycle.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/topic-and-subscription.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/agent-and-agent-runtime.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/message-and-communication.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/distributed-agent-runtime.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/design-patterns/concurrent-agents.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/index.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/state.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/human-in-the-loop.html （200）
- https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/graph-flow.html （200）
- https://devblogs.microsoft.com/autogen/autogen-reimagined-launching-autogen-0-4/ （200；原 stable/blog/... URL 已 404）

1. **控制流单元**：**消息 (message)**，不是事件对象也不是 continuation。分层是 actor 模型：agent 由 runtime 创建，`AgentId = (AgentType, AgentKey)`（字符串形式 `Agent_Type/Agent_Key`）；"messages are the only means through which agents can communicate"。两种投递：direct messaging（点对点 RPC）与 broadcast（topic + subscription pub-sub，"agent runtime implements a publish-subscribe model through its broadcast API"）。`Topic = (TopicType, TopicSource)`，`TypeSubscription(topic_type=, agent_type=)` 是首选声明方式，"Type-Based Subscription = Topic Type –> Agent Type"。设计博客定位："Core API: The foundation layer offering a scalable, **event-driven actor framework**"；"Agents now communicate through asynchronous messages, supporting both **event-driven and request/response** interaction patterns"。AgentChat 层则自称 "task-driven, high-level framework"（每 `run()` 一个 task）。
2. **工具/异步**：handler 全部 `async def`，靠 `@message_handler` + 参数类型注解路由（`RoutedAgent`），无匹配则 `raise CantHandleException`。
   - **阻塞式**：`await self.send_message(msg, agent_id)` / `await runtime.send_message(...)` 是 RPC——"Awaiting calls to these methods will return the return value of the receiving agent's message handler"；"If the invoked agent raises an exception while the sender is awaiting, the exception will be propagated back to the sender."
   - **发出即走**：`await self.publish_message(msg, topic_id=TopicId(...))`（或 `DefaultTopicId()` + `@default_subscription`）——"This call must still be awaited to allow the runtime to schedule delivery of the message to all subscribers, but it will **always return None**"；"If a response is given to a published message, it will be **thrown away**"；发布者不会收到自己发布的消息（防环）。
   - **"稍后再回来"的原语**：**没有** future / promise / `request_info` 这类一次性的挂起-恢复对象。可用的是 (a) broadcast 一把甩出去、由订阅方继续；(b) `ClosureAgent.register_closure(runtime, type, cb, subscriptions=...)` + `ClosureContext` 把结果塞进外部 `asyncio.Queue`（"to collect and process the results programmatically"）；(c) AgentChat 层的 `UserProxyAgent` 只是 **同步阻塞** run。
   - 反向证据（很重要）：`UserProxyAgent` 在 run 中被调用时 "it **blocks the execution of the team** until the user provides feedback or errors out. This will hold up the team's progress and put the team in an **unstable state that cannot be saved or resumed**"。所以框架官方推荐的 HITL 姿势是"结束本次 run，下一次 `run()`/`run_stream()` 再喂回去"。
   - 并发：`SingleThreadedAgentRuntime` 是单线程 asyncio，`runtime.start()` = "Start processing messages in the background"；同一 topic 的多个订阅者并发处理（文档输出中 Agent 1/Agent 2 的 start/finish 交错）。`stop()` "will return immediately but will not cancel any in-progress message handling"，可再 `start()`；`stop_when_idle()` 阻塞至无待处理消息。
3. **持久化与重启恢复**：有，但粒度是**会话快照**，不是工作流检查点。
   - 单 agent：`await agent.save_state()` / `await agent.load_state(state)`；`AssistantAgent` 的 state = `{'type': 'AssistantAgentState', 'version': '1.0.0', 'llm_messages': [...]}`，"its state consists of the model_context"；自定义 agent "The default implementations save and load an **empty state**"。
   - Team：`save_state()`/`load_state()`；`TeamState = {agent_states: {...}, team_id: ...}`，其中 `RoundRobinManagerState = {message_thread, current_turn, next_speaker_index}`、每个 agent 外层是 `ChatAgentContainerState = {agent_state, message_buffer}`。纯 dict，可 `json.dump` 落盘。
   - 恢复语义：恢复的是**对话上下文 + 队伍游标**；`reset()` 会清空（文档示例 reset 后模型答不出上一首诗）。**在途 handler / 未完成的工具调用不在 state 结构里**；且官方明说阻塞等待外部输入时 "cannot be saved or resumed"。→ **重启不恢复 in-flight 工作**，只能从"最后一次 save 的会话边界"继续。
   - 分布式：`GrpcWorkerAgentRuntimeHost` + `GrpcWorkerAgentRuntime`（`pip install "autogen-ext[grpc]"`），host 侧的 session 仅"keeps sessions for all direct messages (i.e., RPCs)"；页面自带 "the distributed agent runtime is an **experimental** feature"。生命周期页明确 agent 被 "paging in/out" 到其他机器 "This is **not implemented yet**"。
4. **关键路径 LLM 调用次数 / 确定性路径**：Core 层 **0 次**——runtime 不调模型，agent 可以是纯代码（`MyMessageType` 打印示例即无模型）。AgentChat 的 `AssistantAgent` 默认每 `on_messages`/每回合 **1 次**（MS AF 迁移指南原话："AssistantAgent is **single-turn unless you increase `max_tool_iterations`**"；而 AF 的 `Agent` "is multi-turn by default"）。确定性路径：`GraphFlow` + `DiGraphBuilder`（`add_node/add_edge`，支持 sequential / parallel fan-out / conditional / loop），"when your task requires **deterministic control**, conditional branching..."，但标注 "GraphFlow is an **experimental** feature"。
5. **谁管状态**：**分层**。Core：runtime 管 agent 身份/生命周期/投递，不碰业务状态；agent 自己持有 instance 状态（`RoutedAgent.__init__` 里的字段），应用负责 `save_state` 序列化落盘。AgentChat：team 管 `message_thread`、`current_turn`、`next_speaker_index` 这些**控制游标**。**模型不掌握控制流**——谁下一个说话由 team 类型 + `TerminationCondition` 决定；只有 `SelectorGroupChat` 让模型参与"选下一个说话人"这一决策点。
6. **一句话定性**：**事件驱动的 actor runtime + pub-sub 消息总线**（控制流单元=消息+订阅），Team 层是 task-driven 的会话循环；**没有超级步/没有屏障**，也没有"挂起等外部输入再原地恢复"的原语（要恢复只能靠 save_state 后重开 run）；确定性只能靠 experimental 的 GraphFlow 显式图。

**未能证实**：
- v0.4 架构博客原 URL `.../stable/blog/2024/10/02/autogen-v0.4.html` 及 `stable/blog/`、`0.4.0/blog/...`、`dev/blog/...` 全部 **404**；只能引用 devblogs 转载页（200，但页面标注日期为 January 17th, 2025）。
- `core-concepts/agent-and-agent-runtime.html`、`.../framework/event.html`、`.../framework/runtime.html` 均 **404**（实际存在的是 architecture.html / message-and-communication.html）；任务里设想的 "event" 页不存在。
- 未找到"pause and resume"专页；`InterventionHandler`（tool approval）只在导航栏出现，未抓正文。
- 官方文档**未直接给出** `asyncio.create_task` 作为"后台继续"的写法；"save_state 是否覆盖在途 handler" 无明文断言，只能从 `TeamState` 字段结构 + "cannot be saved or resumed" 反推。

---

## Microsoft Agent Framework（learn.microsoft.com；页面标注 "Last updated on 2026-08-25" / "2026-10-28"；Python 包 `agent-framework`，checkpoints 文档提到 "Starting in Python version **1.13.0**"）
**来源**:
- https://learn.microsoft.com/en-us/agent-framework/ （200）
- https://learn.microsoft.com/en-us/agent-framework/workflows/ （200，页名 "Workflow capabilities"）
- https://learn.microsoft.com/en-us/agent-framework/concepts/workflows/ （200）
- https://learn.microsoft.com/en-us/agent-framework/concepts/workflows/builder-and-execution （200）
- https://learn.microsoft.com/en-us/agent-framework/concepts/workflows/edges （200）
- https://learn.microsoft.com/en-us/agent-framework/concepts/workflows/events （200）
- https://learn.microsoft.com/en-us/agent-framework/concepts/workflows/state （200）
- https://learn.microsoft.com/en-us/agent-framework/workflows/checkpoints （200）
- https://learn.microsoft.com/en-us/agent-framework/workflows/human-in-the-loop （200）
- https://learn.microsoft.com/en-us/agent-framework/hosting/durable-extension （200）
- https://learn.microsoft.com/en-us/agent-framework/migration-guide/from-autogen/ （200）

1. **控制流单元**：**superstep + executor + edge**（图/BSP），不是消息队列。原文："The framework uses a **modified Pregel execution model** — a **Bulk Synchronous Parallel (BSP)** approach with superstep-based processing." 每个 superstep："Collects all pending messages from the previous superstep / Routes messages to target executors based on edge definitions / Runs all target executors concurrently within the superstep / **Waits for all executors to complete before advancing (synchronization barrier)** / Queues any new messages emitted by executors for the next superstep"。API：Python `WorkflowBuilder(start_executor=...).add_edge(a, b).build()`、`Executor` 子类 + `@handler`、`WorkflowContext`（`ctx.send_message()` / `ctx.yield_output()` / `ctx.set_state()` / `ctx.get_state()`）、`add_fan_in_edges([...], target)`；C# `AddEdge` / `AddFanInBarrierEdge` / `AddFanOutEdge` / `[MessageHandler]` / `IWorkflowContext`。边类型覆盖 direct / conditional / switch-case / fan-out / fan-in。
2. **工具/异步**：**这是本报告里唯一原生支持"挂起等外部输入并原地恢复"的**。
   - 原语：Python `await ctx.request_info(request_data=..., response_type=...)` + `@response_handler`（"The framework matches incoming responses to the correct handler based on the type annotations"）；C# `RequestPort.Create<TReq,TResp>("Name")` → 发出 `RequestInfoEvent` → `handle.SendResponseAsync(requestInputEvt.Request.CreateResponse(value))`；事件流侧 `WorkflowEvent.type == "request_info"`（C# 类型 `RequestInfoEvent`）。
   - 语义："A `RequestPort` defines a typed request/response channel between the workflow and the outside world. When an executor reaches a request port, **the workflow pauses and emits an external request event. The workflow resumes when an external response is provided**." Go 侧有 `WatchUntilHalt` "to return at a pause"，且 "A run can halt while waiting for external input, not only when its work is complete."
   - 等待期间不烧 token："**Pause for human input or external events without consuming compute or model tokens while waiting**"（durable extension 页）。
   - 工具审批也走同一通道：agent 使用需审批的工具时 `RequestInfoEvent` 携带 `ToolApprovalRequestContent`（Python: `type == "function_approval_request"`）。
   - 同步式调用仍在：`workflow.run(input)` 阻塞到终态（或到 halt），`workflow.run(input, stream=True)` 逐事件；`RunStreaming` 时 "WatchStream waits while external requests are pending"。
3. **持久化与重启恢复**：**最强，且在途工作可以恢复**。
   - 开关：Python 建 workflow 时传 `checkpoint_storage=InMemoryCheckpointStorage() | FileCheckpointStorage(path) | CosmosCheckpointStorage(...)`（Python 另有 pending-request 相关能力见下）；C# 用 `CheckpointManager.CreateInMemory()` 传给 `InProcessExecution.RunStreamingAsync(workflow, input, checkpointManager)`；Go `inproc.Default.WithCheckpointing(checkpointManager)`。
   - 颗粒度原文："**Checkpoints are created at the end of each superstep**, after all executors in that superstep have completed their execution. A checkpoint captures the **entire state of the workflow**, including: The current state of all executors / **All pending messages in the workflow for the next superstep** / **Pending requests and responses** / Shared states"。→ **pending request 被写进检查点**，这正是"重启后接着等/接着跑"的基础。
   - 1.13.0 起额外加 "entry checkpoint"，"These checkpoints make the complete workflow run **replayable**"。
   - 恢复 API：Python `async for event in workflow.run(checkpoint_id=saved.checkpoint_id, stream=True)`；C# `run.RestoreCheckpointAsync(cp)`（同一 run）或 `InProcessExecution.ResumeStreamingAsync(newWorkflow, savedCheckpoint, checkpointManager)`（新 run = rehydrate）；Go `run.RestoreCheckpoint(ctx, cp)`。列表：`checkpoint_storage.list_checkpoints(workflow_name=...)`、`get_latest()`。
   - 约束：rehydrate 必须 "preserve the topology and **executor identities**"——agent 必须给稳定 `ChatClientAgentOptions.Id`（.NET 的 executor 身份 = Name + Id），"otherwise the executor ids no longer match the checkpoint and resume fails"。
   - 托管级持久化：**Durable Extension**（`agent-framework-durabletask` / `Microsoft.Agents.AI.DurableTask` + Durable Task Scheduler；或 `agent-framework-azurefunctions`），"Persist state automatically across requests and worker executions / **Resume after failures without losing conversation context or repeating completed work** / Scale across distributed, stateless workers"，"Checkpoint Agent Framework workflows built with the graph-based workflow model"。
4. **关键路径 LLM 调用次数 / 确定性路径**：**可为 0**。原话："This function builds a text-processing workflow **without requiring a model or credentials**"（纯 `Executor` 图 = 确定性数据流）。含 agent executor 时，每次 executor 被激活 ≈ 1 次模型往返；同一 superstep 内 fan-out 的 N 个 executor 并发 → N 个并发 LLM 调用后统一等在屏障处。`Agent` 单体的工具循环 "is multi-turn by default and keeps invoking tools until it can return a final answer"（对比 AutoGen 的 single-turn）。Orchestration builder（`Sequential`/`Concurrent`/`GroupChat`/`Handoff`/`Magentic`）是预置图，`Handoff`/`GroupChat` 让模型决定下一跳，`Sequential`/纯 executor 图则是确定性的。
5. **谁管状态**：**框架**。`WorkflowContext.set_state/get_state`（workflow-scoped，downstream 可见）、`QueueStateUpdateAsync`/`ReadStateAsync`（scope-aware，"The executor that calls QueueStateUpdateAsync can read the updated value immediately in the same handler. **Other executors see that update starting in the next superstep**"）、pending requests、superstep 缓冲全部由框架持有并可被 checkpoint 捕获。模型只在单个 executor 内部做一次推理。Workflow 对象不可变（"workflows are immutable... no public API to modify a workflow"），builder 可变，官方建议每请求新建实例以避免状态串味。
6. **一句话定性**：**显式图状态机**——executor=状态/处理单元、edge=转移、superstep=事务边界（Pregel/BSP + 同步屏障）、checkpoint=事务日志；HITL 靠 typed request port 挂起而非阻塞线程，是本报告三家里唯一做到"挂起→序列化→换进程恢复"的。

**未能证实**：
- `/user-guide/workflows/`、`/workflows/execution-model`、`/durable-task`、`/user-guide/durable-task` 全部 **404**（正确的执行模型说明在 `concepts/workflows/builder-and-execution`）。
- `*.md` 变体（如 `/workflows/checkpoints.md`）**404**；MS Learn 的 "Copy Markdown" 源未直连成功。
- 迁移指南目录里有 "Checkpointing and Resuming Workflows / Resuming from Checkpoints" 章节，但未逐段读取，故 **AutoGen `TeamState` 与 AF checkpoint 之间的映射关系未证实**。
- "AutoGen 的 save_state 能否直接喂给 AF checkpoint" —— 未找到一手来源；迁移指南只说两条 rubric 并列（"Agent Framework Checkpointing / Resuming from Checkpoints"），未给等价性断言。
- 未找到 AF agent（非 workflow）层面的 `save_state` 等价单 agent 快照 API 名称（只有 `AgentSession` 概念出现在迁移指南目录中，未抓正文）。

---

## AgentScope（doc.agentscope.io "Stable(v1.0)"；v0.1.x 文档站已 404，退回官方仓库 v0.1.6 文档源）
**来源**:
- https://doc.agentscope.io/ （200）
- https://doc.agentscope.io/_sources/tutorial/task_pipeline.rst.txt （200，sphinx 干净源；对应 /tutorial/task_pipeline.html）
- https://doc.agentscope.io/_sources/tutorial/task_state.rst.txt （200）
- https://doc.agentscope.io/_sources/tutorial/task_agent.rst.txt （200）
- https://doc.agentscope.io/_sources/tutorial/task_tool.rst.txt （200）
- https://doc.agentscope.io/_sources/tutorial/task_plan.rst.txt （200）
- https://raw.githubusercontent.com/agentscope-ai/agentscope/v0.1.6/README.md （200，v0.1.6 官方仓库）
- https://raw.githubusercontent.com/agentscope-ai/agentscope/v0.1.6/docs/tutorial/en/source/index.rst （200）

1. **控制流单元**：**`Msg` 对象 + Python 层的直接 `await agent(msg)` 调用**。v1.0 没有 actor runtime、没有 broker、没有 superstep。编排只是语法糖："For multi-agent orchestration, AgentScope provides the `agentscope.pipeline` module as **syntax sugar** for chaining agents together"：`MsgHub`（广播）、`sequential_pipeline` / `SequentialPipeline`（串行链：前一个输出=后一个输入）、`fanout_pipeline` / `FanoutPipeline`（同一输入分发，`enable_gather` 控制并发/串行）、`stream_printing_messages`。
2. **工具/异步**：全 async；工具调用 `await toolkit.call_tool_function(ToolUseBlock(...))`；`parallel_tool_calls` 参数打开并行工具（"The parallel tool execution in `ReActAgent` is implemented based on **`asyncio.gather`**"）。
   - **MsgHub 是广播，不是 barrier**："The `MsgHub` class is an **async context manager**, receiving a list of agents as its participants. When one participant generates a replying message, all other participants will receive this message by **calling their `observe` method**. That means within a `MsgHub` context, developers don't need to manually send a replying message from one agent to another. The broadcasting is automatically handled." 支持 `announcement=`（进入上下文即广播一次）、动态 `hub.add(...)` / `hub.delete(...)` / `await hub.broadcast(Msg(...))`；"The newly added participants will **not** receive the previous messages"。**无 rendezvous / 无"等所有人到齐"语义**；并发与否完全看你有没有把它们 gather 起来（示例里是 `await alice(); await bob(); await charlie()` 顺序调用）。
   - **"稍后再回来"**：无 future / request-response 原语。只有两条：`interrupt()`（取消）与重入一次 `await agent(msg)`。
   - **中断/恢复**：有,但是**取消**不是挂起："The realtime steering allows user to interrupt the agent's reply at any time, which is implemented based on the **`asyncio` cancellation mechanism**. Specifically, when calling the `interrupt` method of the agent, it will **cancel the current reply task**, and execute the `handle_interrupt` method for postprocessing."（`AgentBase.__call__` 捕获 `asyncio.CancelledError` → `await self.handle_interrupt(...)`；`ReActAgent` 默认返回固定话术 "I noticed that you have interrupted me..."）。工具层同理，`ToolResponse.is_interrupted=True`，流式/非流式都支持。
3. **持久化与重启恢复**：有快照，**无检查点**，不恢复在途任务。
   - `StateModule`：`register_state(attr_name, custom_to_json=, custom_from_json=)`、`state_dict()`、`load_state_dict(state_dict, strict=)`；嵌套自动递归（"all the attributes that inherit from `StateModule`" 自动纳入）。`AgentBase` / `MemoryBase` / `LongTermMemoryBase` / `Toolkit` 全部继承 `StateModule`。
   - 实际快照内容：`{"memory": {"_compressed_summary": "", "content": [...]}, "toolkit": {"active_groups": []}, "name": "Friday", "_sys_prompt": "..."}` —— 就是 memory + 工具组 + 名字 + 系统提示。
   - 会话级：`SessionBase` 抽象出 `save_session_state` / `load_session_state`；内置 `JSONSession`，用法 `await session.save_session_state(session_id="user_1", agent=agent, ...)` / `await session.load_session_state(session_id="user_1", agent=agent, ...)`，"The keyword argument must be the same as the one used in `save_session_state`"。
   - **不恢复在途 reply**：跑的是 asyncio task（`self._reply_task = asyncio.current_task()`），没有任何序列化入口；session 里只有 memory/toolkit/prompt。→ 重启后只能从"最后一条已写进 memory 的消息"继续。
4. **关键路径 LLM 调用次数 / 确定性路径**：`ReActAgent` 是 reasoning-acting 循环，一次 `await agent(msg)` = **≥1 次模型调用**，每次工具结果回灌再来一轮（工具本身可并行）。**没有确定性路径**：没有图/状态机 DSL；`sequential_pipeline` / `fanout_pipeline` 只固定**调用次序**，被调 agent 内部仍是一次推理；连 `Plan` 模块都是"providing tool functions for plan management" + "inserting hint messages to guide the ReAct agent"，即通过提示词让模型推进（且自承限制："The subtasks in a plan **must be executed sequentially**"）。
5. **谁管状态**：**模型主导控制流**（ReAct 循环、Plan 都是提示词驱动），框架只提供 `StateModule`/`SessionBase` 做**被动快照**。没人持有"下一步该谁"的游标。
6. **一句话定性**：**轻量 asyncio 多 agent 编排库**——`MsgHub` 做广播（≈pub-sub 的简化版，非屏障）、`*_pipeline` 做顺序/并发糖；**不是状态机、不是 actor runtime、没有超级步、没有检查点**；"中断"=asyncio cancel，"恢复"=读 session 重跑。

**未能证实**：
- v0.1.x 文档站全部 **404**（`doc.agentscope.io/v0/en`、`/v0/`、`/v0.1.6/`、`/v0en/`、`agentscope.readthedocs.io/en/v0.1.6/` 均 404；站内版本切换器 `navigateToV0()` 指向的 `/v0/en` 是死链）。故 `to_dist` / `msghub` / `sequential_pipeline` 的正式 API 文档只能引用 **v0.1.6 官方仓库 README**（200）：`to_dist` 原话 "Using `to_dist` function to run the agent in **distributed mode**!"，示例 `DialogAgent(...).to_dist()`，"The two agent will run in parallel"；`msghub` 示例 `with msghub(participants=[...], announcement=Msg(...)) as hub: sequential_pipeline([...], x=None)`。
- v1.0 稳定文档中搜索索引 `searchindex.js` 内 `to_dist` / `distributed` 命中数 **0**，即**分布式 actor 模式在 v1.0 文档中已不再出现**；是否有替代实现（A2A？）**未证实**。
- "MsgHub 是否为 barrier" 没有官方否定语句，只能从 `observe` 广播语义推断为**非屏障**。
- `ReActAgent` 的默认迭代上限（`max_iters`）**未找到文档化数值**。
- `doc.agentscope.io/search.html` 返回 200 但结果由 JS 渲染，未采信。
- GitHub REST API `api.github.com` 返回 **403**（被限流），仓库目录只能靠 `raw.githubusercontent.com` 已知路径探测。
