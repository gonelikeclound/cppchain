# 56 · wfr cron 触发接入（使能②，project.task 两段）验收

> **链路**：problems/12 §三 使能②「到点再跑」押后 → arch 27 §十（增量项）→ §十一 → 本验收。
> **验收标准（arch 27 §十一）**：cron job 支持 `project` 字段 → 触发 lot_project（project 版单批）+
> 两段 session + plan_steps 注入（含单步兜底）；6 新单测；cron E2E 冒烟；全仓全绿。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `wfr_lookup.hpp/cpp` | 新 `wfr_cron_batch(workdir, job, self)`：`.wfr/cron.json` job → 拉起批次 argv（`WfrCronBatch{args, workflow, task, session, project_mode}`）。**project 字段**非空 → `lot_project` + `--set project=…/task=…/plan_steps=…` + goal（job.goal 覆盖任务 goal）+ session 缺省 `"<project>.<task>"`；plan_steps 读 project.json 该任务 steps（无 → **单步兜底**，与 drive 自适应一致）；无 project → `lot`（现状兼容） |
| `src/wfr/main.cpp` | `wfr cron` 分支改用 helper（fire 行打印 workflow/task/session）；last_run 更新不变 |
| 测试 | `tests/wf/cron_batch_test.cpp` 6 例（见 §二） |

### 关键语义

1. **两段任务名 = 每任务独立对话**：`session` 缺省 `"<project>.<task>"`（arch 26 ⑤）——cron 到点触发的
   是该任务的一批（lot_project），不是整个项目驱动（驱动靠 `wfr project_drive` 手工/编排）。
2. **plan_steps 注入**：cron 进程读 project.json 该任务预置 steps → `--set plan_steps=<dump>`；无预置 →
   单步兜底（与 drive 的自适应兜底同规则，免 missing_seed 空转）。
3. **现状兼容**：无 `project` 字段的旧 cron job 走 `lot`，argv 与旧实现一致（零行为变化）。

## 二、单测（tests/wf/cron_batch_test.cpp，6 例）

| 用例 | 断言 |
|---|---|
| `legacy_job_triggers_lot` | 无 project → workflow=lot、session=task、带 task/approve_policy，不带 project/plan_steps |
| `project_job_reads_steps_and_goal` | project → workflow=lot_project、session="monthly_ops.weekly"、plan_steps=预置 steps dump、goal=任务 goal |
| `project_job_falls_back_single_step_plan` | 无预置 steps → plan_steps 单步兜底（s1/任务目标） |
| `job_goal_overrides_task_goal` | job.goal → goal 用 job.goal |
| `explicit_session_wins` | job.session → session 覆盖两段缺省 |
| `empty_args_when_task_missing` | task 缺失 → args 空（cron 分支跳过） |

### cron E2E 冒烟

`wfr cron --workdir <ws> --once`（`.wfr/cron.json` job 带 `project`）→ 触发批次、`last_run` 更新、进程退出 0。
批次完整执行依赖 LLM（lot_project 有 model 节点，与既有 lot cron 同）——装配/调度与 last_run 是本刀验证点。

## 三、回归与全仓

- workflow_tests 389 → **395**（+6）；全仓 **907 全绿**（agent 100 / graph 55 / message 20 / model 59 /
  state 43 / tool 226 / webui 9 / workflow 395，零失败零错误）。
- 既有 lot cron / wfr_lookup 测试原样通过——无 project 字段的 job argv 与旧实现逐元素一致。

## 四、备注
- 触发 workflow 名固定 lot_project；`wfr project_drive`（整项目驱动）仍需手工/编排（cron 语义=单任务一批）。
- 下一步增量仍可选：真机量化 meta 引用经验比例、依赖图失败专线重做路由等（arch 27 §十）。