# 三个框架的一手文档笔记：LlamaIndex Workflows / Pydantic AI (pydantic-graph) / Mastra Workflows

> 说明：全部结论来自本机 `curl` 实际抓取成功（HTTP 200）的一手文档；markdown 源优先（`.md` / `index.md` / `llms.txt`）。
> 版本号取自官方 PyPI / npm registry JSON（HTTP 200）。未抓到的页面记为「未找到一手来源」。

## A. LlamaIndex Workflows (llama-index-workflows 2.26.1；PyPI `requires_python >=3.10`，文档站为 2025 版式)

**来源**: https://developers.llamaindex.ai/python/llamaagents/workflows/ , https://developers.llamaindex.ai/python/llamaagents/workflows/durable_workflows/ , https://developers.llamaindex.ai/python/llamaagents/workflows/managing_state/ , https://developers.llamaindex.ai/python/llamaagents/workflows/human_in_the_loop/ , https://developers.llamaindex.ai/python/llamaagents/workflows/streaming/ , https://developers.llamaindex.ai/python/llamaagents/workflows/concurrent_execution/ , https://developers.llamaindex.ai/python/llamaagents/workflows/dbos/ , https://developers.llamaindex.ai/python/llamaagents/workflows/retry_steps/ , https://developers.llamaindex.ai/python/llamaagents/workflows/branches_and_loops/ , https://developers.llamaindex.ai/python/llamaagents/workflows/async_workflows/ , https://developers.llamaindex.ai/llms.txt , https://pypi.org/pypi/llama-index-workflows/json
（旧的 https://docs.llamaindex.ai/en/stable/understanding/workflows/ 与 https://developers.llamaindex.ai/python/framework/understanding/workflows/ 均 HTTP 200，但 `url_effective` 已重定向到上面这个 llamaagents/workflows 页面，内容一致，即旧 URL 只是同一 canonical 页的入口。）

1. **控制流单元**: step 是唯一执行单元，event 是唯一边。
   - `A workflow is an event-driven, step-based way to control the execution flow of an application.`；`A step receives an event, does some work, and returns another event.`
   - 边由 `@step` 读类型注解自动推导：返回类型 = 出边事件类型，参数类型 = 入边事件类型；不显式声明边。
   - 分支 = 普通 `if` 返回不同事件类型；循环 = 返回被更早 step 消费的事件类型；并发 = 返回 `list[Event]` 配一个接受 `list[Event]` 的 step（fan-out/fan-in）。`@step(num_workers=N)` 控制单 step 并发副本数；`Workflow(num_concurrent_runs=N)` 控制整个 run 的并发。
   - 特殊事件：`StartEvent`（可带任意属性）、`StopEvent`（可子类化做自定义退出点）；另有 `WorkflowTimedOutEvent` / `WorkflowCancelledEvent` / `WorkflowFailedEvent` 在流上发布。
   - 代码就是 async Python 类 + decorator，**没有 DAG 对象**：文档明确把 DAG（含它自己老方案）列为被替代方案。

2. **工具/异步与「稍后回来」原语**: 三种，语义差别很关键。
   - (a) **动态发送事件（推荐用于 fan-out）**：`ctx.send_event(Ev(...))` 立即发出、step 可 `return None` 马上结束；配套 `ctx.collect_events(ev, [T]*n)` 做手工 fan-in。文档点明代价：`Downstream steps can start before the producer step is done, but anything already sent stays out even if the step later fails.`（对比 `list` 返回是 `all or nothing`——step 报错则一批都不发）。签名里仍应写上被发送的事件类型，否则 `static validation and visualization cannot infer that edge`。
   - (b) **人在环事件对（官方首选）**：一个 step 返回 `InputRequiredEvent`，另一个 step 消费 `HumanResponseEvent`；调用侧 `handler.stream_events()` 里看到 `InputRequiredEvent` 就 `await handler.send_event(HumanResponseEvent(...))`。这是真正的「step 立即返回、稍后事件把它续上」——中间 run 一直活着等事件。
   - (c) **`ctx.wait_for_event(...)`（单 step 内等待）**：`await ctx.wait_for_event(HumanResponseEvent, waiter_event=InputRequiredEvent(...), waiter_id="get_number", requirements={...})`。但文档自己给了强警告：`wait_for_event replays all code preceding it whenever the step receives its triggering event`，且 `The step always runs at least once up to the waiter, which then raises an internal exception to pause execution. Any code before the wait_for_event call must be safe to repeat.`——因此 `Due to this complexity, the event-based approach with separate steps is generally recommended.`（`waiter_id` 用于同一 step 多次等待；`requirements` 用于同类型事件路由到正确 waiter。）
   - (d) **流式**：step 内 `ctx.write_event_to_stream(ProgressEvent(...))`，外部 `handler.stream_events()`；`run` 后台调度。`A handler stream can be consumed once.`；`stream_events(expose_internal=True)` 还能看到内部事件（用于 checkpoint）。

