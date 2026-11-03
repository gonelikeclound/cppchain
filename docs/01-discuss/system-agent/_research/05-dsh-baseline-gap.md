# DSH 现状基线：与 SystemAgent 相关的既有原语与缺口

> 一手来源：DSH 源码与 Agent Note（本地 checkout `/Users/dongjunjie/work/deepseek-harness`）。
> 用途：作为 `01-业界调研` 的对照基线，避免"以为要从零造"或"以为已经有了"。
> 结论先行：**DSH 已经有"句柄 + 完成通知 + 唤醒空闲者"这条链路（粗粒度事件），缺的是细粒度事件、持久化续体、状态机、咨询协议。**

---

## 1. 已有：`ctx.jobs` 就是"长任务工具运行时"

证据：`.agents/notes/implemented/architecture/2026-06-20-generic-long-running-tool-runtime.md`
与 `packages/jobs/jobs-local/README.md`。

它已经提供的（正是 SA 需要的异步骨架）：

| 能力 | 具体形态 |
|---|---|
| 立刻返回句柄 | `start()` 提交后返回 branded id `<kind>-N`，模型侧看到 `{ kind: 'background', jobId }` |
| 句柄不随工具调用死亡 | "Once registration publishes the id, cancellation belongs to the task's own controller and the job runtime: later cancellation of the producing tool call must not kill the published task." |
| 增量观测 | `readOutput()` 消费式游标 / final-output 幂等读 |
| 取消 | `kill()` → `stopping` → 生产者 `done` |
| 终态通知 | `onJobDone()` + 模型侧 `job_output`；终态只通知一次（first-wins settlement + `reported` 位抑制重复） |
| 空闲唤醒 | 见第 2 节 |
| 归属隔离 | 按 exact owner（`SessionId` 授权 + 活对象选清理），`maxConcurrentJobsPerOwner` 默认 10 |

**关键局限（原文）**：
- "**Jobs are process-local** — records die with the harness process; durable or cross-restart execution needs a separate backend."
- "Work intended to outlive an agent must be started unowned; **survival across runtime restarts requires a separate durable-job design**."
- 粒度：**只有终态事件**。运行中途的 `ringing` / `connected` / `dtmf` 这类事件，`ctx.jobs` 没有对应概念（`readOutput` 是输出增量，不是语义事件）。
- 流式读取只有一个消费者游标："Stream reads support one consuming reader"。

## 2. 已有：完成通知会"唤醒空闲的 owner"

证据：`.agents/notes/implemented/feature/2026-08-11-background-job-completion-wakes-an-idle-owner.md`。

这条 note 记录了一次真实的失败，几乎是本专题最好的实证材料：

> "The common shape is exactly the one that breaks: **the model starts a long command, tells the user it started it, ends its turn, and the command finishes into an inbox nobody will claim.** The prompt told the model not to poll, and then nothing arrived."

修复方式 = 按 owner 状态选投递通道：
- busy owner → `inject()`（不唤醒）
- idle owner → `followup()`（唤醒）
- 上限 `maxConsecutiveWakes`（默认 3），超限退化为 inject；人类消息被 claim 才恢复额度
- teardown 也置 `reported`，避免销毁中的 agent 被自己的任务唤醒一圈

**这意味着：DSH 已具备"外部事件 → 唤醒 agent 开一轮 turn"的完整机制**，只是它的唯一生产者是 job 终态。SA 要做的是把生产者从"job 终态"扩展到"任意语义事件"。

## 3. 已有：`send(target × wakeup)` 是事件入对话的唯一水闸

证据：同上的 unified send decision（`2026-07-22-unified-send-and-coalesced-user-messages.md`），
以及 `packages/core/agent-loop/README.md`：

- `followup()` → next-turn FIFO + 唤醒
- `steer()` → next-step inbox + 唤醒
- `inject()` → next-step inbox **不唤醒**
- 一个 turn 边界原子 claim：pending next-step + 一条 queued prompt；步与步之间只 claim next-step
- 每次 inbox 变更发布 `agent/inbox/spliced`（唯一规范化事件），可重建

**这就是"事件→对话"的准入层**。SA 的观测归约（observation reduction）应当落在此处或其上游，而不是另开一条通路把事件塞进 prompt。

## 4. 已有：`schedule` 是"持久化定时事件 + 冷恢复"的样板

