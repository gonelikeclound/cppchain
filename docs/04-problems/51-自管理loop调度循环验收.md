# 51 · 自管理 loop 第一刀：project_drive 调度循环验收（arch 27）

> **链路**：problems/49/50 遗留「自管理 loop」→ [arch 27](../02-arch-design/27-自管理loop调度循环设计.md) → 本验收。
> **验收标准（arch 27 §四）**：project_drive 节点（规则/模型双档）+ proxyNode 运行期动态分派 +
> 驱动循环 workflow + 6 新单测 + 离线 demo + 全仓全绿。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `ir.hpp` | `NodeKind::ProjectDrive`（新节点类型）；复用 `NodeIR.project/model/system_prompt/next`，无新字段 |
| `node_types.cpp` | `v_project_drive`（VN26 查 next）/ `c_project_drive` / `t_project_drive` + 注册：读 project.json（fail-closed 缺档）→ 简报（目标/验收标准/纪律 + 每任务进度/摘要）→ **规则档**（无 model）= 第一个 `progress≠done` 的任务（带该任务 `tasks[].steps` 计划种子）；**模型档**（可选 judge）= 简报+system_prompt 决策 `{task\|null, done, reason}`，**选已 done/未知任务 → 拒绝回落规则档**；产出 `results.<id>={next_task\|null, done, plan_steps, goal, reason}` |
| proxyNode | `input.config = {...}` → 与**子 config 逐键合并**（不是整块覆盖）：project_drive 把 `task/project/plan_steps/goal` 动态注入子 workflow 的 `@config.*`；向后兼容（现有 input 无 config 键） |
| `validator.cpp` | VR2 白名单加 `project` / `project_drive`（expr 门 / subworkflow input 引用其 result） |
| 发布模板 | `share/wfr/workflows/project_drive.json`：`loop{max=50} body: [drive → gate(expr: next_task!=null) → run(subworkflow lot_project)]` |
| demo | `examples/project_drive`：离线规则档 3 任务跑完 + 停机 + 档案落项目目录 |
| 测试 | `tests/wf/project_drive_test.cpp` 6 例（见 §三） |

### 关键语义

1. **停机判据**：drive 产出 `next_task:null`（规则档全 done / 模型档判 done）→ expr 门 `next_task != null` 不成立 → `_break` 收尾。loop `max` 兜底防病态（任务永不 done 也不挂死，走 on_max）。
2. **运行期动态分派**：subworkflow 编译期装载 `lot_project`，但 `input.config` 里的 `$drive.result.next_task` 在**运行期**解析（NamedResult 引用）→ 子 config 获得当前任务名 → `@config.task` 命中。任务档案/进度写回全由子 workflow 的 task/project 节点完成。
3. **规则档 vs 模型档**：无 model = 顺序推进（确定性、可离线测试）；有 model = meta 决策（可挑任务/判 done），选已 done 拒绝（fail-closed）+ 解码失败回落规则档。

## 二、demo 验收（examples/project_drive，离线，无模型）

| 断言 | 结果 |
|---|---|
| 循环按序跑完 weekly → monthly → review（每轮 subworkflow sync 动态分派） | ✅ progress 三个全 done |
| 末轮停机：next_task=null、done=true、reason「项目全部任务 done（规则档停机）」 | ✅ |
| 任务档案落 `.wfr/projects/monthly_ops/tasks/`（input.config 合并、主 workspace 未丢） | ✅ 三个 ✓ |
| loop 上界兜底（任务永不 done 时走 on_max 不挂死） | ✅ 单测覆盖 |

## 三、单测（tests/wf/project_drive_test.cpp，6 例）

| 用例 | 断言 |
|---|---|
| `rule_selects_first_unfinished` | 空进度 → next_task=weekly；done=false；plan_steps 带 steps 种子 |
| `rule_stops_when_all_done` | 全 done → next_task=null、done=true（停机判据） |
| `model_selects_task` | ScriptedModel 挑 monthly → next_task=monthly、reason 含「模型档」 |
| `model_picking_done_task_falls_back_to_rule` | 模型选已 done(weekly) → 回落规则选 monthly、reason 含「回落」 |
| `loop_runs_all_tasks_and_stops` | 3 任务 subworkflow 动态分派全 done；末轮停机；档案落项目目录（input.config 合并语义被覆盖） |
| `loop_max_bounds_runaway` | 任务永不 done（probe 缺）→ loop 触顶走 on_max 正常收尾（不挂死） |

## 四、回归与全仓

- workflow_tests 362 → **368**（+6）；全仓 **880 全绿**（agent/graph/message/model/state/tool/webui/
  workflow 100/55/20/59/43/226/9/368，零失败零错误）。
- proxyNode 系列原样通过（input 无 config 键时行为零变化）。

## 五、遗留（下一刀）

- **meta 从 lessons 学习改进决策**（自管理的"学习"层）：本期 lessons 只沉淀、不回流决策——真正的
  「读经验 → 改调度/选任务优先级」押后。
- **任务依赖图调度**（非顺序）：本期规则档=顺序；模型档可挑但无显式依赖约束。
- **评审 fail 后的处置**（problems/50 §四）：自动重跑未达标任务仍由 human/meta 驱动（loop 会再次
  选它直到 done 或 max），无专门的"fail→重做"路由。
- **自适应 plan_steps**：本期 tasks[].steps 预置；"plan 由 meta 模型产"未接。