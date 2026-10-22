# 19 · Agent 执行治理：跨系统优点提取与关键技术点（外部对照档案）

> **定位**：外部调研档案（整理 2026-10-19），逐字留存。来源：Claude Code / Codex / Anthropic harness / ADK / LangGraph / Apache Maka / agent-git / Meta Muse 的可迁移优点。
> **用途**：与 [discuss 18](../01-discuss/18-Agent执行治理总纲.md) 并列作对照来源；[problems/39](39-治理手册对照评估与A6验收规划.md) 是逐条映射与 A6 落地。
> **纪律**：历史与证据可回溯——本页保留原文，不删改；新增理解写在 problems/39。
>
> 配套文档：`Agent执行治理-总纲与完整性审计.md`（架构与缺口）、`Agent完成判定-跨家做法与可信执行.md`（完成判定全景）。原始调研笔记：`research-可信执行/A~G-*.md`。

---

## 0. 怎么用这份文档

- 每条技术点给四样东西：**关键实现点 / 边界与代价 / 我们的落点（对应总纲缺口）**。
- 标 ⭐ 的是我认为**最该抄**的五条。
- 标 ⚠️ 的是"看着好、抄了会出事"，放在 §11 反模式里单独列。
- 所有结论优先取代码与可执行契约，**文档与源码冲突时以源码为准**（见 §10 元教训）。

---

## 1. 索引：系统 → 最大优点 → 关键点编号

| 系统 | 最值得拿的一样东西 | 技术点 |
|---|---|---|
| **Anthropic harness** | sprint contract：动手前把 "done" 谈成可测条目 | A1 A2 A4 I1 |
| **Claude Code** | 把「停止」变成可被外部否决的事件（Stop hook） | A5 A6 A7 |
| **Codex CLI** | 目标循环 + 预算兜底 + "放弃需连续 ≥3 轮" | A7 D- |
| **ADK** | 产物即证据：命名/版本/作用域，元数据与载荷分离 | B1–B7 |
| **LangGraph** | checkpoint 粒度与 pending writes 的精确语义 | G6 G7 E1 |
| **Apache Maka** | T1/T2 双提交 + `unknown` 显式态 + repair≠resume | C1–C3 G1–G4 |
| **agent-git（agit）** | 内容寻址 commit（真幂等）+ LOG/VIEW 分离 | D2 G8 |
| **agent-git（MAS）** | 补偿注册表（reverse_tools）+ 撤销/补偿二分 | C9 F1 F2 |
| **Meta Muse** | 审批 = 能力授权，单点不可绕过的 Sentinel | C5–C8 |
| **社区闸门**（proof-of-done 等） | 证据新鲜度 + 否决原因封闭枚举 | A9 A10 |

---

## 2. 完成判定与验收

| # | 技术点 | 来源 | 关键实现点 | 边界与代价 |
|---|---|---|---|---|
| **A1** ⭐ | **sprint contract** | Anthropic harness | 动手前由生成方提出"做什么 + 怎么验证"，判定方审提案，双方迭代到一致才开工；通信走文件 | 需要一次协商往返；只适合可分解任务 |
| **A2** | **硬阈值合取门** | Anthropic harness | 每条判据有阈值，**任一不达标即整体失败**，回灌具体证据 | 阈值要校准，过严会无限返工 |
| **A4** | **判官必须接地** | Anthropic harness vs Claude Code `/goal` | harness 的 evaluator 用 Playwright **真的点应用**；`/goal` 的裁判**只读 transcript 不跑工具**——后者是弱形态 | 接地成本高（4 小时级试验） |
| **A5** ⭐ | **停止可被外部否决** | Claude Code hooks / Codex turn loop | `Stop`/`SubagentStop` 返回 `decision:"block"` 即拒绝结束并回灌反馈；Codex 是 `should_block` + `continuation_fragments` 重新进循环 | 连续否决需上限（Claude Code 是 8 次续跑后强制放行） |
| **A6** ⭐ | **agent 型钩子（带工具的验收者）** | Claude Code hooks | 派一个 subagent，可 Read/Grep/Glob，**最多 50 轮**，返回 `{"ok": true/false}`；官方原话"verification requires inspecting actual files or test output" | 标记为 experimental；默认关闭 |
| **A7** | **三态裁决** | Claude Code `/goal`；Codex `update_goal` | `met / not yet met / **impossible**`；Codex 的 `blocked` 需**连续 ≥3 轮**才成立 | 给"判不了"和"做不到"各一个合法出口，避免二选一硬判 |
| **A8** | **世界状态对账** | τ-bench | 判定 = 比对会话结束时的 **DB 状态**与标注的 goal 状态，不看文本 | 需要可观测的状态容器 |
| **A9** ⭐ | **证据新鲜度** | proof-of-done / agent-done-or-not | 最后一次通过必须**晚于最后一次修改**；三处独立实现收敛到同一个词"新鲜/FRESH/NEW" | 需精确的写入与检查时序 |
| **A10** | **否决原因封闭枚举** | proof-of-done | `supported / stale / no_command / failed_exit / failed_output / empty_run / masked_inconclusive / background_only / superseded_by_failure` | 枚举本身是"可分别拦截的失败机制"清单 |
| **A11** | **覆盖检查** | 本方案 | plan 的全部验收条目 ∪ 起来必须覆盖 goal 的条目，否则拒绝执行 | 一次静态比对，成本极低 |
| **A12** | **每步自带验收** | 本方案（吸收 A1） | `step = {目标, 验收条目, 预算, 产出物}`；没有验收条目的"步骤"不是步骤 | 条目要能被 A11 核对，否则注水 |

