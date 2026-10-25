# 26 · Project 自管理（第一刀：目录化 + 多任务独立对话 + 项目级注入）

> **来源**：[discuss 16](../01-discuss/16-project自管理设计档.md)（🟡 待启动）—— 长程任务升级为
> project：project = 独立目录多文件（目标/完成标准/触发/任务列表/纪律/经验），任务 ↔ 独立对话一对一，
> 最终形态是 project **自管理**；本期只做目录/多任务/独立对话/注入，自管理 loop 押后。
> 本档把第一问 6 条逐条定局，落到现有长程/多 Agent/Context 机制上。

## 一、核心定局（对 discuss 16 §五 第一问逐条）

| 第一问 | 定局 |
|---|---|
| ① project 目录与 task 档案关系 | 新增 `.wfr/projects/<p>/` 体系；**不迁移**旧 `.wfr/tasks/*`（兼容优先）。任务档案路径双轨：`@config.project` 非空 → `.wfr/projects/<p>/tasks/<t>.json`（.history 快照同目录），空 → 老路径（零改动回归）。`project.tasks` = 独立任务们；`task.plan.steps` = 某任务的多步 —— 两层，不混 |
| ② 任务↔对话一对一 + 注入 | 每任务一个独立 `--session`（现有）；项目级上下文 = **父上下文**：`project` 节点把 project 目标/完成标准/纪律/相关经验写入 `state[system_context]`（复用 Context→model 拼装点）。共享 vs 私有：项目上下文不进任务会话日志（system_context 不回溯）、任务历史不进 project 上下文 |
| ③ 经验沉淀形态 | `lessons.json` 数组 `{when, applicable_if, conclusion, evidence}`；写回通道 = 任务尾部可选产出 `state.lesson`（模型/规则），`project` end 相位追加（when=today、evidence=本批 last_summary）；begin 把 lessons 注入上下文（一期全量注入，相关性挑选留 meta） |
| ④ 项目级完成标准 | `project.completion_criteria` + **复用** `verify phase=review`（独立 Reviewer 终审）→ verdict 写回 `project.progress` |
| ⑤ 触发条件 | 入档 `project.triggers` 声明；**本期手工触发**（每任务各自 session 跑 project 版 live workflow），接 `wfr cron` 是任务名 = `project.task` 两段的浅改，无常驻调度 |
| ⑥ 自管理 loop | 只圈边界不实现（meta = 读纪律/标准/经验 → 决定下一任务 → 跑 → 写回 → 沉淀；节点形态押后） |

## 二、机制落点（都是现成件，零新机制）

- **`task` 节点档案逻辑不动**，只改一个 helper `task_archive_path(state, task)`：`@config.project` 非空时档案/历史快照整体下沉到 `.wfr/projects/<p>/tasks/`。begin/end 双相位、批还原、batch_no、recheck_prev、.history 快照全部沿用。
- **注入 = `system_context` 槽**（kSystemContextKey）：Context 节点已把"系统消息数组 → model 装配请求拼在其系统提示之后"，project 节点按同形状写入，model 自动带上 —— 零新拼装逻辑。
- **完成标准 = verify review**：Role/输入隔离/批级终审现成。
- **任务↔session = 宿主约定**：wfr 侧按 `project.task` 合成 session 名（一期 demo 直接传不同 --session）。

## 三、目录与 schema

```
.wfr/projects/<p>/
├── project.json      # 项目级唯一真相
├── lessons.json      # 经验沉淀（结构开口，meta 读回押后）
└── tasks/
    ├── <t1>.json     # 与现有 task 档案同构（plan/cursor/batch_no/last_summary/artifact_versions）
    └── <t1>.history/batchN.json   # 批间可改 / revision 续跑（与 task 同纪律）
```

`project.json`：
```jsonc
{
  "id": "monthly_ops",
  "goal": "…", "completion_criteria": "…",
  "discipline": ["…"],
  "triggers": [{"task": "weekly", "on": "cron:0 9 * * 1"}],   // 入档；本期手工触发
  "tasks": [ {"id": "weekly", "name": "周报", "description": "…"}, … ],
  "progress": { "weekly": "done", "monthly": "in_progress", "review": "" }
}
```
`lessons.json`：`[ {"when":"2026-10-25","applicable_if":"…","conclusion":"…","evidence":"…"}, … ]`

## 四、新节点 `project`（kind=Project，phase=begin|end）

| 相位 | 行为 |
|---|---|
| **begin** | 读 `.wfr/projects/<project>/project.json`（缺 → `WorkflowError` fail-closed；project 名取 `@config.project` 或字面量 `n.project`）→ 校验 `@config.task` ∈ `tasks[]`（不在 → 报错 fail-closed）→ 读 lessons.json → 拼 `system_context`（项目目标 + 完成标准 + 纪律 + 经验 + 本任务目标）→ 写 `state.project`（project.json 摘要，供任务内 review/参考展示） |
| **end** | 若 `state.lesson` 非空（约定槽，任务内节点可写 `{applicable_if, conclusion}`；宿主也可经 `@config.lesson` 提供）→ 追加 lessons.json（when=today、evidence=本批 last_summary）；更新 `project.json.progress[task]`（按 task end 的状态标 done/in_progress）；全部**整文件原子写**，缺档案/缺 progress 键 tolerant 兜底 |

IR 字段：`NodeIR.project`（名/引用）+ `NodeIR.phase`（复用 task 的 phase 字段）+ next。ref 文本 = project 字段（VR1 收集）。

## 五、发布 workflow `lot_project`（lot 的 project 版，share/wfr/workflows/）

```
load_history(@config.session) → project begin → task begin(@config.project 下沉) →
  loop{ verify announce → model → route → approve → run_tools → verify judge } →
  final model(批间摘要) → task end → project end
```
与 lot 只差头尾：project begin/end 包在 task begin/end 外。后续若做项目级 review（完成标准），在 task end 后加一个 `verify phase=review`（独立 Reviewer，判 project.goal）。

## 六、边界

- **自管理 loop 本期不做**；相关性经验挑选押后（一期全量注入）。
- 不做 agent 间自由对话 / 自协调循环；不迁移旧 `.wfr/tasks/*`。
- `state.lesson` 目前是"任务内节点可选产出"（模型/规则写），谁产经验的引导（任务结尾的沉淀节点模板）进 demo，不进引擎。
- 项目级 review（completion_criteria 自动判定）本期只接 verify review 语义，workflow 里怎么编排由用户案例定（demo 可加一个独立 Reviewer 终审行）。

## 七、测试 / demo / 验收

- 单测 `tests/wf/project_test.cpp`（预计 ~12 例）：begin 读档+校验任务 ∈ tasks（缺档/不在列表 fail-closed）、system_context 注入含 goal/discipline/lessons、`@config.project` 归档下沉（+历史快照同目录）、`@config.project` 空回归老路径、end 写回 lessons、end 更新 progress、兼容回归（task 不带 project 原样）。
- demo `examples/project_run` / wfr 冒烟：project「月度运营综述」3 任务（周报/月报/复盘）各自独立 session 分批推进，验证档案独立 + 注入可见 + 经验沉淀。
- 验收 problems/49（下一个空闲编号；48 已被 P1.5 声明式配置占用）。