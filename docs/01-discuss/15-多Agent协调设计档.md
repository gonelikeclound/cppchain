# 15 · 多 Agent 协调设计档（✅ 选 A 启动 · wfr-㊾ 批① 已落地）

> **定位**：05 未来专题 #4 的设计稿。前两道已开：#2 复杂任务（PEV，arch19/11）、
> #3 长程（wfr-㊿，arch20/14）。本稿：**把"单 Agent 的长任务"升级成"多 Agent 任务级角色编排"**——
> 在已有的 M8 子代理隔离 + PEV 的 plan/judge/final 角色雏形上，定义协调原语与第一问。
> **进度**：2026-10-15 **选 A（角色化 PEV）起步，批① 批② 均已落地** —— `mch` 三角色 workflow
> （Planner / Executor / 独立 Reviewer 批级终审）+ 角色身份入 plan_log + 真机对照（problems/29）；
> Send 并行波（按 deps 派发互不依赖步骤）+ 冲突护栏（problems/30）。见
> [arch 21](../02-arch-design/21-多Agent角色化PEV设计.md)。

## 一、动机（为什么现在值得做）
1. **PEV/长程暴露的单 Agent 墙**：同一模型既当执行者又当裁判（`judge` 是"自己审自己"）；长程里 Agent 前面
   忘后面。多 Agent 的价值 = **职责分离**（规划者/执行者/裁判者/解说者）+ **并行**（读多任务）+ **隔离**（M8 已有）。
2. 五框架（08）都给了多 Agent 原语：deepseek（Spaw + Agent Teams 的 `blockedBy` 无环 DAG）、LangGraph
   （subgraph + `Send` 并行派发 + orchestrator-worker）、SDK（`handoff` 交接 + agent loop）、Gemini（subagents/
   `interrupt_agent`）、OpenHands（趋避）。**原语在同构上可用**，我们的落点是"接 PEV/长程"。
3. 长程的**任务档案 + 每批 verdict** = 天然的多 Agent 共享记忆（per-task），会话日志 = 全局史。

## 二、垫底盘点（已具备，直接可接）
- **M8 多智能体**（arch 09 + problems/09 真机三形态：ask_expert / orchestrator-串行 / handoff）+ `as_agent_node`；
  subagent 隔离（SideEffect/审批在子代理侧也生效）。
- **PEV 角色雏形**：`plan`（规划）、loop 的 think（执行）、`judge`（裁判）、`final`（解说）——同一模型但已在图里分职责。
- **长程**：任务档案（per-task 记忆）+ `lot` + 批号/时间感；`recheck_prev` 已是"跨角色复查"的雏形。
- **执行防护**：ExecutionGuard/审批/沙箱/审计（多 Agent 并行改 state 的冲突/副作用有底座）。
- **测试/对照**：wf 环 harness、真机批 runner、problems 线。

## 三、可选方向（第一问的候选切入，不都做）
| 方向 | 内容 | 接现在的 |
|---|---|---|
| **A 角色化 PEV** | 把 `plan/judge/final` 升级成**独立角色**（不同 system/职责/不被同一段上下文污染）：Planner 产任务谱系 → Executors 每人领批 → Reviewer(judge) 独立复核 → 解说者 final | PEV 图已有雏形，改动最小 |
| **B 交接式执行** | 两角色接力：A 完成任务产出 → 显式 `handoff` 给 B（带 `input_filter` 脱敏/摘要）→ B 续做；验证跨角色"交接件"契约（类似长程批摘要） | M8 handoff + 长程批摘要 |
| **C 共享任务 DAG** | 角色间用 `blockedBy` 无环 DAG（deepseek Agent Teams 同款）+ peer `send_message`；任务板在 `.wfr/tasks/`（长程档案扩展成多角色） | 长程档案 + Agent Teams 契约 |
| **D 并行执行器** | 多个 Executor 并发领互不依赖的步骤（LangGraph `Send` 同款）：各自 log/产物、主协调者收拢；冲突靠 ExecutionGuard/沙箱 | send？M10 `Send` + 现有并行库 |

## 四、第一问（启动时依次打）
1. **角色怎么落地**：图节点（一个 model 节点带 role system）vs 独立子工作流（subagent/`as_agent_node`）？——决定"职责分离"的粒度。
2. **共享记忆切法**：全局会话日志（都看得见）⇄ per-role 私有上下文（隔离）怎么平衡；角色间"交接件"用长程批摘要同款契约吗？
3. **并行度**：同一批内多个 Executor 并发 vs 串行接力；`Send` 能不能复用到"依据依赖批量派发"。
4. **验证**：Reviewer 独立于 Executor 会不会比"自审"更可信补齐（对照 PEV 的 `judge`）；跨角色产物怎么探（复用 verify rule/probe）。
5. **放哪**：先做角色化 PEV（A）还是交接式（B）——A 改动小能先见三观，B 与长程批摘要接合。

## 五、验收预想（按基调）
- `examples/multi_agent_demo`（或 workflow）：Planner 生成计划 → 两个 Executor 分领（可并行）→ Reviewer 独立裁判 → final 汇总；
  断言：角色上下文隔离（Executor 看不到裁判内部）、交接件契约成立、verify/审计在每位角色侧都生效、全绿。
- 真机真一轮（两角色接力完成一个任务，产物可探），对照"单 Agent 同任务"看可信/可查差别。

## 六、边界
- 不做：agent 间自由对话循环（避免人格化/失控）；不做全局神化协调器。
- 与 #5 自进化的接点：多 Agent 的交接件/裁判记录也是自进化可吃的运行数据（`export_task_life` 可扩展按角色导出）。

## 待办锚点
- 关联：05 #4 · M8（arch09/problems09）· PEV（11/19）· 长程（14/20/28）· **wfr-㊾ 批①（arch 21 / problems/29，✅ 已落地）· wfr-㊾ 批②（arch 21 §四批② / problems/30，✅ 已落地）**。
- 下一步：批③（交接式 A→B handoff 交接件契约，arch 21 §四批③）——顺带把并行 Executor 接进 mch 真机对照。