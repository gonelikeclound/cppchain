# 04 · 事件驱动 + 状态机驱动 Agent 的形式化与架构基础

> 目标：为 SystemAgent（用「事件驱动 + 长生命周期状态机」替代 ReAct 循环）寻找可引用的形式化与架构依据。
> 方法：`web_search` + `curl` 抓一手页面（arXiv API / W3C 规范 / 官方文档 / DOI）。所有 arXiv 编号经 `export.arxiv.org/api/query?id_list=` 逐个校验标题与作者与日期；所有 URL 实测解析（少数出版商站点如 ACM DL / MIT Press / dblp 对脚本返回 403/202 反爬，DOI 本身已确认正确解析）。
> 约定：`【已验证】`＝读到过一手原文；`【未找到】`＝检索后确实没有，不做推测。

---

## 0. 结论摘要

1. **Harel 状态图 = 扁平 FSM + 层次 + 并发 + 广播事件 + 历史状态**。它解决的不是"表达力"，而是**状态爆炸与模块化**。SCXML 把语义固化成可执行的解释算法，这是"可验证性"的前提。
2. **Actor 的 mailbox + 单线程串行**把"并发事件乱序"从**全局问题**降级为**边界问题**：actor 内部事件序完全确定，乱序只可能出现在跨 actor 边界，因此可在边界处一次性用序号/守卫解决。这是 SystemAgent 事件总线的正确性根基。
3. **事件日志作为唯一真相**不是可选优化，而是"进程可重启恢复"的唯一自洽方案：`state = fold(events)`，恢复即重放，审计即读日志。**LLM 对话历史不能充当该日志**（有损、无类型、无幂等键、不可重放），它只能是一个投影。
4. **LLM 只是 policy 的一个实现**。POMDP 理论给出支点：状态估计（belief update）与决策（policy）可分离。相关工作明确指出部署态 LLM agent 是"以历史为条件、无显式 belief 的 policy"。**这解释了为什么 LLM 不该在每一次状态跃迁的关键路径上——它本来就不该是状态的一部分。**
5. **"LLM + 状态机"已有明确已发表工作**（StateFlow / HEXIS / Stagent / SMoT / MetaAgent / MetaGPT-SOP / AWM 等），并非空白；但**未找到**把「层次+并发状态图 + 持久化快照 + 补偿事务」与 LLM 统一建模的系统性论文——这是本设计的增量空间。
6. **确定性路径绕开 LLM 是数量级收益**：调用数从 O(步数) 降到 O(不确定点数)，成功率从 `(1-ε)^N` 的指数衰减变为 `(1-ε)^k`（`k ≪ N`）。已发表数据：StateFlow 成本 1/5~1/3 且成功率 +13~28pp；HEXIS token −38%~−89% 且 +16.2pp。

---

## 1. Statecharts（Harel 状态图）

### 1.1 Harel 1987 的核心贡献

Harel, *Statecharts: A Visual Formalism for Complex Systems*, **Science of Computer Programming** 8(3):231–274, 1987 — <https://doi.org/10.1016/0167-6423(87)90035-9>

动机是**反应式系统**：复杂度不来自计算，而来自"对大量异步刺激做时序相关的响应"。传统 FSM 在此状态数指数爆炸，图退化成无法阅读的"意大利面条"。四项核心机制：

| 机制 | 原始提法 | 解决什么 |
|---|---|---|
| **层次 / 状态聚合** | 超状态（super-state）内含子状态与子转移 | N 个特征从 `2^N` 笛卡尔积压缩为 N 棵子树；公共转移挂在父状态上，进入/退出语义由层次自动继承 |
| **并发 / 正交区域** | orthogonal components：一个状态被分解为多个**同时活跃**的区域 | 表达"握手 与 超时监控 与 渲染 同时进行"，不必显式枚举交错 |
| **广播通信** | 事件广播到整张图，所有活跃配置中的状态同时"看见" | 生产者无需知道消费者地址；一个事件天然触发多区域响应 |
| **历史状态** | shallow / deep history：重入复合状态时恢复上次的子状态 | "中断—恢复"成为原语，而非把"上次在哪"编码成变量 |

另有：转移上的**条件（guards）**、**转移动作**、**延迟/超时事件**、**entry/exit 动作** —— 即"事件—条件—动作"（ECA）模型的可视化形式。

**关键论点：statechart 的收益是语义压缩。** 同一行为，扁平 FSM 状态数是各特征的**乘积**，statechart 是**和**；前者不可读、不可维护、不可局部修改。

### 1.2 SCXML：把语义固化成可执行规范

W3C **Recommendation, 2015-09-01** — <https://www.w3.org/TR/scxml/>【已验证：抓取规范目录与附录 D】

结构元素（原文目录）：`<state>` / `<parallel>` / `<initial>` / `<final>` / `<history>` / `<onentry>` / `<onexit>` / `<transition>`（含 `type`、`cond`）/ `<raise>`（内部事件）/ `<send>` / `<cancel>` / `**<invoke>` / `<finalize>`**（一个状态可拉起子状态机并在退出时收尾）/ `<datamodel>` / `<assign>`。

**真正的价值在附录 D 的解释算法**（原文函数名）：

```
mainEventLoop()                      // 内部事件队列优先于外部队列
selectTransitions(event) / selectEventlessTransitions()
removeConflictingTransitions(...)    // 正交区域冲突消解
microstep(enabledTransitions)        // 一微步 = computeExitSet + 动作 + computeEntrySet
getTransitionDomain / findLCCA(...)  // Least Common Compound Ancestor：转移作用域
```