---

## 3. 证据与产物（ADK Artifacts）

| # | 技术点 | 关键实现点 | 边界与代价 |
|---|---|---|---|
| **B1** ⭐ | **产物即一等公民** | `{filename, version, scope, payload}`，由独立 `BaseArtifactService` 管，**不进会话状态** | 只是容器，不含判定语义 |
| **B2** | **版本单调性替代时间戳** | 新鲜度判据写成 `evidence.version ≥ 目标文件的最后写入版本`；版本号由服务分配，模型伪造不了 | 需要"每次写入必产生版本" |
| **B3** | **前缀即作用域** | `{app}/{user}/user/{filename}/{version}` vs `{app}/{user}/{session}/{filename}/{version}`；`user:` 前缀不是装饰而是分支判据 | 文件名含 `/` 时列举有前缀歧义，需"后缀必须是纯数字"的解析守卫 |
| **B4** | **引用的深度与作用域校验** | artifact 可引用 artifact；`max_depth=5`；`validate_artifact_reference_scope` 禁止跨 app/user/session | 引用图要防环 |
| **B5** | **墓碑式撤销** | rewind 不真删，**追加一个"空 payload"新版本**，加载时视为不存在——历史因此永远可解释 | 官方自承标记与合法空文件冲突（向后兼容锁死了格式） |
| **B6** | **delete 不给模型** | `delete` 是 service 方法但**不暴露给 agent context**（原文 for safety） | —— |
| **B7** | **临时附加、不落历史** | `LoadArtifactsTool` 把选中产物**只附加到当次请求**，不写回会话历史；下次要用得再调 | 模型必须重新取，多一次调用 |

**为什么这套重要**：它把"证据"从**模型复述的文本**变成**可引用的对象**，于是"对账"退化成"比指针/hash"，而不是"读一段散文"。

---

## 4. 副作用治理

| # | 技术点 | 来源 | 关键实现点 | 边界与代价 |
|---|---|---|---|---|
| **C1** ⭐ | **T1/T2 双提交点** | Maka | 短事务 T1 提交 `actions.toolDispatch`（模型不可见）→ 外部副作用窗口 → 短事务 T2 提交结果。T1 语义：*"no longer safe to assume the implementation did not run"*，**不等于成功**；T1 失败则实现必须**零次调用** | 需要两个独立提交点与投影 |
| **C2** ⭐ | **`unknown` 是一等状态** | Maka | T1 有 / T2 无 ⇒ `outcome_unknown`，既不重试也不宣称成功 | 必须有人/机制收敛它 |
| **C3** | **缺失结果的四种解释** | Maka | ①没启动 ②启动没写 ③写了但结果没提交 ④写了又被改过。*"Always retrying duplicates case 3; always declaring success gives a false history in 1,2,4."* | —— |
| **C4** | **幂等键由框架生成** | miclaw | `hash(callerScope, taskId, **intentSlot**, canonicalArgs, tool)`；`canonicalArgs` 做规范化（去时间戳/排序/折叠等价形式）；**不信三方** | 规范化规则需工具级声明 |
| **C5** | **唯一约束做 at-most-once** | Maka | `tool_operations(operation_id PK, UNIQUE(invocation_id, provider_tool_call_id))`；续跑侧 `target_run_id UNIQUE` / `boundary_digest UNIQUE` | **exactly-once 是明确 non-goal**，写进文档 |
| **C6** ⭐ | **审批 = 能力授权** | Muse | 一次性 / 仅本会话 / 仅本任务 / 限时 / 长期；授权是"边界明确的能力"，不是聊天里一句话 | 需要票据载体 |
| **C7** | **单点不可绕过的审批通道** | Muse（Sentinel） | 所有出站与连接器操作经宿主侧唯一审批方；运行单元看不到真实凭证 | 集中式，单点可用性要求高 |
| **C8** | **按可逆性分级打断** | Muse | 只读 / 已获准 / 可证低风险 → 不打断；**难撤销就停**；默认允许浏览但付款必批 | 过度打断会导致"横幅盲视" |
| **C9** | **补偿注册表** | agent-git(MAS) | `reverse_tools={"process_payment": reverse_payment}` + `rollback_tools=True` | **无 handler 时会静默部分撤销**（见 §11） |
| **C10** | **补偿窗口** | 本方案 | `expiresAt` 随时间提升风险等级；超过窗口即"不可补偿的不可逆" | 需要触发者去扫（总纲缺口 G2） |
| **C11** ⭐ | **回滚原子性** | 从 agent-git 教训反推 | **回滚 = 状态回滚 + 账本活跃窗口截断 + 补偿标记，三者必须原子**；缺任一项补偿就不可重入 | —— |