3. **持久化与重启恢复**: **官方正式文档化，而且明确写了「可恢复」而非「仅调试」**，但边界写得很具体。
   - 存在性确认：`Context.to_dict()` / `Context.from_dict(w, d)` / `w.run(ctx=ctx)` 三件套。文档原话：`That state is the events still in flight plus the state store. Context.to_dict() serializes it, Context.from_dict() rebuilds it, and run(ctx=...) continues from there, even in a different process`。→ **确实捕获 in-flight 状态（未消费的事件 + fan-in 缓冲 + store）**，不只是 store。
   - 可恢复性的诚实边界（逐条直引）：
     - `This survives a restart, but not a crash during the run. You only have the state as of the last to_dict().`
     - `On resume, the restored run re-dispatches the events that were still pending and rebuilds the partial fan-in buffers. Completed steps don't re-run... A step that was mid-execution when you captured the state is rewound and runs again from the top.` → `Resume is at-least-once, and step side effects need to be safe to repeat.`
     - `Workflow instance attributes are not part of the snapshot.`（挂在 workflow 实例上的东西不是 durable state）
     - 不是「打开开关」：`There is no built-in checkpointer to enable.` 你得自己订阅内部事件：`stream_events(expose_internal=True)` + `isinstance(ev, StepStateChanged) and ev.step_state == StepState.NOT_RUNNING` 时落盘。
     - 序列化是 JSON：`Snapshots use a JSON serializer, Pydantic models included. If it hits a value it can't encode, to_dict() raises and the whole snapshot fails, not just that field.`；非可序列化客户端必须放 `Resource`（`A Resource factory is resolved once, cached on the workflow, and never serialized into the context. On resume it is re-created by calling the factory again.`）。
     - `run(ctx=ctx)` 双语义：`If the context is still running, run(ctx=ctx) resumes that run and does not send a new StartEvent. If the previous run has completed, run(ctx=ctx) starts a new run with the same stored state.`
   - **换 run 的 HITL 手册**：snapshot → `await handler.cancel_run()` → 新进程 `Context.from_dict` → `run(ctx=...)` → `send_event(HumanResponseEvent)`。文档要求主动 cancel：`That avoids leaving the in-memory run waiting for an answer that will be delivered to the restored run.`
   - **runtime plugin 路线（更接近「框架托管」）**：`llama-agents-dbos` + `DBOSRuntime`，`Every step transition is persisted automatically`，`idle_timeout` 可把空闲 HITL run 从内存里释放、来事件时自动还原。其代价同样直说：`Because recovery is replay-based, **steps may execute more than once** if they were interrupted before the journal entry was committed.`；代码改了导致非确定性 → 要么 drain，要么 `workflow_name="counter-v2"` 换身份；in-process `handler.stream_events()` 在 DBOS 下会从头重放（`you will receive all events the workflow has ever emitted`）。
   - retry 与幂等：`Retries re-run the whole step body for the same input event. Keep side effects before the failing call idempotent`（与 at-least-once 恢复同构）。

4. **关键路径上的 LLM 调用 / 确定性路径**: 框架不管。
   - step 就是普通 async 函数，LLM 调用是里面的 `await self.llm.acomplete(...)`；没有「必须把 LLM 放某个特殊节点」的约束，也没有为 LLM 特化的确定性切分（对比 Temporal 的 workflow/activity 二分）。
   - 流式 LLM token 走 `ctx.write_event_to_stream`（`astream_complete` 循环里逐 delta 写），不影响 step 返回结构。
   - 确定性只以「副作用要可重复」的形式出现（at-least-once 恢复 + retry policy 重跑整个 step body），默认 runtime 不要求 step 是 deterministic 的；只有 DBOS runtime 因为 journal replay 才引入代码版本非确定性风险。