**为什么这对 SystemAgent 是决定性的**：`microstep` 把"状态跃迁"定义成纯函数 `(配置, 事件, 数据) → (新配置, 新数据, 输出事件)`；`removeConflictingTransitions` + `LCCA` 让"并发区域同时被触发时谁先谁后"有唯一确定答案。缺了这两点就写不出可重放执行器，也就没有"重启恢复"与"回归测试"。

### 1.3 XState：actor 模型化的实现

<https://stately.ai/docs/actors>、<https://stately.ai/docs/persistence>【已验证：以下为原文】

> "When you run a state machine, it becomes an **actor**: a running process that can receive events, send events and change its behavior based on the events it receives."
> "An **invoked** actor is started when its parent machine enters the state it is invoked in, and stopped when that state is exited. A **spawned** actor is started in a transition and stopped either with a `stop(...)` action or when its parent machine is stopped."
> "Actors process **one message at a time**. They have an internal **'mailbox'** that acts like an event queue, processing events sequentially. Internal actor state is not shared between actors."

三段几乎逐字对上 SystemAgent 的三项诉求：**`invoke`** = 能力生命周期绑定到状态（进状态即启动、退状态即取消，无需手写 start/stop 配对与泄漏防护）；**`send` + mailbox** = "工具调用立刻返回、结果作为事件晚到"的标准形态；**快照** = 进程重启恢复。

持久化文档尤其点明了后端长流程场景：*"In backend applications, persistence allows workflows to **span multiple requests**, **survive service restarts**, be fault-tolerant, represent long-running processes, and be **auditable and traceable**."* API 形态：`actor.getPersistedSnapshot()` / `createActor(machine, { snapshot }).start()`。文档还显式区分 `getSnapshot()`（对外发出的最后值）与 `getPersistedSnapshot()`（内部状态）——**"事件输出"与"内部状态"是两个不同的持久化对象**，这个区分对 SystemAgent 的存储设计直接可用。

### 1.4 为什么 statechart 比扁平 FSM 更适合真实世界流程

| 维度 | 扁平 FSM | Statechart | 对 SystemAgent |
|---|---|---|---|
| **并发** | 显式枚举交错，O(∏\|Sᵢ\|) | 正交区域，语义上"同时活跃" | "等用户确认"+"等工具结果"+"心跳超时"可同时成立，不必造复合状态名 |
| **层次** | 公共转移在每个状态重复 | 挂父状态，一处定义 | 全局 `Cancel`/`AuthExpired`/`BudgetExceeded` 只在根定义一次，全子状态继承 |
| **超时** | 手维护定时器集合，易泄漏 | 状态级 timeout，退出即撤销 | 每个等待态自带 deadline，无需旁路扫描 |
| **中断恢复** | 另设变量记"回到哪" | 历史状态一等公民 | "暂停两小时后恢复"是原语，不是补丁 |
| **可审计** | 状态爆炸后人类无法核对 | 分层阅读，每层有限 | 状态配置可打印、可 diff、可断言 → 回归测试基础 |
| **可验证** | 语义靠实现约定 | 标准解释算法 → 可模型检查 | 可静态验证不可达状态 / 死锁 / 缺失补偿 |

**一句话**：扁平 FSM 描述"机器的全部状态"，statechart 描述"机器的状态**结构**"。真实流程的复杂度是结构性的，所以必须用结构化的形式化方法。

---

## 2. Behavior Trees：分工，而非替代

一手来源：Colledanchise & Ögren, <https://arxiv.org/abs/1709.00084>；Iovino et al. 综述 <https://arxiv.org/abs/2005.05842>。

> ⚠️ **检索校正**：常被误引为 BT 综述的 `arXiv:1709.07074` 实际是**太阳日震学**论文（*Etude et mise en oeuvre des techniques Temps-Distance en Heliosismologie*）。正确编号为 `1709.00084` 与 `2005.05842`，均经 arXiv API 校验。

### 2.1 BT 的出发点就是对 FSM 的批评

2005.05842 摘要原文：*"game programmers found that the **Finite State Machines (FSM) that they used scaled poorly and were difficult to extend, adapt and reuse**. In BTs, the state transition logic is **not dispersed across the individual states, but organized in a hierarchical tree structure**, with the states as leaves."*

**BT 与 statechart 解决同一类问题（FSM 的模块化灾难），但走相反路线**：
- Statechart **保留状态**，把状态的层级与并发显式化 → 状态仍可命名、可持久化。
- BT **取消状态**，把转移逻辑提升为树 → 行为是可插拔子树，"执行到哪"退化为隐式的 **tick 游标**。

### 2.2 取舍

| | BT 胜 | Statechart 胜 |
|---|---|---|
| 组合性 | 子树可插拔，`Fallback`/`Retry`/`Timeout` 装饰器即用；重规划 = 换子树 | 层次是"包含"关系，换父状态影响面较大 |
| 反应性 | 每 tick 从根重评估，天然随时抢占 | 需显式建模抢占事件与优先级 |
| 状态可读性 | ✗ 当前状态是隐式 tick 位置 | ✓ 状态是可打印的活跃配置集合 |
| 可持久化 | ✗ 需额外存整树游标 + 黑板，恢复语义弱 | ✓ 快照直接可序列化 |
| 长事务/审计 | ✗ 无事务、无补偿点概念 | ✓ 状态边界天然是检查点/补偿边界 |
| 形式验证 | 主要在合成与正确性证明方向 | ✓ SCXML 标准解释算法 → 可模型检查 |

### 2.3 机器人/游戏 AI 的三条可迁移教训