---

## 5. 防重放：四条正交路线

| 路线 | 假设 | 手段 | 代表 | 适用 |
|---|---|---|---|---|
| **幂等** | 重复执行结果相同 | 幂等键 / 唯一约束 / 内容寻址 | miclaw、agit | 首选，最便宜 |
| **记忆** | 重放必然重跑 | checkpoint 复用已完成结果 | LangGraph `task()` | 只能覆盖"成功返回"的调用 |
| **意图日志** | 重跑与否**不可知** | T1/T2 + `unknown` | Maka | 有不可逆副作用时必需 |
| **补偿** | 允许重复，事后回滚 | reverse tool | agent-git(MAS) | 前三种都不成立时 |

| # | 技术点 | 关键实现点 | 边界 |
|---|---|---|---|
| **D1** | **路线优先级** | 有幂等键→用键；能查状态→读后写；都不可→**承认 unknown**；都不行→补偿+窗口 | 不要跳过第二档直接补偿 |
| **D2** | **内容寻址 commit** | commit id = 内容 SHA + 连续性检查，重复提交直接判 `Noop` | 只作用于"会话存档"，不覆盖文件 |
| **D3** | **canonicalArgs** | 顺序无关归一 / 保序归一 / 去随机 id / 空白归一 | 语义等价不能纯函数化，宁可保守 |
| **D4** | **重复意图的逃生阀** | 命中重复键时要求**显式确认**，不是拒绝 | —— |
| **D5** | **禁止伪造成功** | 所有"不执行"分支必须返回显式语义（`blocked/simulated/rejected`）并记账 | 与 A9 同一条纪律 |

---

## 6. 防环与防无进展

| # | 技术点 | 来源 | 关键实现点 | 边界与代价 |
|---|---|---|---|---|
| **E1** | **步数硬线** | LangGraph | `recursion_limit` 按 **super-step** 计数，超限抛 `GraphRecursionError`（默认值文档与源码不一致，见 §10） | 只是计数，无语义检测 |
| **E2** | **窄口径循环门** | Maka | `LOOP_GATE_IDENTICAL_THRESHOLD=3`，**只拦"同参连续失败"**，成功或任何不同调用即重置；状态 per-turn 不落盘——**刻意不误伤 polling** | 默认**无轮次上限**（`maxSteps` undefined 即无界） |
| **E3** ⭐ | **到顶也要有交代** | Maka / 01 C-11 | 达到上限时**保留已完成的工具结果**，若无可读的收尾文本则追加一条**确定性提示**告诉用户如何继续 | 与"宁可诚实失败帧"同向 |
| **E5** | **信息增益判据** | 本方案 | `gain = (参数规范形式, 结果结构化投影, 世界状态增量)`，三者全不变即无效；**投影不是全文** | 参数震荡必须靠投影识别 |
| **E6** | **前置条件失败不可重试** | 本方案 | 契约声明 `precondition`，失败返回 `{missing, retryable:false}`，同参重试直接拦 | 要把 `missing` 翻译成子目标 |

---

## 7. 可撤销