证据：`packages/schedule/README.md`

- 持久状态写在**原 session 日志**里（版本化 Schedule 事件 + fold），不另设数据库
- 进程内 owner 只在该 session 有 live root Agent 时等待；冷 session 在重新 live 时**补跑逾期项**
- 到期工作"enters the same conversation through the Agent's ordinary follow-up queue"
- 明确不提供外部通知通道

**这是 SA 续体账本最好的参照实现**：事件写进 session 日志 → fold 出状态 → live 时补投递。SA 只要把"定时到期"换成"外部事件到达"，把"fold 出 schedule"换成"fold 出机器状态 + 待兑现承诺"。

## 5. 已有：`goal-round-driver` 是"空闲驱动"的续跑，不是事件驱动

证据：`packages/goal/goal-round-driver/`（详见 workspace `goal-round-driver详解.md`）

- 触发条件：agent idle + goal phase=active + armed + 还有轮次 → 注入 `<goal_round>` 提示
- 它**不知道"发生了什么"**，只知道"还没干完，继续"
- 与 SA 的关系：正交但会互相干扰——一台机器在等事件时，driver 可能仍在空转烧 token

## 6. 已有：`ask_user_question` 把"等待外部输入"建模成一个挂起的工具调用

证据：`packages/interaction/tool-ask-user/README.md`

`ctx.userQuestions.ask()` 让工具调用 promise 一直挂着，直到人回答。这是 DSH 里唯一的**挂起/恢复**形态，
但它（a）只在 turn 内成立；（b）不跨进程持久化；（c）只服务"人"。SA 想要的"等待设备事件"没有对应物。

## 7. 架构约束：SA 不能 fork agent-loop

证据：`packages/core/agent-loop/README.md`

> "This is the only package in the harness that contains concrete loop logic. Everything else is an abstract service or a plugin against extension points — **new behavior goes into plugins, not here**."

⇒ SA 只能是插件层，且**活在循环之外**：持有机器、订阅事件、在 gate 上发起咨询（`agent.send`），
而不是改造 `ReactLoopAgent` 本身。

---

## 8. 缺口清单（SA 的真正增量）

| # | 缺口 | 现状 | 需要什么 |
|---|---|---|---|
| G1 | **细粒度事件** | 只有 job 终态；`readOutput` 是字节流不是语义事件 | 带类型的事件（`call.ringing` / `call.connected`），可与一个 operation 关联 |
| G2 | **持久化续体** | jobs 明确 process-local；ask_user 只在 turn 内 | 承诺账本：`op → 完成后做什么`，落 session 日志，恢复后补跑 |
| G3 | **机器状态** | 无。状态在模型上下文里"脑内" | 显式 FSM 状态 + 版本 + fold 恢复 |
| G4 | **咨询协议（gate）** | 二元：要么模型全程在环（ReAct），要么完全不在环（纯脚本） | 声明式 gate：只在标注的接缝上发起一次模型咨询 |
| G5 | **观测归约** | 事件要么全进 prompt，要么不进；只有 compaction 事后压缩 | 事前准入：边沿触发 + 由机器决定物化什么给模型看 |
| G6 | **并发机器 ↔ 单对话** | inbox 是 FIFO，无 correlation；多任务完成通知会各自开 turn（靠 `maxConsecutiveWakes` 兜底） | 机器级串行 + 咨询排队，避免 N 台机器抢一句对话 |

> 注：G1/G2/G3 是"能不能做"（能力），G4/G5/G6 是"做出来会不会崩"（治理）。
> 后三项在实践中更容易翻车，但业界几乎没有人公开讲过。

---

## 9. 可直接复用的零件（不要重造）

| 零件 | 复用方式 |
|---|---|
| `ctx.jobs` | 长任务生产者接口（`cancel`/`done`/`readOutput`）+ 归属隔离 + 上限 |
| `Agent.send(target × wakeup)` | 事件 → 对话的唯一准入层；`inject` 不唤醒正好是"机器攒着、等一次咨询"的语义 |
| session 日志 + persistence | 续体与机器状态的落盘点（照 `schedule` 的做法） |
| `schedule` 的 owner 模式 | 冷 session 恢复补跑的实现范式 |
| `agent/inbox/spliced` 事件 | 可重建的投递审计 |
| goal 的 armed/disarmed | 持久化意图的激活语义（防 fork/重启后乱跑） |