5. **谁管状态**: 框架管「run 级」执行状态，**完全不管模型状态**。
   - 框架持有：`Context`（in-flight 事件、fan-in 缓冲、untyped 或 Pydantic typed state store `ctx.store`、事件流）、step 生命周期、retry 计数。
   - 并发写状态需自己加锁：`async with ctx.store.edit_state() as state:`（`No other step can edit the state while the block is running`）。
   - 模型侧：没有任何 message history / conversation 抽象出现在 workflows 文档里；LLM 只是被 step 调用的外部依赖，要持久化就把结果放进 event 或 store。
   - 重对象归属：`Resource` 工厂，实例级缓存、不进快照、resume 时重建。

6. **一句话定性**: 一个把「step + 类型化事件」当唯一抽象、把「断点续跑」写成你可以自己实现的 20 行循环（`to_dict`/`from_dict` + `StepStateChanged` 钩子）的库——**官方承认 resume 是 at-least-once、跑到一半的 step 会重头再来**：`Resume is at-least-once, and step side effects need to be safe to repeat.`

**未能证实**:
- 「workflows 1.0」官方博客文章（未抓取；本笔记不引用）。
- 是否有 `python/llamaagents/workflows/index.md` 之外的「serializing_state」专页——该路径 404；序列化内容只集中在 `durable_workflows` 与 `managing_state` 两页。
- API reference 页面 `https://developers.llamaindex.ai/python/workflows-api-reference/context/`（human_in_the_loop 页内链）抓取 404，故 `wait_for_event` 的完整签名只能以教程页示例为准。
- 跨进程/多副本下的默认 runtime 行为（除 DBOS 插件页外无一手描述）。
- `llama-index-core` 对 workflows 的版本绑定关系（未抓 PyPI 的 core 元数据）。

---

## B. Pydantic AI / pydantic-graph (pydantic-ai-slim 2.64.0, pydantic-graph 2.64.0)

**来源**: https://ai.pydantic.dev/ , https://ai.pydantic.dev/graph/ , https://ai.pydantic.dev/graph/beta/ , https://pydantic.dev/docs/ai/graph/graph/ , https://pydantic.dev/docs/ai/graph/builder/ , https://pydantic.dev/docs/ai/graph/builder/steps/ , https://pydantic.dev/docs/ai/graph/builder/joins/ , https://pydantic.dev/docs/ai/capabilities/durable_execution/overview/ , https://pydantic.dev/docs/ai/capabilities/durable_execution/temporal/ , https://pydantic.dev/docs/ai/capabilities/durable_execution/dbos/ , https://pydantic.dev/docs/ai/tools-toolsets/deferred-tools/ , https://pydantic.dev/docs/ai/tools-toolsets/tools/ , https://pydantic.dev/docs/ai/core-concepts/persistence/ , https://pydantic.dev/docs/ai/harness/step-persistence/ , https://pydantic.dev/docs/ai/harness/background-tools/ , https://ai.pydantic.dev/llms.txt , https://pypi.org/pypi/pydantic-graph/json
（注意：文档已整体迁到 `pydantic.dev/docs/ai/...`，`ai.pydantic.dev/*` 是 200 的重定向入口；`https://ai.pydantic.dev/graph/beta/` 现在 200 并 302 到 `/graph/builder/`，所以「新 graph API / beta」= **GraphBuilder**，不是叫 `StateMachine` 的类。）