| # | 技术点 | 来源 | 关键实现点 | 边界 |
|---|---|---|---|---|
| **F1** | **撤销 vs 补偿二分** | agent-git(MAS) | *"reversion for state-independent tools, compensating actions for path-dependent tools"* | 两者不能混为一谈 |
| **F2** | **墓碑** | ADK | 撤销 = 追加"不存在"标记版本，不删除历史 | 标记格式一旦发布就锁死 |
| **F3** | **身份 ≠ 内容** | Maka | *"Workspace identity proves 'this is the same workspace,' not 'every file still has the old contents.'"*（`.maka-workspace.json` 只是一个 UUID） | 想做文件级撤销必须另存内容 |
| **F4** | **部分撤销必须显式** | 从 agent-git 教训 | 未撤项要列出来并报"部分撤销"，不能返回成功 | —— |

---

## 8. 可续跑

| # | 技术点 | 来源 | 关键实现点 | 边界与代价 |
|---|---|---|---|---|
| **G1** ⭐ | **repair ≠ resume ≠ reconcile** | Maka | repair：把卡在 running 的 run **收敛成明确终态**（不重跑任何 model 请求与工具副作用）；resume：在**已证明安全的历史边界**之后创建新执行；reconcile：解释历史证据 | 三者不能压缩成一个 `resume=true` |
| **G2** | **安全边界的语义** | Maka | *"facts before the boundary are trusted, facts after it are not"*；边界 = 已提交高水位 + 准入 gates + 执行前重读校验 | —— |
| **G3** | **续跑必建新身份** | Maka | 新 Run/Invocation/Turn + `AgentRunLineage(parentRunId, resumedFromRunId, parentTurnId)` | 不要"复活旧进程" |
| **G4** | **续跑准入条件** | Maka | Phase 1 规则：**只有当每个已接受的 tool call 都有 committed outcome** 才继续 | 有一个 unknown 就不许续跑 |
| **G5** | **取消非破坏** | ADK | *"Cancellation is non-destructive: events already committed remain persisted"*、*"No partial events"*，async generator **不抛异常**只停止产出 | 取消是"停止产生新事件"，不是"回滚已产生的事件" |
| **G6** | **checkpoint 粒度** | LangGraph | 每 super-step 一个 checkpoint；node 级 writes **不是完整快照**，time travel 只能从 super-step 边界恢复；`fork`/`update_state` **不回滚 thread** | 别再假设"能回到任意一行代码" |
| **G7** | **记忆按位置** | LangGraph ⚠️ | `task()` 的结果复用是**按位置**匹配，重排/改分支会**静默错配** | 记忆键应用调用身份（C4） |
| **G8** ⭐ | **LOG / VIEW 分离** | agit | 存档 LOG（不可变事实）与续跑 VIEW（执行视图）显式分开；**视图可丢弃重建，事实不能** | 这正是 task 层该长的样子 |

---

## 9. 人机协作与审批

| # | 技术点 | 来源 | 关键实现点 | 边界 |
|---|---|---|---|---|
| **H1** ⭐ | **审批必须在 step 边界** | LangGraph ⚠️→本方案 | LangGraph 的 `interrupt` 恢复**从 node 开头重跑**（原文："do not automatically resume execution from the interruption point… rerun the entire node"；另一处："Code and side effects **before the pause run again**"）⇒ 审批点落在 node 内部时，**批准一次 = 副作用一次** | 必须做成 step 边界事件 |
| **H2** | **审批是 typed fact，不是 UI 暂停** | Maka | 请求与决策作为事件入库，只带 `actions.stateDelta` 不带 `content`；决策事件 `role=system` 而 `author=user`——**人做的决定不伪装成一条聊天消息** | 需要 authored-by 与 role 正交建模 |
| **H3** | **"跟用户说话"的二维收敛** | 本方案 | 只用 `是否阻塞 × 是否收尾` 两个维度区分 `<ask>` / `<suggest>` / `ask_user_question` / `Signal ask` / 进度通知 | 五种机制必须有唯一映射 |
| **H4** | **干预数作为对外指标** | Anthropic autonomy 报告 | *"Claude Code's success rate on internal users' most challenging tasks doubled, at the same time that the average number of human interventions per session decreased from 5.4 to 3.3."* | 成功率有两套逃逸路径，干预数逃不掉 |

---

## 10. 上下文与状态承载

