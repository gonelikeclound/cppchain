# 55 · 评审 fail 专线重跑路由（arch 27 第五刀）验收

> **链路**：problems/50 §四 / arch 27 §六（押后最后一项）→ §九 第五刀 → 本验收。
> **验收标准（arch 27 §九）**：终审 fail 点名 rework → completion.rework 落档 → project_drive 返工模式
> 重跑清单任务（含已 done）→ 再评审闭环；5 新单测；全仓全绿。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `node_types.cpp` `judge_project_review` | 新出参 `rework_out`：模型判 fail 时可输出可选 `rework:[任务id]`；只收存在于 project.tasks 的 id（未知忽略 fail-closed）；review 缺省 prompt 说明"判 fail 时如能指出返工任务就列出" |
| `c_verify`（review 项目模式） | 收集 rework → `state.project_review.rework` |
| `c_project`（end） | `completion.rework`（verdict=fail 且有点名时）落档 |
| `c_project_drive` | 读 `completion` → 返工模式（verdict=fail && rework 非空）：候选 = rework 清单任务（**即使 progress=done 也重跑**，依赖仍须就绪），reason「项目终审 fail → 返工重跑」；返工候选无一就绪 → 阻塞停机；简报【项目终审】状态 + prompt 优先返工清单（模型选非清单 → 回落）；结果 `rework` 计数 |
| 测试 | `project_drive_test.cpp` +5（见 §二） |

### 关键语义

1. **返工 ≠ 重跑未完成**：普通模式重跑"未完成"；返工模式重跑"评审点名的任务"——即使标了 done
   （整体不达标可能因已标完成的任务不合格）。
2. **闭环靠子 workflow 的再评审**：每任务跑完 → 项目终审重判 completion → done 退出返工、fail 继续点名；
   loop max 兜底防病态。
3. **fail-closed**：rework 里的未知任务 id 被忽略；模型选非清单任务 → 回落规则档；返工候选无一就绪 → 阻塞停机。

## 二、单测（project_drive_test.cpp，新增 5 例）

| 用例 | 断言 |
|---|---|
| `review_fail_writes_rework_to_completion` | 模型判 fail+rework[weekly,monthly] → project end 写 `completion={verdict:fail, rework:[weekly,monthly]}` |
| `drive_reroutes_rework_task_even_if_done` | completion.fail + rework[weekly] + progress weekly=done → drive 仍选 weekly（返工重跑）、reason 含「返工重跑」、rework=1 |
| `drive_rework_respects_dependency_ready` | rework[weekly,review]，weekly 依赖 monthly 未 done → 选就绪的 review（返工候选仍受依赖门） |
| `drive_rework_blocked_when_none_ready` | rework[weekly]，weekly 依赖 ghost → blocked=true、done=false |
| `loop_rework_resolves_after_rerun` | 闭环：返工重跑 weekly → 子 workflow 内项目终审判 done → completion 变 done → 末轮正常停机 |

## 三、回归与全仓

- workflow_tests 384 → **389**（+5）；全仓 **901 全绿**（agent 100 / graph 55 / message 20 / model 59 /
  state 43 / tool 226 / webui 9 / workflow 389，零失败零错误）。
- 既有 27 例 drive + project/review 全部原样通过——无 completion.fail/rework 时返工模式不激活，行为零变化。

## 四、自管理 loop 收官（arch 27 §十）

problems/51–55 六刀全景：调度循环（880）→ 经验回流（885）→ 任务依赖图（891）→ 自适应 plan_steps（896）
→ **评审 fail 专线重跑（901）**。arch 27 押后清单清空。下一阶段增量（真机量化、cron 触发接入、依赖图
失败专线等）另行立项。