# 16 · Project 自管理设计档（长程任务下一跳 · 🟡 已记录待启动）

> **定位**：05 #4/#5/#3 三线的汇合下一跳。长程任务（arch 20 + `lot` + `wfr cron`，见 [arch 22](../02-arch-design/22-长程任务实现全景.md)）已把
> "**单任务跨批续跑**"做扎实；本题把长程任务升级为 **project**：一个 project = 自有独立目录，
> 目录内**多文件**承载目标/完成标准/触发条件/任务列表/纪律/执行情况/经验沉淀；project 内的
> **每个任务 = 一个独立对话**（独立历史、独立 agent 上下文）——最终目标是 project 的**自管理**。
> 状态：🟡 已记录、待启动（先立「第一问」，不实现）。**自管理闭环押后**：先做目录/多任务/独立对话
> 与项目级注入，自管理（meta 决定下一个任务 + 经验回流）作为远期收口。

## 一、动机：长程落地后暴露的"项目级"缺口

1. **长程的 `lot` 是真"单任务多批"**：`task` 档案（`.wfr/tasks/<task>.json`）= 一个目标 + steps 全清单 +
   cursor + batch_no + last_summary。它长得像"一个 project 的**一件事**"，而不是"一个 project"。
   实际工程的形态是**一个目标下挂着多个子任务、各自独立推进、共享同一套纪律与经验**——现有单文件
   steps 列表表达不了"多个可独立对话的任务"。
2. **会话=任务，不=项目**：现在 `--session NAME` 隔离的是"一个任务的多批"。project 需要**任务↔对话一对一**
   （每个任务有自己的历史、自己的 agent 上下文），而 project 级的目标/纪律/经验是**父上下文**——正好
   落在批③ 交接件/输入裁切的延展线上（角色看**该角色该看的东西**，任务看**该任务该看的东西**）。
3. **自管理需要"运行记录 → 经验 → 规则 → 影响下次"**：`tools/export_task_life.py` 已导出任务档案 +
   plan_log verdict（05 #5 的现成数据源）。project 化给它一个结构化开口：经验沉淀不再甩进自由文本
   `last_summary`，而是每任务的独立"经验槽"（学到什么、适用条件），供 meta 层读回。

## 二、两级心智模型（贯穿设计）

- **任务任务（现有，含 22 全景）**：一个 `task` = 一个档案 + 一个 session（多批续跑），单 steps 计划。
- **Project** = 多任务容器：目标/完成标准/触发/纪律/经验是**项目级**资产（父上下文）；任务列表里每项 =
  一个**独立对话**（独立 session/历史），执行时注入项目级纪律+相关经验。

```
.wfr/projects/<p>/
├── project.json          # 目标 / 完成标准 / 触发条件 / 纪律指针 / 任务列表 / 进度（项目级唯一真相）
├── lessons.json          # 经验沉淀（结构化：时机 / 适用条件 / 结论）—— 供 meta 读回 + 自进化吃
├── tasks/                # 每个任务一个档案（与现有 .wfr/tasks/<task>.json 同构，移入 project 下）
│   ├── <t1>.json
│   └── <t2>.json
└── ...
```

## 三、期望能力（分步走，自管理押后）

| 能力 | 含义 | 本期（先做）/ 押后 |
|---|---|---|
| **目录化多文件** | project = 独立目录，目标/完成标准/触发/任务列表/纪律/经验分文件承载 | ✅ 本期核心 |
| **多任务 + 独立对话** | 任务列表每项 = 独立 session、独立历史；非"一任务多步" | ✅ 本期核心 |
| **项目级上下文注入** | 任务开局注入 project 目标 + 纪律 + 相关经验（= 批③ `input_filter`/交接件的父上下文化） | ✅ 本期核心 |
| **完成任务标准** | project 级完成标准/DoD（对 project.json goal 的整体判据，非逐批） | ✅ 本期（复用 verify/review） |
| **触发条件入档** | 项目声明"什么触发哪个任务"，接现有 `wfr cron` | ⚠️ 接现有件，浅做 |
| **经验沉淀结构化** | lessons.json（时机/适用条件/结论），任务完结写回 | ⚠️ 结构化落盘，先做 |
| **自管理 loop** | meta 层读 纪律+标准+经验 → 决定下一个任务 / 更新列表 / 沉淀经验 | 🔮 **押后**（远期收口） |

## 四、现有垫底（比 12 建档时厚得多，直接可接）