| # | 技术点 | 来源 | 关键实现点 | 边界 |
|---|---|---|---|---|
| **I1** | **reset 优于 compaction** | Anthropic harness | 模型"接近自认为的上限就开始草收尾"（context anxiety）；**清空上下文 + 结构化交接**比就地摘要更能破这个；Opus 4.5 后该行为自行消失，被删掉 | 需要高质量的 handoff 产物 |
| **I2** | **只进不回** | ADK `LoadArtifactsTool` / 本方案 Signal | 大块内容临时附加到当次请求，**不写回历史**；信号消费后只留一行投影 | 每轮上下文只留投影，不留原文 |
| **I3** | **账本拉取不推送** | miclaw / 本方案 | 框架不主动把账本塞进上下文，由需要者按需取 | —— |

---

## 11. 反模式清单（学错了会怎样）

| # | 反模式 | 谁踩过 | 后果 | 正确做法 |
|---|---|---|---|---|
| **K1** | 判官只读 transcript、不接地 | Claude Code `/goal`（相对 Anthropic harness） | 判官只能"顺着模型的话判断"，progress mirage 风险最高 | 给判官工具，让它去看真东西（A4/A6） |
| **K2** | 验证器 fail-open | Cursor Bugbot | **验证器坏了与"没问题"不可区分** | 三态输出：通过/不通过/**无法判定** |
| **K3** | 验证器 fail-noisy | Anthropic 自认 | *"A reviewer prompted to find gaps will usually report some, even when the work is sound"* → 过度工程 | 收窄靶子：只报影响正确性与既定需求的缺口 |
| **K4** | 闸门只写在 prompt 里 | Codex `/goal`（社区批评） | "stop-condition string matches" 就宣布胜利 | 闸门写进代码路径（Roo/Roo 式双门禁） |
| **K5** | 静默部分撤销 | MAS Agent-Git | 用户以为回滚了，其实撤了一半 | 未撤项显式列出（F4） |
| **K6** | 用进度清单冒充验收 | Manus | todo.md 官方自承是 *"a deliberate mechanism to manipulate attention"* | 清单必须挂验收条目（A12） |
| **K7** | 论文能力 > 源码能力 | MAS Agent-Git（merge 不存在） | 基于不存在的机制做设计 | 看有没有对应提交点（§12） |
| **K8** | `load` 默认取 latest | ADK 的最佳实践提醒 | 审计与复现失效 | 验收/审计钉死版本号 |

---

## 12. 元教训：判据是"有没有提交点"

本轮调研出现的文档/代码冲突至少三次：

| 冲突 | 表现 |
|---|---|
| LangGraph | `recursion_limit` 默认值**文档 1000 / 源码 10000→10007**；《Durable Execution》概念页**已下线**，旧的"replay 会重复副作用"警告只在 `0.6.0` tag 可读 |
| MAS Agent-Git | **论文明写的 merge，源码里不存在** |
| Maka | 博客称工具可声明 idempotency/status-check，**源码只接受两个 `recoveryMode`**，注明 "never authorizes execution" |

> **一个能力如果没有对应的写入点 / 约束 / 事务，它就只存在于文档里——而文档会下线。**
> 反过来，判断自己系统是否"真的做到了"也用同一把尺：**找那个提交点**。

---

## 13. 最小落地集（按总纲缺口排）

| 优先级 | 动作 | 借的技术点 | 补的缺口 |
|---|---|---|---|
| 1 | `EffectEntry` 拆成 **T1/T2 双提交**，引入 `unknown` 态 | C1 C2 C3 | 矩阵 B / G2 |
| 2 | 审批点移到 **step 边界**，授权做成能力票据 | H1 C6 C7 | G9 |
| 3 | 证据改 `evidenceRefs:[{key,version,hash}]`，新鲜度用**版本单调性** | B2 B1 | C6 时序 |
| 4 | task 层用 **LOG/VIEW 分离**：LOG 是事件与 Terminal，VIEW 是可重建的进度 | G8 | **G1** |
| 5 | 收尾闸门接 **agent 型钩子**（带工具的验收者），三态输出 | A5 A6 A7 K2 | L4 判定 |
| 6 | 否决原因用**封闭枚举** + 陈旧证据判定 | A9 A10 | 可观测 |
| 7 | 回滚做成 **原子三元组**（状态+账本截断+补偿标记） | C11 C10 | 副作用治理 |
| 8 | 对外只看**每会话干预数** | H4 | 度量 |

---

## 14. 一句话

**这一轮所有系统的优点，本质上都在做同一件事：把"相信模型"换成"找得到一个提交点"**——契约是提交点，版本号是提交点，T1 是提交点，停业的 hook 是提交点。区别只在于他们把这个点放在了判定链的哪一环，以及放上去之后有没有人负责扫它。