1. **控制流单元**: 两套并存、互操作、官方都说 supported。
   - **老 API（class-based state machine）**：`BaseNode[StateT, DepsT, RunEndT]` dataclass 子类 + `async def run(self, ctx: GraphRunContext[StateT, DepsT]) -> NextNode`；返回 `End(value)` 表示结束。**边由 `run()` 的返回类型注解推导**——即 type hints 就是 edge。`graph.run(state=state)` 返回裸输出值。核心类型齐备：`GraphRunContext`(带 `.state` / `.deps`)、`End`、`BaseNode`。
   - **新 API（builder，原 /graph/beta/）**：`GraphBuilder(state_type=, deps_type=, input_type=, output_type=)` + `@g.step` async 函数（拿 `StepContext`：`ctx.state` / `ctx.deps` / `ctx.inputs`）+ 显式边 `g.add()/g.add_edge()/g.edge_from()` + `g.join(reducer, initial_factory=)` / decision / `.map()` / broadcast，最后 `g.build()` → `await graph.run(state=, inputs=)`。
   - 定性直引：`pydantic-graph -- an async graph and state machine library for Python where nodes and edges are defined using type hints.`；`Reach for pydantic-graph when control flow is the hard part: explicit states, branches and resumable transitions you want typed and diagrammed.`；`it has no dependency on pydantic-ai and can be considered as a pure graph-based state machine library.`
   - 官方还很直白地劝退：`Don't use a nail gun unless you need a nail gun` / `If you're not confident a graph-based approach is a good idea, it might be unnecessary.`
   - typed state 是显式的：`StateT` 泛型贯穿 builder 与 node；state 是**用户自己 new 出来再传进去的可变对象**（`state = CounterState()` → `graph.run(state=state)`），不是框架内部持有的。

2. **工具/异步与「稍后回来」原语**: Agent 层有真正的「先返回、稍后再续」；graph 层只有「手工步进」。
   - **Deferred tools（官方核心原语）**：工具体 `raise ApprovalRequired`（需审批）或 `raise CallDeferred`（交给外部执行）。两种解析方式：
     - inline：`HandleDeferredToolCalls` capability 带 handler，`the agent run continues in a single call`；
     - stop-the-world：run **结束**并把 `DeferredToolRequests` 作为 output 返回，调用方拿到结果后用「原 message history + `DeferredToolResults`」**起一个全新的 run**。直引边界：`That follow-up is a separate agent run with its own run_id (do not reuse the paused run's); keep pause/resume correlation via conversation_id.`
     - 要求 `DeferredToolRequests` 出现在 `output_type` 里，否则类型推断不成立；两种方式可组合（handler 解一部分，剩下的冒泡出去）。
   - **BackgroundTools**（Harness）：`lets selected tools run in the background while the agent continues without waiting`，结果作为 follow-up message 回灌。注意：`Realtime sessions already run tools concurrently, so BackgroundTools leaves them unchanged.`
   - **graph 层**：没有 wait-for-event 原语；只有 `graph.iter()` 手工步进：`async with graph.iter(state=state) as graph_run: async for event in graph_run:`，配合 `graph_run.next_task` / `.output` / `.next()` / `override_next()`（`Yielded by the graph iterator instead of raising immediately, allowing the caller to recover by sending new tasks via GraphRun.next() or GraphRun.override_next()`）。→ 这是「人当调度器」的暂停，**不是**持久化暂停。

