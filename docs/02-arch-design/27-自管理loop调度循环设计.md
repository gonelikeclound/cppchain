# 27 · 自管理 loop 第一刀：project_drive 调度循环（meta = 决定下一个任务 → 跑 → 收口）

> **来源**：[discuss 16](../01-discuss/16-project自管理设计档.md) §三 自管理 loop（押后）+ [arch 26](26-project自管理设计.md)
> §六/§九 + problems/49/50 遗留。自管理 loop = meta：读 project 纪律/标准/经验 → **决定下一个任务** →
> 跑（复用批次执行）→ 写回 → 沉淀经验。本档落第一版：**调度循环**（meta 决策 + 动态分派 + 停机判据），
> "meta 从经验学习改进决策"仍押后。

## 一、形态定局（arch 26 第一问⑥ 的落地）

| 项 | 定局 |
|---|---|
| 决策载体 | 新节点 `project_drive`：读 `.wfr/projects/<p>/project.json`（progress/tasks/steps/lessons）→ 产出**下一个任务**（或 done 停机）。**双档**：规则档（无模型）= 第一个 status≠done 的任务顺序推进；模型档（可选 judge 模型）= 读项目简报 + system_prompt，由模型挑任务或判 done |
| 执行载体 | **proxyNode 运行期动态分派**：subworkflow 调 `lot_project`，`input.config` 把 drive 选中的 `task/project/plan_steps` **逐键合并进子 config**（`@config.task` 命中）——新机制：proxyNode 的 `input.config` 从"整块覆盖子 config"改为"与主 config 逐键合并" |
| 循环 | `project_drive` 发布 workflow：`loop{ body: [drive → gate(expr: next_task!=null) → run(subworkflow) ] }`；`next_task==null`（全 done / 停）→ `_break`；loop `max` 做防病态兜底（一批任务可跨多轮多批） |
| 停机判据 | 项目全部任务 progress==done → drive 产出 `next_task:null`（规则档）；模型档由模型判 done |
| 写回 / 沉淀 | 复用子 lot_project 的 project begin/end（每任务进度写回 progress + state.lesson→lessons.json）+ 第二刀项目终审（completion） |
| 保留押后 | meta 从 lessons 学习改进决策；任务依赖图调度（非顺序）；自适应 plan_steps（本版 tasks[].steps 预置） |

## 二、mechanism 落点

- **`project_drive` 节点**（kind=ProjectDrive，字段 project/model/system_prompt/next）：
  - 读 project.json（fail-closed 缺档）→ 简报同第二刀格式（目标/验收标准/纪律 + 每任务进度/last_summary）。
  - 规则档：第一个 `progress[tid] != "done"` 的任务 → `{next_task, reason, plan_steps(tasks[].steps JSON 串), goal}`；无未完成 → `{next_task:null, done:true, reason}`。
  - 模型档：简报 + system_prompt → 模型输出 `{"task":"weekly"\|null, "done":bool, "reason"}`；task null/done → 停机。
  - 停机会被 gate 吃住（`$drive.result.next_task != null` → else `_break`）。
- **proxyNode input.config 合并**：`input: {"config": {...}}` → 逐键并入子 config（不整块替换主 config）。
  子 workflow（lot_project）task begin 读 `@config.task` → 命中 drive 传的值。向后兼容：现有 input 无 config 键。

## 三、发布模板 `project_drive.json`

```
loop{max=50, next:_turn_end, on_max:_turn_end}
 body:
   drive   (project_drive, project:@config.project, model:main[可选], next:gate)
   gate    (expr: "$drive.result.next_task != null" → run / _break)
   run     (subworkflow sync lot_project,
            copy_state:[session],
            input:{config:{ task:$drive.result.next_task,
                            project:@config.project,
                            plan_steps:$drive.result.plan_steps,
                            goal:$drive.result.goal }},
            next:_continue)
```

## 四、demo / 测试 / 验收

- 单测 `tests/wf/project_drive_test.cpp`：规则档顺序推进全 done 并停机 / 已 done 任务跳过 / 模型档挑指定任务 /
  loop 上界防病态 / proxyNode input.config 合并。测试用无模型的 chain 子 workflow（project→task→task→project）。
- demo `examples/project_drive`：离线规则档「monthly_ops」3 任务（各带 steps seed）跑完 → progress 全 done +
  next_task null 停；模型档（ScriptedModel）挑 monthly 先做。
- 验收 problems/51；全仓全绿。