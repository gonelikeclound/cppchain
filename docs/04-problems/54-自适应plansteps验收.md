# 54 · 自适应 plan_steps（meta 产 plan，arch 27 第四刀）验收

> **链路**：problems/51 §五 → arch 27 §六（押后）→ §八 第四刀 → 本验收。
> **验收标准（arch 27 §八）**：规则档单步兜底 + 模型档产 plan + 预置优先 + plan_source 可观测；
> 5 新单测；全仓全绿。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `node_types.cpp` `c_project_drive` | 新 `ensure_plan_seed`（无/空预置 steps → 单步计划 `[{"id":"s1","goal":目标}]`）；`plan_source` 槽（preset/model/fallback）；结果加 `plan_source` |
| 同上 · 规则档 | 选中任务后：预置 steps 缺/空 → 单步兜底（reason 附「无预置 steps → 自适应单步兜底」）——消除 "缺 seed → 子 task begin missing_seed → drive 反复选同任务" 的空转死循环 |
| 同上 · 模型档 | 选中任务无预置 steps 时，模型可同轮输出可选 `plan_steps`（每步须含 id/goal，非法 → fail-closed 回落单步）；预置 steps 存在 → 模型 plan_steps 不覆盖（幂等） |
| 同上 · prompt | 缺省 meta 提示加：无预置可输出 `plan_steps:[{"id","goal"}]`；有预置则不输出 |
| 测试 | `project_drive_test.cpp` +5（见 §二） |

### 关键语义

1. **首版计划来源自适应**：预置 steps（部门预置）→ 模型产（无预置 + 模型档）→ 单步兜底（无预置 + 规则档）。
   三级递退，任何情况子 workflow 都能建档执行，不空转。
2. **预置优先 = 幂等**：每轮 drive 决策不会改写已预置计划；跨批计划更新仍走长程语义（档案保留 +
   recheck_prev），meta 产 plan 只决定**建档的首版**。
3. **可观测**：`plan_source` 三态 + reason 标注，审计"计划从哪来"。

## 二、单测（project_drive_test.cpp，新增 5 例）

| 用例 | 断言 |
|---|---|
| `rule_fallback_single_step_plan_without_seed` | 无 steps → 选中；plan_source=fallback、reason 含「自适应单步兜底」、seed 解析为 1 步 s1（goal=周报） |
| `model_plans_for_seedless_task` | 模型输出 plan_steps（2 步 p1/p2）→ plan_source=model、seed 用模型计划 |
| `model_plan_invalid_falls_back_single` | 模型 plan_steps 非数组 → fallback（fail-closed）、seed 单步 |
| `preset_steps_beat_model_plan` | 预置 steps 存在 + 模型给别的 plan_steps → plan_source=preset、seed 仍含 done_weekly |
| `loop_runs_seedless_task_without_starvation` | 无预置 steps 任务经 drive 循环建档+跑完（单步 `unverified` 诚实推进 → done 写回），末轮正常停机非 blocked |

## 三、回归与全仓

- workflow_tests 379 → **384**（+5）；全仓 **896 全绿**（agent 100 / graph 55 / message 20 / model 59 /
  state 43 / tool 226 / webui 9 / workflow 384，零失败零错误）。
- 既有 drive 22 例（调度/经验/依赖）原样通过——有预置 steps 的任务 plan_source=preset，行为零变化。

## 四、遗留（下一条，沿 arch 27 §六）
- **评审 fail 专线重跑路由**（problems/50 §四 + arch 27 §六 剩下的最后一项）。