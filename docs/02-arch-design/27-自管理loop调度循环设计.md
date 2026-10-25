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

- 单测 `tests/wf/project_drive_test.cpp`（11 例）：规则档顺序推进全 done 并停机 / 已 done 任务跳过 /
  模型档挑指定任务 / 模型选已 done 回落 / loop 上界防病态 / proxyNode input.config 合并；
  **经验回流 5 例**（§五）：加权重排 / 通用经验计而无序 / 无关过滤 / 加权跳过 done / 模型档简报含相关经验。
  测试用无模型的 chain 子 workflow（project→task→task→project，probe 判 done）。
- demo `examples/project_drive`：离线规则档「monthly_ops」3 任务（各带 steps seed）跑完 → progress 全 done +
  next_task null 停；含一条 applicable_if=weekly 的经验（经验优先演示）。
- 验收 problems/51（调度循环）+ problems/52（经验回流）；全仓全绿。

## 五、第二刀：经验回流决策（meta 从 lessons 学习改进决策，problems/52）

problems/49/51 押后的「meta 从 lessons 学习改进决策」落地为 project_drive 的经验增强：

1. **相关性挑选**：drive 读 `lessons.json`，按 `applicable_if` 命中（空 = 通用经验总相关；非空 = 与任务
   id/name **双向子串匹配**）筛出**相关经验**；无关经验不灌上下文（防噪音）。
2. **规则档加权**：候选未完成任务排序 = applicable_if 命中的未完成（pref）排前，其余按文件序跟上。
   对应任务已 done → 不参与加权（只作用于未完成）。
3. **模型档决策依据**：简报新增【项目经验（lessons，仅相关…）】段（编号 [lesson N]）；缺省 meta
   system_prompt 要求参考经验、reason 引用经验时以 `[lesson N]` 标注（**可审计的回流锚点**）。
4. **可观测**：drive 结果带 `lessons_used`（相关条数计数）。
5. **语义边界**：经验只**影响**决策（排序/上下文），不保证模型必然套用——"学习"的判定 = 经验
   进得了上下文、用得上（引用锚点）、可审计；模型是否真套用由调度案例实测（真机待跑）。

## 六、保留押后（沿 §四）
- 评审 fail 专线重跑路由。
- （任务依赖图调度已在 §七 实现；自适应 plan_steps 已在 §八 实现。）

## 八、第四刀：自适应 plan_steps（meta 产 plan，problems/54）

任务计划不再只依赖预置 `tasks[].steps`：

1. **单步兜底（规则档）**：无/空预置 steps → 产出单步计划 `[{"id":"s1","goal":任务目标}]`，子 task begin
   可建档（原 missing_seed 空转会反复选同任务 → 现在能跑、能完结回写）。
2. **模型产 plan（模型档）**：选中任务无预置 steps 时，模型可同轮输出可选 `plan_steps` 数组（每步须含
   `id`/`goal`）；合法 → 用模型计划；非法 → 回落单步兜底（fail-closed）。
3. **预置优先（幂等）**：预置 `steps` 存在 → 恒用预置，模型 plan_steps 不覆盖（每轮决策可预期）。
4. **可观测**：drive 结果带 `plan_source = preset | model | fallback`；规则档 reason 附「自适应单步兜底」。
5. prompt 给模型说明：无预置时可输出 `plan_steps`；有预置则不输出。

语义：首版计划来源自适应；跨批计划更新仍走长程语义（批次间档案保留 + recheck_prev 复查），
meta 产 plan 只影响任务**建档**的首版步骤。单测 +5，全仓 896。

## 七、第三刀：任务依赖图调度（problems/53）

`tasks[].deps = ["<任务id>"]` 声明任务级前置依赖；`project_drive` 运行期做**依赖就绪遴选**：

1. **就绪判定**：未完成 且 `deps` 全部 `progress==done` 才算就绪；缺失/未知/非字符串依赖 → 不满足（fail-closed）。
2. **规则档**：候选 = 就绪的未完成（文件序），再走既有经验加权。**依赖门**：早期任务依赖未满足也会被挡下，
   选依赖已就绪者。
3. **阻塞停机（新停机判据）**：有未完成但**无一就绪**（环 / 缺失依赖 / 依赖任务在途）→ `blocked=true`、
   `next_task=null`（gate 照常 `_break`）、`done=false`（**阻塞 ≠ 完成**）——不静默死循环（loop max 兜底仍在）。
4. **模型档**：简报每任务带 `依赖:[…]`；提示词要求只能选依赖就绪任务；模型选未就绪任务 → 拒绝回落规则档。
5. **结果槽**：`blocked` / `ready`（就绪数）/ reason（阻塞文案点明"依赖未就绪"）。
6. 模板/工作流无需改动（gate 仍判 `next_task != null`）。

单测 +6（依赖门 / 解锁 / 环阻塞 / 缺失依赖阻塞 / 模型拒选未就绪 / 循环按依赖序推进），全仓 891。