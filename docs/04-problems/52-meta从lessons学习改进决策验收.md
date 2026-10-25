# 52 · meta 从 lessons 学习改进决策（arch 27 第二刀）验收

> **链路**：problems/49/51 遗留「meta 从 lessons 学习改进决策」→ [arch 27](../02-arch-design/27-自管理loop调度循环设计.md) §五 → 本验收。
> **验收标准（arch 27 §五）**：经验回流进 project_drive 决策的闭环——读得进（相关性挑选）、用得上
> （规则档加权 / 模型档简报）、可审计（[lesson N] 引用锚点 + lessons_used）；5 新单测；全仓全绿。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `node_types.cpp` `c_project_drive` | **经验读入 + 相关性挑选**：`lessons.json` → `applicable_if` 命中（空 = 通用总相关；非空 = 与任务 id/name **双向子串匹配**）；命中未完成任务 → `pref_tasks`，相关条目 → `related`（编号 [lesson N]） |
| 同上 · 简报 | 新增【项目经验（lessons，仅相关；决策请参考，引用时标注 [lesson N]）】段（模型档依据），只列相关条目 |
| 同上 · 规则档 | **经验加权**：候选未完成排序 = `pref_tasks`（applicable_if 命中的未完成）在前、其余按文件序；已 done 任务不参与加权 |
| 同上 · 模型档 | 缺省 meta system_prompt 加"参考```【项目经验】```，reason 引用时以 [lesson N] 标注" |
| 同上 · 结果 | `lessons_used` = 相关经验条数（可观测） |
| 测试 | `project_drive_test.cpp` +5（见 §二）；`examples/project_drive` 加一条 applicable_if=weekly 的经验并打印 lessons_used |

### 关键语义

1. **"学习"的判定边界**：经验只**影响**决策（排序/简报/提示要求），不保证模型必然套用——"学习"定义为
   「经验进得了上下文、用得上（引用锚点）、可审计」三段可观测；模型是否真套用留给调度案例实测。
2. **可审计回流链**：任务完结 `state.lesson` → lessons.json（第一刀）→ drive `related` 编号 `[lesson N]` →
   模型 reason 标注 `[lesson N]` → 决策可追溯到经验条目。无凭空引用（提示词要求「不要凭空引用不存在的经验」）。
3. **不打破既有行为**：无 lessons / 无关 lessons → 行为与上一版完全一致（reason 仍「顺序推进」、lessons_used=0）。

## 二、单测（project_drive_test.cpp，新增 5 例）

| 用例 | 断言 |
|---|---|
| `rule_lesson_weighting_reorders` | 文件序 monthly 在前 + 一条 applicable_if=weekly → 规则档先做 weekly、reason 含「经验优先」、lessons_used=1 |
| `rule_generic_lesson_reported_without_reorder` | 通用经验（applicable_if 空）= 总相关（lessons_used=1）但不改变文件序 |
| `rule_ignores_unrelated_lessons` | applicable_if=“洋务运动” → 过滤（lessons_used=0）、选择不受影响 |
| `rule_lesson_weight_skips_done` | weekly 已 done → 经验不插手，规则选第一个未完成（monthly） |
| `model_brief_includes_related_lessons` | 模型档：简报含【项目经验】段且只含相关条目（不含无关）、lessons_used=1；模型 reason 可带 `[lesson 1]`（审计锚点） |

## 三、回归与全仓

- workflow_tests 368 → **373**（+5）；全仓 **885 全绿**（agent 100 / graph 55 / message 20 / model 59 /
  state 43 / tool 226 / webui 9 / workflow 373，零失败零错误）。
- 既有 drive 6 例原样通过（无经验时行为零变化）；整个 project 系列（project/review/drive 34 例）全绿。

## 四、遗留（下一条）

- **模型是否真套用量化**：真机调度案例（带经验的真实项目）实测模型决策引用经验的比例——离线只能验证
  "上下文有 + 可引用 + 可审计"。
- 任务依赖图调度、自适应 plan_steps、评审 fail 专线重跑路由（arch 27 §六）仍押后。