- **长程全系**：`task` begin/end 档案（`.wfr/tasks/`）、`lot` 一次触发=一批、`.history/batchN.json`
  revision-fork、recheck_prev 批间 L3 复查、`wfr cron` 到期触发、时间感 `📅 第N批@天`
  （arch 20 + problems/27/28 + [arch 22](../02-arch-design/22-长程任务实现全景.md)）。
- **多 Agent 角色化 + 交接件**：`mch` 三角色（Planner/Executor/Reviewer）、`executor/parallel` 并行波、
  `handoff` 交接件 + `.wfr/handoffs/` 落盘、`--max-parallel`、Reviewer 输入隔离（只读判定+判据）
  （arch 21 + problems/29/30/31）。
- **会话即任务隔离**：`--session NAME` 已支持独立会话日志（replay 投影），每任务独立对话 = 每任务一个
  session（现成）。
- **自进化数据源**：`tools/export_task_life.py`（任务档案 + plan_log verdict 导出）—— lessons.json 可并入。
- **上下文裁剪机制**（project 注入的底座）：`judge_batch_review` 输入隔离 + `refine_history` 规则引擎 +
  `summarize` LLM 折叠（做"注入纪律/经验"时可复用）。

## 五、第一问（启动切入点）

1. **project 目录与现有 task 档案的关系**：project.json 是新建，还是把 `.wfr/tasks/<task>.json` 迁入
   `.wfr/projects/<p>/tasks/`？兼容旧路径（`.wfr/tasks` 读档兜底）还是硬迁移？任务列表如何与
   `steps[]` 区别（project.tasks = 独立任务们；task.tasks.steps = 某任务的多步）？
2. **任务↔对话的一对一**：任务列表每项对应一个独立 session；项目级纪律/经验怎么进每个任务的上下文
   （load_history 之后注入 project 摘要？复用交接件/裁剪的手法？看不看全局项目对话——哪些是共享记忆、
   哪些是 per-task 私有，沿用 15 第一问② 的平衡）？
3. **经验沉淀的形态**：lessons.json 结构（`{when, applicable_if, conclusion, evidence}`）？谁写回——
   任务完结的 task end 相位 → meta 收口（押后）之前先手工/节点落盘？与 `last_summary`（自由文本）的
   关系（升级 or 并存）？
4. **项目级完成标准**：对 project.goal 的整体 DoD 判定 = 复用 `verify review`（Reviewer 独立终审批③ 已
   够）还是需 project 级`judge`？触发条件（何时做哪个任务）第一版就人/编排出，meta 押后？
5. **项目级触发**：接 `wfr cron`（cron.json 里任务名 = project+task 两段）改造幅度？还是先手工触发？
6. **自管理 loop 的形状（仅顶层约定，不实现）**：meta = 读 project.json 纪律/标准/经验 → 决定下一任务 →
   跑（复用 executor/M10）→ 写回执行情况 → 沉淀经验。它的节点形态（一个 `project_think` 节点？还是
   复用 plan/review）留给后续，先圈边界。

## 六、验收预想（按基调，本期范围=目录/多任务/独立对话/注入）

- demo `project_run`：一个 project（如"月度运营综述"）含 3 个任务（周报/月报/复盘），各自独立 session
  分批推进；断言：project 下 3 个任务档案独立存在、各 session 历史互不污染、每任务开局能看到 project
  目标+纪律（注入口径可探）、任务间产物可交接（复用 handoff）。
- 全仓全绿 + problems/NN 验收记录。自管理 loop 不在本期验收内（标注押后）。

## 七、边界与不做的

- **自管理 loop 本期不做**（meta 决定下一个任务 + 经验回流押后；先让结构成立）。
- 不做：agent 间自由对话/自协调循环；多模型厂商混跑（沿用 21 边界）。
- 不迁移历史 `.wfr/tasks/*`（兼容优先，新项目走 project 目录；迁移路由列为第一问解决项）。
- 与 #5 自进化的接点：`export_task_life.py` 扩到 lessons.json（押后）。

## 待办锚点
- 关联：05 #3（长程）/#4（多 Agent）/#5（自进化）；arch 20（长程核心）+ arch 21（角色化/交接件）+ arch 22（长程全景）；problems/27/28（长程）、29/30/31（多 Agent）、13（时间旅行）。
- 启动第一步（可商）：**project.json 多文件目录 schema + 任务↔session 一对一 + 项目级注入** 这条线先落 arch，再出一版 `project_run` demo。
- 备注：用户在 2026-10-16 提出此方向（把长程任务看作 project，独立目录多文件自管理），已定调"建 discussion 档 + 自管理押后"。