1. **BT 取代 FSM 的真实动因是"复用"，不是"表达力"**（见 2005.05842 原文）。若痛点是"新增一种并发等待就要重构全图"，要的是层次/并发，不是 BT。
2. **BT 在"单次决策周期内"极强，在"跨事件长生命周期"上很弱**。BT 假设有持续 tick 的运行时；进程一退出，"我在树的哪个叶子"就丢了。**这正是 SystemAgent 的核心场景，所以外层不能用 BT。**
3. **实际系统是分层的**：机器人栈常见 `任务层（BT/规划）→ 技能层（FSM/控制器）`。映射到 SystemAgent：**外层 statechart 管理跨事件的生命周期，内层每个状态里的"如何做完这件事"用 BT/计划/单次 LLM 调用**。二者是上下层，不是竞争关系。

---

## 3. Actor 模型

来源：Hewitt/Bishop/Steiger IJCAI 1973 — <https://dblp.org/rec/conf/ijcai/HewittBS73.html>；Agha, MIT Press 1986 — <https://mitpress.mit.edu/9780262010924/actors/>；Armstrong 博士论文 2003 — <https://erlang.org/download/armstrong_thesis_2003.pdf>；O TP [`gen_statem`](https://www.erlang.org/doc/apps/stdlib/gen_statem.html)、[Design Principles](https://www.erlang.org/doc/system/statem.html)；[Akka](https://doc.akka.io/libraries/akka-core/current/typed/guide/actors-intro.html)。

### 3.1 模型原语

Actor 是唯一一等公民，三个原语（Hewitt 1973；Agha 1986 形式化）：**创建更多 actor**、**向已知地址发异步消息**（单向、不阻塞）、**改变自身行为**（become）。**无共享可变内存，无全局锁**，交互只能是消息。

### 3.2 mailbox + 单线程串行如何"天然解决乱序"

**问题**：`TOOL_RESULT(t=5)` 先于 `TOOL_RESULT(t=3)` 到达，旧结果被新结果反向覆盖；或 `CANCEL` 与 `RESULT` 竞态，导致已取消任务仍推进状态。

**Actor 分三层消解**：

1. **单 actor 内部：事件序 = 邮箱序，完全确定。** 一次只处理一条消息，处理期间不收新消息（XState 原文："Actors process one message at a time... sequentially"）。**actor 内部不存在任何并发**，转移函数是 `(state,msg) → state'` 的纯串行折叠。乱序的可能性被**消灭**，而非降低概率。
2. **跨 actor：乱序被限制在显式消息边界上，可被建模。** 乱序仍存在，但**只**出现在两个 actor 之间，不再弥散全系统。于是可用**因果序 / 逻辑时钟 / 单调 generation 号**在**边界处**一次性解决，而不是在每个业务逻辑里防御。
3. **乱序变成应用层可判定的问题。** 收件方状态封装且串行，可**基于自身当前状态**判定消息是否陈旧（"我不是 `WaitingForResult`，这条结果丢弃并记事件"）。这是状态机 + 邮箱组合出的确定性防护，**不需要分布式锁**。

**推论**：以"每个 Agent 实例 = 一个 actor = 一个串行事件循环"为架构不变量，"结果晚到 / 重复到达 / 乱序到达"三类问题全部退化为状态机的**事件守卫（guard）**问题——可单元测试，无需分布式协调。

### 3.3 Erlang/OTP：supervision tree、let-it-crash、`gen_statem`

- **supervision tree**：进程按树组织，父进程声明**重启策略**（`one_for_one` / `one_for_all` / `rest_for_one`）与重启强度（次数/时间窗）。故障处理**不写在业务代码里**，而是**声明式写在树上**。
- **let-it-crash**（Armstrong 2003 核心）：不要试图让每个进程处理所有错误并自愈；让它在无法处理的状态下**快速崩溃**，由监督者用**已知良好初态重启**。前提是**状态必须外置可恢复**——这正是它与 event sourcing 的接合点。反直觉但重要：崩溃是**有界**的（回到干净状态），"带伤运行"是**无界**的（腐蚀扩散）。
- **`gen_statem`**：OTP 官方状态机行为，文档列出的特性（原文抓取）逐条对上 SystemAgent 需求：
  **Event postponing**（事件暂缓，进合适状态再处理 = 结果晚到先排队）、**Self-generated events**（状态可插入新事件 = 内部事件）、**State time-out / multiple named time-outs**（状态级与命名超时）、**Automatic state enter calls**（onEntry 语义）、**Reply from other state than the request / Multiple replies**（请求与响应解耦 = "调用立刻返回、结果作为事件晚到"的**库级原生支持**）、**arbitrary term state**（状态可为结构化数据而非枚举）。
  文档明确它 "fits into an OTP supervision tree"——**状态机与监督树是一等集成**，崩溃恢复无需另造轮子。

### 3.4 Akka 与小结