3. **持久化与重启恢复**: 这是三个框架里**最诚实、也最割裂**的一个：graph 层明确不做，durable execution 层另买。
   - **graph 层：明确「不快照」。** 一级标题直引：`Graph state is not snapshotted` / `Neither the graph builder API nor the original Graph API snapshots graph state; pydantic_graph.persistence was removed in V2, because of the complexity of achieving consistent snapshotting with parallel execution`。官方给的替代路径是 StepPersistence（管 agent run）或 durable engine（管整个 run 存活）——**没有** graph 级 checkpoint。
   - **durable execution：靠外部引擎，8+1 家。** 目前列表为 Temporal / DBOS / Prefect / Restate / AWS Lambda / Absurd / pgtask / Apache Airflow，外加 backend builder。核心声明：`A durable engine journals the agent's model requests and the tool calls it wraps, and a restarted run replays the completed ones from the journal and retries the one in flight.`——**「哪一个不是 durable」是引擎相关的，框架只说「我包了哪些」**：`Which tool calls (function tools, MCP servers) an engine wraps for you and how it delivers streaming differ per engine`。且 `One durable execution engine per agent`（挂第二个直接 `UserError`）。
   - **「durability ≠ storage」的显式切分**（overview 页 Pull-quote）：`Durability is not storage` — `Durability is not storage / A durable engine keeps one run alive when it crashes partway through a step... Saving a run, resuming it later and forking it from an earlier step need no engine: StepPersistence from Pydantic AI Harness snapshots every settled step to a store you choose, the way a checkpointer does`。
   - **Temporal 页的 not-durable 直引**：
     - `Critically, workflow code generally cannot include any kind of I/O, over the network, disk, etc. Activity code faces no restrictions on I/O or external interactions, but if an activity fails part-way through it is restarted from the beginning.`
     - `Capability listeners registered with @on_event run in workflow code rather than in a durable unit, so they re-run on every workflow replay and must be deterministic.`
     - 流式非幂等：`Model events are published from inside the model-request activity, so if that activity retries, its events are published again at new offsets. Consumers should tolerate duplicates.`
     - 取消/超时跨不过边界：`Because the model stream is consumed inside the activity, cancelling it from the workflow side is not available across the durable boundary.`；`RunContext.cancel() requires being in the same process as the run`；`CancellationToken is also same-process state and cannot be passed to a Temporal durable run`。
     - activity 内 `RunContext` 是残缺版：`tool_manager is None`，`ctx.enqueue()` 与 `ctx.load_capability()` 在 activity 内直接 raise；`deps` 必须 Pydantic 可序列化（`it's your responsibility to make sure that the dependencies object provided to Agent.run() can be serialized`）。
   - **DBOS 页的 not-durable 直引**：
     - `Custom tool functions and event stream handlers registered on the agent directly or through another capability are **not automatically wrapped** by DBOS.`
     - 常见静默失效：`DBOSDurability deliberately does not [wrap run] -- a run is only durable when agent.run() is called inside your own @DBOS.workflow. Porting the constructor arguments but calling agent.run() directly produces a run that works but is **not durable**.`
     - 并发 step 的确定性硬约束：`DBOS requires durable steps to start in a deterministic order. When tools run concurrently, a tool that awaits one durable operation before starting another can interleave differently with sibling tools or their emitted-event handlers during replay, causing recovery to fail.`
     - 流式丢 delta：`the deltas aren't recorded in the step's result`；`Agent.run_stream() 和 run_stream_events()` 的 `events are buffered rather than delivered in real time`。
     - step 内 emit 不重放：`Events emitted with ctx.emit() from inside a DBOS step ... are not re-emitted when its recorded result is replayed on recovery.`；`ctx.enqueue()` 在 step 内被拒。
     - `If a step fails, it restarts from the beginning.`
   - **StepPersistence（无需引擎的「快照续跑」）的边界直引**：
     - `A snapshot holds the run's message history, not everything around it: capability state outside the messages, workspace files, and resuming from the middle of a step are tracked separately`
     - `A step that was in flight is not retried: its tool calls show up as started effects in the ledger, and you decide whether redoing them is safe.`
     - `a tool with a started record and no terminal update should be treated as unknown_after_crash: the side effect may or may not have happened`
     - `interrupted` 快照 `is sendable, but not necessarily _safe_`；`The default read path skips interrupted snapshots`（需 `include_interrupted=True`）。

4. **关键路径上的 LLM 调用 / 确定性路径**: 唯一做了显式「确定 vs 非确定」二分的框架，但二分发生在**引擎层**，不在 pydantic-graph 层。
   - Temporal：`The key to making this work is to separate the application's repeatable (deterministic) and non-repeatable (non-deterministic) parts`——deterministic = workflow（agent 编排循环），non-deterministic = activity（model request / tool call / MCP）。`integration with Temporal means that model requests, tool calls that may require I/O, and MCP server communication all need to be offloaded to Temporal activities`。
   - DBOS：`**Workflows** must be deterministic and generally cannot include I/O.` / `**Steps** may perform I/O`。
   - pydantic-graph 本身对 LLM 零假设：node/step 就是普通 async 函数，里面 `await agent.run(...)` 与 `await http.get(...)` 无区别；LLM 是「用户的选择」，不是框架节点类型。model 请求作为 durable unit 是引擎 wrap 的结果。
   - 结果：Agent 层（`Agent.run` 的 tool-calling 循环）是框架强制的，graph 层是用户强制的。

5. **谁管状态**: 三份状态，归属各不相同，且官方把它们分得很清。
   - **graph state**：用户的（`StateT` 可变对象，自己在外面 new，`graph.run(state=state)` 内部 mutate）。框架不持有、不快照。
   - **deps**：用户的，typed DI（`deps_type`），在 Temporal 下必须 Pydantic 可序列化。
   - **message history / model state**：框架生产（`ModelMessage` 序列，可 `as JSON` 存取），持久化归用户；`StepPersistence` 只是把它按 step 边界写到你自己选的 store（in-memory / file / SQLite / MongoDB / 自定义 protocol）。
   - **run 的存活**：只有引擎管（journal），且引擎是外部的（Temporal/DBOS/...）。`One durable execution engine per agent`。

6. **一句话定性**: 唯一一个**官方明确写出「图状态不做快照、旧 persistence 在 V2 被删掉」**的框架——它把「原生状态机」和「崩溃存活」彻底切成两层卖：`Graph state is not snapshotted` / `Neither the graph builder API nor the original Graph API snapshots graph state; pydantic_graph.persistence was removed in V2`。

**未能证实**:
- 名为 `StateMachine` / `GraphBuilder`(beta) 的 `StateMachine` 类：**不存在**；`/graph/beta/` 只是 `GraphBuilder` 的旧入口。
- **Inngest 集成**：`llms.txt` 的 Durable Execution 章节只列 Temporal / DBOS / Prefect / Restate / AWS Lambda / Absurd / pgtask / Airflow，**未出现 Inngest**；Inngest 未找到一手来源。
- `pydantic_graph.persistence` 在 V1 的确切 API 形状（V2 已删，只在 builder 页被回溯提及）。
- Prefect / Restate / Airflow / Absurd / pgtask 各自页面的 caveats（未抓取，只抓了 overview 的清单表格）。
- `core-concepts/persistence` 与 `harness/step-persistence` 之外的「fork」UI 语义（未细读）。

---

## C. Mastra Workflows (@mastra/core 1.81.0，npm `latest` 发布于 2026-10-30T23:22:55Z)

**来源**: https://mastra.ai/docs/workflows/overview , https://mastra.ai/docs/workflows/control-flow , https://mastra.ai/docs/workflows/snapshots , https://mastra.ai/docs/workflows/suspend-and-resume , https://mastra.ai/docs/workflows/human-in-the-loop , https://mastra.ai/docs/workflows/workflow-state , https://mastra.ai/docs/workflows/time-travel , https://mastra.ai/docs/workflows/error-handling , https://mastra.ai/docs/workflows/agents-and-tools , https://mastra.ai/docs/deployment/workflow-runners , https://mastra.ai/integrations/deploy/inngest , https://mastra.ai/reference/workflows/run , https://mastra.ai/reference/workflows/workflow-methods/commit , https://mastra.ai/llms.txt , https://registry.npmjs.org/@mastra/core
（全部以 `.md` 变体命中 200，站点自述 `Mastra docs are the canonical, current reference.`）

1. **控制流单元**: step（`createStep`，typed `inputSchema`/`outputSchema`/`stateSchema`）+ 链式 DSL 组成的静态图。
   - 组合原语：`.then()` / `.branch()` / `.parallel()` / `.dountil()` / `.dowhile()` / `.foreach()` / `.map()` / `.sleep()` / `.sleepUntil()`，末尾 `.commit()` 才可用：`The .commit() method finalizes the workflow definition and prepares it for execution.` / `Creating a run on a workflow with uncommitted step changes throws an error.`
   - Schema 用 Standard JSON Schema（Zod / Valibot / ArkType 皆可）——类型既做运行时校验也做 TS 推断。
   - 与 LlamaIndex/Pydantic 相反：这是**显式构图 + 显式边**（DSL 即边），不是靠类型注解/runtime 事件路由推边。图结构还存在快照里（`serializedStepGraph`），所以**改图会让老 run 失效**（见 time-travel 的报错）。
   - 另有 `Dynamic Workflows`（reference 有 `DynamicWorkflowDefinition`），本次未细读。