Akka（typed）提供同一套概念 + `Cluster Sharding`（保证"某实体 actor 在集群中唯一存活"）与 [Persistence](https://doc.akka.io/libraries/akka-core/current/typed/persistence.html)（事件溯源式 actor 状态）。

**Actor 给 SystemAgent 的不是"并发框架"，而是三个正确性保证**：状态封装→无共享可变状态→无需锁；邮箱串行→actor 内事件序确定→转移函数可重放；监督树→故障处理声明式外置→恢复策略与业务逻辑解耦。

---

## 4. Saga / 补偿事务

Garcia-Molina & Salem, *Sagas*, **ACM SIGMOD 1987**, 249–259 — <https://dl.acm.org/doi/10.1145/38713.38742>

### 4.1 原始动机与三个常被误读的点

长事务不能持锁数小时。Saga 把长事务 `T` 分解为 `T₁…Tₙ`，每步配**补偿事务** `Cᵢ`（语义逆操作）；整体要么全成，要么按逆序 `Cₙ…C₁` 撤销。

1. **补偿 ≠ 回滚**。回滚是物理的、无痕的；补偿是**语义的、有痕的**（"取消订单"产生新记录而非删记录）。硬约束：**补偿动作本身必须被记录且可见**。
2. **补偿必须幂等**。失败可能发生在"补偿执行到一半"，重启后要能重放补偿。
3. **必须有决策点（pivot）**。Saga 分两段：pivot 前失败→补偿；pivot 后失败→重试直到成功（后续已不可逆）。在 agent 语境里 pivot 就是**提交点（commit point）**。

### 4.2 映射到"Agent 中途失败要回滚副作用"

| 副作用 | 可补偿性 | 补偿动作 | 备注 |
|---|---|---|---|
| 读（检索、查询） | 无需 | — | 只进事件日志 |
| 创建资源（分支、容器、工单） | 可补偿 | 幂等删除 + 登记墓碑 | 需幂等 |
| 修改资源（改配置、写文件） | 可补偿 | 写回旧值 | **必须"先读后写"，把 `before` 存进事件** |
| 发送通知（邮件、webhook） | **不可补偿** | ❌ 只能"发送更正" | 必须置于 pivot 之后或需用户确认 |
| 付款/转账 | 不可补偿 | 只能反向交易 | 必须有 pivot 与人工闸门 |

设计含义：① 每个工具注册时声明 `{idempotent, compensable, compensator}`；② 进入不可补偿动作前，状态机必须处于显式 `pivot` 状态——这是"人机确认点"的**形式化位置**；③ **补偿序列本身是一个状态机**（`Compensating` 下的子状态机），不是 `catch` 块里的 for 循环，因为它必须可持久化、可中断、可恢复。

### 4.3 为什么对 agent 尤其必要

ReAct 循环没有"事务"概念，也没有"提交点"。`act → observe → think` 每一步都**不可撤销地**改变世界；第 7 步出错时第 1–6 步的副作用永久留下，而 LLM 根本无法可靠推断"我到底改了什么"（它只有一段自然语言历史）。**Saga 把"副作用"从语言空间搬到状态空间**：副作用有编号、有反向操作、有记录位置。这是"agent 失败可收拾"的唯一工程解。

---

## 5. 事件溯源 / CQRS / 事件日志

Fowler: [Event Sourcing](https://martinfowler.com/eaaDev/EventSourcing.html)、[CQRS](https://martinfowler.com/bliki/CQRS.html)；Candea & Fox, *Crash-Only Software*, HotOS 2003 — <https://usenix.org/legacy/events/hotos03/tech/full_papers/candea/candea.pdf>；Temporal — <https://docs.temporal.io/workflows>。

### 5.1 为什么"事件日志是唯一真相"

命题：**不存当前状态，只存导致状态改变的事件序列；当前状态是事件的折叠（fold）**。三条推论各对应一项诉求：

1. **恢复**：`state = fold(events)`，因此**恢复路径与正常运行路径是同一条代码路径**。Candea & Fox 的 *crash-only software* 推到极致：**不写"优雅关闭"逻辑，让重启成为唯一的恢复正常态手段**——这只有在日志是唯一真相时才成立。对 SystemAgent：**恢复 = 重放日志 + 恢复快照，不需要任何退出钩子**。
2. **审计**：日志是 append-only、不可变、带因果序的。"为什么系统是这个样子"的答案全在日志里，且**可被机器验证**（重放一遍）。
3. **可回归**：把生产日志做成"事件回放测试集"——同一串事件必须得到同一终态。**这是确定性转移函数的直接红利**，LLM 参与的路径做不到。

工程细节：**快照 + 日志必须共存**。纯重放随日志增长变慢；标准做法是周期物化快照，恢复时 `load(snapshot) + replay(events after snapshot)`。XState 的 `getPersistedSnapshot()` 正好就是那个"快照"。

### 5.2 CQRS 与"LLM 该看到什么"

CQRS = 写侧（command）走状态机做决策与一致性校验；读侧（query）是写侧事件的**投影**，可为每个查询场景建独立物化视图。映射到 SystemAgent：**写侧 = 状态机**（唯一权威、串行、严格转移）；**读侧 = 任意投影**（给用户的进度条、给运维的任务列表、**给 LLM 的上下文**）。**这些都是一级投影，不是真相。** 这就回答了"LLM 该看到什么"：从事件日志**投影**出的结构化视图（当前状态 + 关键事实 + 未决目标），而不是原始对话历史。

### 5.3 与 LLM 对话历史的关系（重点）

**对话历史不能充当事件日志**，五点理由：

| 维度 | 事件日志 | LLM 对话历史 |
|---|---|---|
| 类型 | 强类型事件（`TOOL_RESULT{call_id,status,payload}`） | 自由文本 + 半结构化 JSON 混排 |
| 幂等键 | `event_id`/`call_id`，可去重 | 无；重复调用无法识别 |
| 可重放 | 确定性 `fold` | 不可重放（温度、模型版本漂移） |
| 完整性 | 记录所有状态改变 | 只记录"被塞进 prompt 的部分"，有损、会被 context window 截断 |
| 语义 | "发生了什么" | "模型被告知了什么" + "模型说了什么"，**两者混淆** |

正确架构关系：

```
事件日志（唯一真相）
  ├─ 投影 A：状态机快照（恢复用）
  ├─ 投影 B：审计视图（人看）
  └─ 投影 C：LLM 上下文（模型看） ← 对话历史只是这一层的序列化格式
```

对话历史降级为**一个投影的序列化格式**，而非系统状态。这解决了 ReAct 的根本病灶：**把"记忆"当成了"状态"**。旁证：TapeAgents（<https://arxiv.org/abs/2412.08445>）以结构化 "tape"（可重放会话日志）为单一真相源；Temporal 的 durable execution 用 **append-only Event History** 重放工作流以重建内存状态。

---

## 6. POMDP / 决策论视角：LLM 只是 policy 的一个实现

### 6.1 形式化基线

POMDP（Kaelbling, Littman & Cassandra, *Planning and acting in partially observable stochastic domains*, **Artificial Intelligence** 101, 1998 — <https://doi.org/10.1016/S0004-3702(98)00023-X>）：

```
M = ⟨S, A, T(s'|s,a), O, Z(o|s,a), R(s,a), γ⟩
belief:  b'(s') ∝ Z(o|s',a) · Σ_s T(s'|s,a) · b(s)
policy:  π(a | b)     ← policy 的输入是 belief，不是 history
```

**核心结论**：只要 belief 是 history 的**充分统计量**，POMDP 可约化为 belief space 上的 MDP，从而继承 Bellman 最优性保证。**即：状态估计与决策是两个可分离的问题。** 这是全部论证的支点。

### 6.2 一手表述：部署态 LLM agent 是没有 belief 的 history-conditioned policy

Chattopadhayay, *Belief-State Engine*, <https://arxiv.org/abs/2609.10036>（2026-09）摘要原文：

> "We trace these symptoms to a common structural cause. **An LLM agent, as commonly deployed, is a history-conditioned policy with no explicit belief over hidden state.**"

它点出的三个症状几乎逐条命中 ReAct 的工程痛点：*"Ambiguous feedback pushes them into **premature commitments**. A single informative observation can **collapse their uncertainty onto the wrong hypothesis**. **Policies drift as the history grows.**"*

其修法正是 SystemAgent 想要的形态——把 belief 维护放在 **LLM 之外**（"an inference module placed outside the LLM"），每步只暴露后验、**不暴露原始 history**；并证明"LLM + BSE 是 belief MDP 上的健全马尔可夫策略"，从而继承 POMDP 的 Bellman 保证——**前提是 LLM 永远不接触原始历史**。

**推论**：LLM 在架构中的正确位置是 **policy 的一个实现（或仅是 policy 的先验/提案器）**；而**状态估计应当确定、由事件驱动、不经过 LLM**。这从理论上解释了"LLM 不应出现在每一次状态跃迁的关键路径上"——因为它本来就不是状态的一部分。

### 6.3 支持性论述：LLM 不是完整的 policy

- **LLM-Modulo**（<https://arxiv.org/abs/2402.01817>）：LLM 是**想法生成器**，必须配**外部 sound verifier** 在环才得到有保证的计划。→ LLM 提议转移，确定性代码校验并执行。
- **LLM+P**（<https://arxiv.org/abs/2304.11477>）：LLM 只把自然语言**翻译成 PDDL**，求解交给经典规划器。→ LLM 负责"理解"，求解器负责"决策"。
- **SayCan**（<https://arxiv.org/abs/2204.01691>）：LLM 给"语言可行性"打分，与底层 affordance 相乘，由确定性策略执行。→ LLM 是**评分/先验**，不是执行器。
- **长程误差累积**：PlanBench / *On the Planning Abilities of LLMs: A Critical Investigation*（<https://arxiv.org/abs/2206.10498>）显示 LLM 在多步状态跟踪上系统性失败；*How Fast Do Agents Rot?*（<https://arxiv.org/abs/2609.01660>）把"基准成功率上升但生产不可靠"归因于**任务长度**：基准集中在短中程，生产需要多一个数量级的依赖步数。

### 6.4 数学直觉

设单步可靠率 `1-ε`，独立近似下 N 步成功率 ≈ `(1-ε)^N`。若每步都过 LLM（ReAct），`ε` 是 LLM 决策错误率 → 指数崩塌；若只有 `k` 个真正不确定点过 LLM，其余 N−k 步 `ε=0` → `(1-ε)^k`，且 `k ≪ N`。**"确定性路径不经过 LLM"不是风格偏好，而是把指数衰减的底数从"每步"改成"每个不确定点"。**

---

## 7. 已发表的「LLM + 状态机」工作

**找到了，且不止一篇。** 2023–2026 形成清晰谱系：从"用状态机组织 prompt"演进到"把技能编译成可静态检查的 FSM"。

| 工作 | 年 | 核心做法 | 与本设计的关系 |
|---|---|---|---|
| **StateFlow** <https://arxiv.org/abs/2403.11322> | 2024 | 任务求解**概念化为状态机**；分离 process grounding（状态与转移）与 sub-task solving（状态内动作）；转移由启发式规则**或 LLM** 决定 | 最接近的先行工作。InterCode-SQL / ALFWorld 上比 ReAct 成功率 **+13%/+28%**，成本 **1/5、1/3** |
| **HEXIS** <https://arxiv.org/abs/2609.30123> | 2026 | **把 agent skill 编译成扩展 FSM**，分离"知识"（状态内局部指令）与"控制流"（转移条件）；更新须通过**静态检查 + 历史 trace 重放**才被接受 | **最接近"LLM 生成状态机"的已发表工作**。比 `Skill+ReAct` 平均 **+16.2pp**；token **−38.4%~−88.9%**。"静态检查+trace 重放"正是第 8 节的可验证性 |
| **Stagent** WESAAC 2025 <https://sol.sbc.org.br/index.php/wesaac/article/download/45240/45001/> | 2025 | agent 执行建模为**显式状态与转移**，输出**合法 FSM trace**，使步骤可检视、失败后可改道 | 强调 traceability（failure localization、recovery control）；所有执行均产出合法 FSM trace，任务成功率 0.963 |
| **SMoT** <https://arxiv.org/abs/2312.17445> | 2023 | 用状态机**记录历史推理轨迹**；状态=子问题，转移=依赖；同时记录成功与失败轨迹 | 状态机作为**经验记忆结构**，与"事件日志即真相"同构 |
| **MetaAgent** <https://arxiv.org/abs/2507.22606> | 2025 | 给定任务描述，**自动设计 FSM 形式的多 agent 系统**并用优化算法精炼；部署时 FSM 控制动作与转移 | 明确"LLM 自动生成 FSM 拓扑" |
| **MetaGPT** <https://arxiv.org/abs/2308.00352> | 2023 | 把 **SOP 编码进 prompt 序列**，流水线式角色分工 | SOP = 人写的、自然语言形态的状态机；缺的是**可执行性与持久化** |
| **Agent Workflow Memory** <https://arxiv.org/abs/2409.07429> | 2024 | 从过往轨迹**归纳可复用 workflow（routine）**并注入后续任务 | "从事件日志学出状态机"的可行路径 |
| **AFlow** <https://arxiv.org/abs/2410.10762> / **GPTSwarm** <https://arxiv.org/abs/2402.16823> | 2024 | workflow 表示为**可优化的图/代码**，用 MCTS / 图优化改进 | workflow（≈状态图）成为**可被算法优化的对象** |
| **MASMP** <https://arxiv.org/abs/2510.18395> | 2025 | 自然语言驱动的状态机架构，引导 LLM **模拟** FSM 与行为树 | 注意：这是"**提示 LLM 模仿**状态机"，不是真状态机——反面对照，说明"真状态机必须在代码里" |
| **LLM + BT** <https://arxiv.org/abs/2406.00965>、<https://arxiv.org/abs/2409.09435> | 2024 | LLM 生成/精化行为树，交给 BT 执行器 | 印证第 2 节"LLM 产出结构、执行器确定性运行" |
| **Belief-State Engine** <https://arxiv.org/abs/2609.10036> | 2026 | belief 维护外置为 LLM 之外的贝叶斯模块；LLM 只看后验 | 第 6 节的形式化落地 |

### 7.1 明确的检索缺口（如实记录）

- **未找到**：把「Harel statechart 的层次 + 并发 + 历史状态」完整实现为 LLM agent 运行时、并叠加「持久化快照 + Saga 补偿语义」的**系统性论文**。现有工作里的"状态机"绝大多数是**扁平、单层、无并发区域**的 FSM（StateFlow、HEXIS、Stagent、MetaAgent 均是）。
- **未找到**：把「actor mailbox 串行化」作为 LLM agent 事件乱序问题的**形式化解决方案**并给出实验的论文。这一块**工程实践领先于文献**（Erlang/XState/Akka 成熟，agent 领域尚未系统引用）。
- **未找到**：Saga / 补偿事务与 LLM agent 副作用回滚的**正式学术论文**（Temporal 等框架文档有 Saga 模式，论文层面未检索到）。

→ 这三点是本设计相对现有文献的**明确增量空间**，也意味着这三部分需靠**工程论证 + 经典文献引用**，而不能靠"已有论文这么做"背书。

---

## 8. 确定性 / 可验证性：为什么确定性路径必须绕开 LLM

（本节为论证，不依赖外部引用；量化数据来自第 7 节。）

**① 可测试性。** LLM 转移的断言只能依赖语义相似度 / LLM-as-judge → **没有 Pass/Fail，只有分数**。确定性转移 `transition(config,event) → config'` 是**纯函数**，可 `assert transition(S_WAIT_RESULT, TOOL_OK) == S_DONE`，可做到分支覆盖 100%、property-based 测试、变异测试。决定性差异：**确定性转移的 bug 能被发现并永久修复；LLM 偏差只能降低概率，永远复发。前者是工程，后者是运维。**

**② 可回归性。** 纯转移函数 + append-only 日志 ⇒ **回放回归**：把生产日志喂给新版本状态机，断言终态逐条一致。这是"引入新能力不破坏旧流程"的唯一可靠手段。LLM 路径无法回放（模型版本、温度、上下文截断都会变）——**回归测试在 ReAct 架构里事实上不可能**。与 HEXIS 做法一致：只接受"通过静态检查**且**重放当前及全部历史 trace"的状态机更新。**"重放即验收"已是可发表的方法论。**

**③ 成本。** 设流程 `N` 步、真正需要判断的不确定点 `k` 个：

| 架构 | LLM 调用数 | 备注 |
|---|---|---|
| ReAct 循环 | ≈ `N`（含反思则 `2N`） | 每步重组全历史 → token 随步数**二次增长** |
| 状态机 + 定点 LLM | ≈ `k` | 确定性步零调用；每次调用的上下文是**投影后的最小视图** |

已发表量化：StateFlow 成本降至 **1/5~1/3** 且成功率 **+13~28pp**；HEXIS token **−38.4%~−88.9%** 且 **+16.2pp**。效率提升不是"省钱"，而是**改变成本曲线的形状**：ReAct 随流程长度超线性增长，状态机随"不确定点数量"线性增长，后者通常与流程长度解耦。

**④ 延迟。** LLM 调用是秒级、高方差的（p99 常为 p50 的 3–10 倍）。把 LLM 放在每步关键路径上，端到端 p99 是 **N 个高方差分布之和**，尾部极难看；确定性步是微秒–毫秒、低方差。分离后 `端到端 ≈ k × LLM_p99 + (N−k) × 常数`。更重要的是**可预测性**：确定性路径的延迟可被 SLO 保证，含 LLM 的路径**不能**对用户做延迟承诺。

**⑤ 可审计 / 可合规。** "为什么执行了这个副作用？"——确定性转移答："因为事件 E 在状态 S 下满足守卫 G，依据规则 R。"LLM 转移只能答"模型认为应该"。这在受监管场景（金融、医疗、运维变更）**不可接受**。更进一步：确定性状态图**可被模型检查**（SCXML 标准解释算法 → 可转 Kripke 结构 → 可验证"不存在到达 `Deployed` 而跳过 `Approved` 的路径"），这是**静态保证**而非测试抽样。

**⑥ 可扩展性。** 加能力 = 加一个正交区域或一棵子树，其余不动 → 变更影响面局部化、可 review。ReAct 里"加能力"= 改系统提示词 → **影响面全流程、不可预测、不可测试**。这是 prompt 工程无法工程化的根因。

**⑦ 结论 — 收益按维度清点，每项都是数量级而非百分比：**

| 维度 | 收益 |
|---|---|
| 可测试性 | 从"打分"到"断言"，覆盖率可度量 |
| 可回归性 | 从"不可能"到"逐事件回放 diff" |
| 成本 | 从 O(N) 超线性到 O(k) 线性 |
| 延迟 | 从 N 个高方差之和到 k 个 LLM + 常数 |
| 可审计 | 从"模型认为"到"规则 R + 事件 E" |
| 可验证 | 从抽样测试到模型检查 |

---

## 概念对照表

| 概念 | 适合表达什么 | 不适合表达什么 | 对 SystemAgent 的角色 |
|---|---|---|---|
| **Statechart（Harel）** | 层次化生命周期、并发区域、历史状态（中断—恢复）、状态级超时、广播事件下的多区域响应 | 单次决策内的细粒度行为组合；连续控制；大量同构子任务调度 | **架构骨架**：每个能力一张 statechart；状态配置是可持久化、可断言、可审计的权威状态 |
| **FSM（扁平）** | 小而封闭的协议（重试计数、握手、UI 向导）；状态数 < ~20 且无并发 | 并发特征（状态数乘积爆炸）；需中断恢复的场景；长期演进 | **叶子节点**：作为 statechart 单个子状态内部实现；也是"LLM 输出"最合适的**受限目标**（HEXIS 做法） |
| **Behavior Tree** | 单次决策周期内的技能组合、回退/重试/抢占、子树可插拔、反应式重评估 | 跨事件的持久状态（tick 游标隐式、难序列化）；长事务与补偿边界；审计"现在在哪一步" | **状态内的执行器**：进入某状态后"如何做完这件事"；可由 LLM 生成、由确定性 tick 运行 |
| **Actor** | 状态封装、异步消息、监督与重启、分布式唯一实例（sharding）；**mailbox 串行消除内部并发** | 全局一致性快照；跨 actor 强事务；简单同步调用链（过度设计） | **运行时模型**：一状态机实例 = 一 actor；事件总线 = 消息投递；监督树 = 恢复策略；**保证 actor 内事件序确定** |
| **Saga（补偿事务）** | 长事务失败撤销、不可逆动作的提交点（pivot）、跨外部系统的语义回滚 | 需 ACID 的短事务；可物理回滚的同库操作；要求"完全无痕"撤销 | **副作用治理层**：工具声明 `{idempotent, compensable, compensator}`；不可补偿动作前设 `pivot`（人机确认点）；`Compensating` 自身是子状态机 |
| **Workflow Engine（Temporal / Step Functions）** | 持久执行（durable execution）、跨请求跨重启编排、重试与超时、可视化运维、事件历史重放 | 极低延迟的内联决策；高度动态的拓扑（引擎多为静态图）；跨引擎强一致 | **可选执行底座**：提供 Event History + 重放 + 定时器 + 重试；也可自研（statechart + 快照 + 日志）。与 statechart 重叠，需二选一或明确分层 |
| **ReAct loop** | 探索性、开放域、步骤不可预知的任务；快速原型；无需审计的一次性任务 | 长流程（误差指数累积）；有副作用的操作（无事务无补偿）；需审计/回归/成本控制的场景；并发事件（无乱序防护） | **应被替代的对象**；可降级为**单个状态内部的兜底策略**（在 `Exploring` 状态里跑有界 ReAct），而非全局控制流 |

横向观察：**Workflow Engine 与 Statechart 高度重叠但重心不同**——前者重心是"持久执行的可靠性"（事件历史、重试、定时器），后者重心是"状态语义的正确性"（层次、并发、历史状态、冲突消解）。SystemAgent 两者都要：用 statechart 定义语义，用事件日志/快照实现持久执行。

---

## 对 SystemAgent 的设计约束

每条一句话、可执行、可验收。

1. **状态机必须支持层次 + 正交区域 + 历史状态三件套**——验收：全局 `Cancel`/`AuthExpired`/`BudgetExceeded` 只在根状态定义一次即可作用于所有子状态；"等用户输入"与"等工具结果"可在两个正交区域同时活跃而不产生复合状态名。
2. **每个状态机实例映射为一个 actor，其 mailbox 串行处理事件**——验收：注入 1000 个乱序事件后终态唯一，且同实例处理函数永不并发。
3. **事件日志是唯一真相，状态 = `fold(events)`，并对外提供快照**——验收：`kill -9` 后仅凭 `快照 + 事件` 恢复的终态与未崩溃对照组的终态逐字段相等。
4. **所有工具调用一律"立即返回 receipt、结果作为事件晚到"**——验收：重复到达的结果被守卫丢弃并记 `DUPLICATE_DROPPED`；陈旧结果被状态守卫拒绝；工具 p99 不计入状态跃迁延迟。
5. **每个工具必须声明 `{idempotent, compensable, compensator}`，不可补偿动作之前必须存在显式 `pivot` 状态**——验收：静态检查能枚举所有 `compensable=false` 动作，并证明到达它的每条路径都经过某个 `pivot`。
6. **`Compensating` 必须是可持久化的子状态机，而不是 `catch` 块里的循环**——验收：补偿执行到一半时崩溃，重启后从中断步骤继续，且已完成步骤不重复执行。
7. **LLM 只允许出现在显式标记的"不确定点"上，输出必须被约束为有限动作集或状态机片段**——验收：CI 中断言单次流程的 LLM 调用数 `k ≤ K_max` 且与步骤总数 `N` 解耦；LLM 输出经确定性校验后才允许触发转移。
8. **LLM 的输入必须是事件日志的投影（当前状态 + 关键事实 + 未决目标），禁止投喂原始对话历史**——验收：构造超长历史时请求 token 数保持有界；投影函数有独立单测；可证明"LLM 未见原始 history"。
9. **转移函数必须是纯函数 `(configuration, event, data) → (configuration', data', out_events)`，不得内含 I/O 或 LLM 调用**——验收：任意事件序列可纯回放且同序列必得同终态；生产日志可直接转为回归测试集。
10. **恢复与回归只能依赖事件重放，禁止写优雅关闭钩子**——验收：crash-only 测试在任意随机步数 `kill -9` 后均恢复一致状态；生产日志回放通过率 100%；状态图静态检查"不可达状态 = 0、无未声明死锁、并发转移冲突均有优先级规则"。

> 若需裁剪，保留 **2、3、5、7、9、10** —— 分别锁住事件序确定性、可恢复性、副作用可收拾、成本可控、可测试、可回归。这六条是"事件驱动 + 状态机驱动"成立的最小充分集。

---

## 参考来源（arXiv 编号已逐个 API 校验；DOI 已确认解析）

**形式化基础** — Harel 1987 <https://doi.org/10.1016/0167-6423(87)90035-9>；SCXML W3C Rec 2015-09-01 <https://www.w3.org/TR/scxml/>；UML 状态机形式化综述（arXiv:2407.17215）<https://arxiv.org/abs/2407.17215>

**Actor / 运行时** — Hewitt 1973 <https://dblp.org/rec/conf/ijcai/HewittBS73.html>；Agha 1986 <https://mitpress.mit.edu/9780262010924/actors/>；XState [Actors](https://stately.ai/docs/actors) / [Persistence](https://stately.ai/docs/persistence)；Erlang [`gen_statem`](https://www.erlang.org/doc/apps/stdlib/gen_statem.html) / [Design Principles](https://www.erlang.org/doc/system/statem.html)；Armstrong 2003 <https://erlang.org/download/armstrong_thesis_2003.pdf>；Akka [actors](https://doc.akka.io/libraries/akka-core/current/typed/guide/actors-intro.html) / [persistence](https://doc.akka.io/libraries/akka-core/current/typed/persistence.html)

**Behavior Trees** — 入门书 arXiv:1709.00084 <https://arxiv.org/abs/1709.00084>；综述 arXiv:2005.05842 <https://arxiv.org/abs/2005.05842>

**事务与日志** — Sagas (SIGMOD 1987) <https://dl.acm.org/doi/10.1145/38713.38742>；Fowler [Event Sourcing](https://martinfowler.com/eaaDev/EventSourcing.html) / [CQRS](https://martinfowler.com/bliki/CQRS.html)；Crash-Only Software (HotOS 2003) <https://usenix.org/legacy/events/hotos03/tech/full_papers/candea/candea.pdf>；Temporal <https://docs.temporal.io/workflows>

**决策论** — POMDP (AI 101, 1998) <https://doi.org/10.1016/S0004-3702(98)00023-X>；LLM-Modulo arXiv:2402.01817 <https://arxiv.org/abs/2402.01817>；LLM+P arXiv:2304.11477 <https://arxiv.org/abs/2304.11477>；SayCan arXiv:2204.01691 <https://arxiv.org/abs/2204.01691>；PlanBench arXiv:2206.10498 <https://arxiv.org/abs/2206.10498>；Belief-State Engine arXiv:2609.10036 <https://arxiv.org/abs/2609.10036>；How Fast Do Agents Rot arXiv:2609.01660 <https://arxiv.org/abs/2609.01660>

**LLM + 状态机 / SOP** — StateFlow arXiv:2403.11322 <https://arxiv.org/abs/2403.11322>；HEXIS arXiv:2609.30123 <https://arxiv.org/abs/2609.30123>；Stagent (WESAAC 2025) <https://sol.sbc.org.br/index.php/wesaac/article/download/45240/45001/>；SMoT arXiv:2312.17445 <https://arxiv.org/abs/2312.17445>；MetaAgent arXiv:2507.22606 <https://arxiv.org/abs/2507.22606>；MetaGPT arXiv:2308.00352 <https://arxiv.org/abs/2308.00352>；Agent Workflow Memory arXiv:2409.07429 <https://arxiv.org/abs/2409.07429>；AFlow arXiv:2410.10762 <https://arxiv.org/abs/2410.10762>；GPTSwarm arXiv:2402.16823 <https://arxiv.org/abs/2402.16823>；MASMP arXiv:2510.18395 <https://arxiv.org/abs/2510.18395>；HBTP arXiv:2406.00965 <https://arxiv.org/abs/2406.00965>；BT-Gen arXiv:2409.09435 <https://arxiv.org/abs/2409.09435>；TapeAgents arXiv:2412.08445 <https://arxiv.org/abs/2412.08445>；ReAct arXiv:2210.03629 <https://arxiv.org/abs/2210.03629>；Anthropic *Building Effective Agents*（workflows vs agents 的工程定义）<https://www.anthropic.com/engineering/building-effective-agents>