2. **工具/异步与「稍后回来」原语**: `suspend()/resume()` 是**真原语**，而且是被快照支撑的。
   - 用法：step 的 `execute` 里 `return await suspend({...})`，payload 由 `suspendSchema` 定型；run 返回 `result.status === 'suspended'`，`result.suspended` 给出被挂起的 step 路径（嵌套 workflow 是 `['nested-workflow','step-1']` 这样的路径）。恢复：`await run.resume({ step: 'step-1', resumeData: {...} })`，`resumeData` 必须匹配 `resumeSchema`；`If only one step is suspended, you can omit the step argument entirely`。恢复后同一 `execute` **重新进入**，这次拿到 `resumeData`（及 `suspendData`，即当初 `suspend()` 的 payload），所以写法是 `if (!approved) return await suspend(...)`。
   - `resume()` 可以从任何地方调用（HTTP endpoint、`setTimeout`、人在环 UI、事件处理器）——这正是「返回后立刻回去」的形态：进程不用挂着。
   - 另一档：`.sleep()` / `.sleepUntil()` 是**workflow 级**暂停，状态是 `waiting`，不是 `suspended`（`Sleep methods can be used to pause execution at the workflow level, which sets the status to waiting. By comparison, suspend() pauses execution within a specific step and sets the status to suspended.`）。
   - 长任务取恢复句柄：`workflow.getWorkflowRunById('run-123')` + `createWorkflowStateReader(state)` → `reader.getSuspendedStep()` / `reader.getResumeLabel('approve')`（含 `foreachIndex`），再 `createRun({runId})` + `run.resume(...)`。
   - 流式：`run.stream()` → `fullStream` + `stream.result`。

3. **持久化与重启恢复**: 快照是**一等公民且内容清单被逐项列出**；恢复粒度 = step。
   - 快照定义直引：`a snapshot is a serializable representation of a workflow's complete execution state at a specific point in time. Snapshots capture all the information needed to resume a workflow from exactly where it left off, including:` → `The current state of each step` / `The outputs of completed steps` / `The execution path taken` / `Any suspended steps and their metadata` / `The remaining retry attempts for each step` / `Additional contextual data`。→ **明确包含「已完成 step 的输出」，不只是挂起点信息**。
   - 序列化形态（官方给了 JSON）：`runId`、`status`、`value`、`context`（每个 step 的 `payload` / `startedAt` / `status` / `suspendPayload` / `suspendedAt` / `resumePayload` / `resumedAt` / `output` / `endedAt`）、`activePaths`、`serializedStepGraph`、`suspendedPaths`、`waitingPaths`、`result`、`requestContext`、`timestamp`。
   - 存储：`Each snapshot is saved in the workflow_snapshots table and identified by the workflow's runId`；默认 libSQL，可换 Postgres / Upstash / OracleDB / MongoDB / D1 / DynamoDB（`LibSQLStore` 在示例里配 `storage`）。`Snapshots are stored in your configured storage provider and persist across deployments and application restarts.`
   - **崩溃恢复的真实机制与边界**（这是最该引的一段）：run 执行期间 Mastra 持有 storage lease 并后台续租；`restart()` 从 last active step 续跑；`The lease is advisory and doesn't fence writes. If a process stops renewing its lease but keeps executing a step, for example because its event loop is blocked for longer than the lease TTL, recovery can run that step a second time.` → **跑到一半的 step 可能被执行两次**（与 LlamaIndex 的 rewound、Pydantic/Temporal 的 activity restart 同构）。另有 `WORKFLOW_RESTART_RUN_OWNED` / `recovery.workflowConcurrency`（默认 5）/ `autoRestartActiveRuns: false`；`When running the local mastra server, all active workflow runs will be restarted automatically when the server starts.`
   - 时间旅行（`run.timeTravel({ step, inputData, context, initialState, resumeData })`）：从任意 step 重放，`Step results before the target step are reconstructed from the snapshot or provided context`；图变了就报错且 `the stored snapshot is left unchanged`；并发调用有 `WORKFLOW_TIME_TRAVEL_ALREADY_CLAIMED` / `WORKFLOW_TIME_TRAVEL_RUN_RUNNING`（原子 claim）。
   - **durable execution 是外挂的**：内置引擎默认；`managed infrastructure is available through workflow runners such as Inngest`。
     - Inngest：`Inngest executes it step by step and memoizes each result. On retry or resume, Inngest skips completed steps based on those saved results.`；要能扛崩溃必须显式设 `retries`：`Set retries to let a run survive a failed request to your app, such as a process restart, redeploy, or out-of-memory crash`；且 `retries defaults to 0. Errors thrown by your step code are retried per step with retryConfig or the step's retries option, not at the function level.` → **默认 0 retry = 不设就没有跨进程恢复**。
     - Temporal：`The @mastra/temporal package is experimental and not ready for production use.`（`each createStep mapped to a Temporal activity`）。
   - 快照纪律（官方 best practices）：`Ensure Serializability: Any data that needs to be included in the snapshot must be serializable (convertible to JSON).` / `Minimize Snapshot Size`。

4. **关键路径上的 LLM 调用 / 确定性路径**: 无确定性约束，也不区分模型调用与其他 I/O。
   - step 里可以直接跑 agent / tool（`.agent()` / `.tool()` helper，或自己 import 执行），`You control the task breakdown along with the flow and timing of data`。
   - 内置引擎没有 Temporal 那种 workflow/activity 确定性二分；文档也**没有**要求 step 幂等——幂等性只以「lease 可能让 step 跑两次」+「retry 会重跑 step」的形式隐含存在。
   - 外挂引擎改变了语义：Inngest 下 step 结果被 memoize，`skips completed steps`，所以 LLM 调用的「重放成本」由引擎的 memoization 决定，而不是 step 代码。

5. **谁管状态**: 框架管得最多——快照 + 共享 state + per-step 输入输出，三层都在框架里。
   - **step I/O**：`Data flows sequentially between steps. Each step receives the previous step's output as its inputData`（框架在快照里存每步 payload/output）。
   - **workflow state**：`A shared store that all steps can read and update via state and setState`，且 `State persists across the entire workflow run, including suspend/resume cycles.`（`stateSchema` 在 workflow 与 step 上分别声明，step 只声明自己那段）。
   - **model state**：不归 workflow 管——agent 自带 memory/thread，workflow 只把它当 step 里的调用。
   - **执行所有权**：storage lease（advisory，不 fence 写）——即「谁在跑这个 run」也是框架/存储层的事，而不是用户。

6. **一句话定性**: 三个里唯一把「运行状态」做成**完整、可枚举、落库、可时间旅行**的序列化对象的框架，但它的崩溃恢复粒度也只到 step，且官方承认租约只是建议性的：`The lease is advisory and doesn't fence writes. If a process stops renewing its lease but keeps executing a step ... recovery can run that step a second time.`

**未能证实**:
- 内置引擎是否**每完成一个 step 就写一次快照**：`snapshots` 页只说 `Snapshots are automatically created and managed by Mastra whenever a workflow is suspended`，而 `time-travel` 页又依赖「已记录 step 结果」的快照——两者粒度未在一手文档里对齐，**未找到明确陈述**。
- Mastra 是否有独立 `durable execution` 文档页（`/workflows/durable-execution` 未抓；`llms.txt` 里只有 `deployment/workflow-runners` 与 `harness/durable-agents`，后者是 agent 级而非 workflow 级）。
- `@mastra/temporal` 的具体映射语义（仅一句 "experimental"）。
- `agents-and-tools.md` / `dynamic-workflows` 的细读（已抓未逐段引用）。
- Mastra 内置引擎的跨进程 `startAsync()` 行为（未抓 `run-methods/startAsync`）。

---

## 三个横向对照（只基于上述直引）

| 维度 | LlamaIndex Workflows | Pydantic AI / pydantic-graph | Mastra |
|---|---|---|---|
| 控制流单元 | `@step` + 事件类型（边由注解推导） | `BaseNode.run()` 或 `@g.step`（边由返回类型 / 显式 `g.add`） | `createStep` + 链式 DSL（边显式） |
| 「先返回、稍后回来」 | `InputRequiredEvent`/`HumanResponseEvent`（首选）或 `ctx.wait_for_event`（有重放副作用警告） | deferred tools（run 结束 → 新 run + message history）或 `HandleDeferredToolCalls` inline | `suspend()`/`resume()`（step 内中断，快照落库） |
| 快照是否原生 | `Context.to_dict()/from_dict()`，官方文档化为可恢复 | **明确不快照图状态**（V2 删了 `pydantic_graph.persistence`） | 原生快照 + `workflow_snapshots` 表 |
| 恢复语义 | at-least-once，半途 step 重跑 | 引擎决定；Temporal activity 失败从头重来 | 租约建议性，step 可能跑两次 |
| 崩溃存活 | 需自己写 checkpoint 循环，或换 DBOS runtime | 需接外部引擎（Temporal/DBOS/…） | 内置 lease + restart sweep；真跨进程靠 Inngest `retries` |
| 确定性要求 | 默认无；DBOS runtime 下有 | 引擎层强制区分 workflow/activity | 无 |
| 模型状态归属 | 框架不管 | 框架产出 message history，持久化归用户 | workflow 不管，agent 自带 memory |
