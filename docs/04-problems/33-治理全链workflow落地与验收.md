# 33 · 治理全链 workflow（governed）落地与验收（problems/33）

> **链路**：外部《Agent 执行治理总纲》（[discuss 18](../01-discuss/18-Agent执行治理总纲.md)）→ 评估（[problems/32](32-副作用治理对照总纲评估与规划.md)）→
> 设计（[arch 23](../02-arch-design/23-治理全链workflow设计.md)）→ 本验收。
> 目标：新建 `governed` workflow 完整实现治理思想（批准→执行→账本→验证→终态四档）。

## 一、落地（改了哪里）

| 机制 | 内容 |
|---|---|
| **UNVERIFIED 双档** | `node_types.cpp` c_verify judge：无判据 → `{verdict:"unverified"}` + `steps[].status="unverified"`；并行 Executor L1/收拢/cursor 同步；judge results 加 `verdict/outcome`；reviewer 提示加纪律；done_summary 计入 unverified |
| **_timeout 档** | `ir.hpp` 加 kTimeoutName；`compiler.cpp` resolve + `__timeout__` 落地节点（不抛异常，写 plan.terminal="timeout"）；validator VG1/VN2 白名单；debug_view isTerm（C++/JS） |
| **_cancelled 档** | `ir.hpp` kCancelledName；compiler `__cancelled__` 落地节点；`c_plan_review` abort → cancelled（统一取消语义，取代 aborted）；`main.cpp` EOF/quit → abort 续跑落档 |
| **effect_class + 自检** | `tool.hpp` EffectClass 枚举（read/idempotent/irreversible）+ with_effect_class；`registry.hpp` effect_class_violations；file_write→Idempotent、bash→Irreversible、submit_result→Idempotent；`wfr/main.cpp` 注册后自检 fail-fast |
| **governed.json** | `share/wfr/workflows/governed.json` 新治理 workflow（零新节点类型，全现成） |
| 测试 | `tests/wf/governance_test.cpp` 6 例 + `tests/tool/effect_class_test.cpp` 3 例 |

## 二、验收判据（评审者视角）

1. **终态四档穷尽**（总纲 G7）：完成(_turn_end) / 失败(_fail, jgate+gate) / 超时(_timeout, on_max) / 取消(_cancelled, plan_review abort)——四档互斥且穷尽（governance_test 6/6）。
2. **UNVERIFIED 诚实性**（总纲 G5）：无判据步 = unverified（不是静默 done），cursor 照推，Reviewer 批级兜底问责（提示含纪律）。
3. **G4 自检 fail-fast**：非只读工具缺 effect_class 声明 → 启动即失败（effect_class_test 3/3 + wfr 启动验证）。
4. **零回归**：on_max 缺省不动（既有 loop 语义不变）；plan_review 取消语义统一（爆炸半径 pev_review + 1 测试）；l1 教学档语义跟随 unverified。
5. **全仓 720 全绿**（governance 6 + effect_class 3 + 既有断言更新）。

## 三、测试清单

| 测试 | 断言 |
|---|---|
| `WfGoverned.unverified_step_marks_plan_and_advances` | s1 probe done + s2 unverified + cursor=2 + plan_log verdict=unverified |
| `WfGoverned.judge_fail_routes_to_fail_terminal` | probe 失败 → jgate → _fail（fire 报错） |
| `WfGoverned.on_max_timeout_records_terminal` | max=1 不够跑 → plan.terminal=timeout + plan_log terminal 行 |
| `WfGoverned.plan_review_abort_lands_cancelled_terminal` | abort → plan.cancelled + terminal=cancelled + plan_log cancel |
| `WfGoverned.happy_path_full_chain` | 全链跑通：无 terminal 档 + 档案落盘 |
| `WfGoverned.review_fail_routes_to_fail` | review fail → gate → _fail |
| `EffectClass.non_read_without_declaration_is_violation` | 未声明违规 |
| `EffectClass.declared_and_read_tools_are_clean` | 声明/只读干净 |
| `EffectClass.vocab_closed` | 词表封闭 |

## 四、遗留与押后
- irreversible 自动升审批/进 HITL payload（P1）；approve 边界 EOF 取消落档（P1）；_timeout/_cancelled 写任务档案终态（P1）；effect_class 自检覆盖 ⑤ 动态插件（文档注明复调 helper）；governed 真机对照（同任务带判据 vs 不带 + abort 一趟）待 LLM 环境跑。

## 五、真机对照（2026-10-21，同 governed workflow × 三趟）

> 同一 `governed` workflow，不同 `@config.plan_steps` 注入；`plan_confirm=false`（abort 趟除外）+ `approve_policy=allow`。
> 工作区留档 `tmp/govcmp/ws_{judged,unv,abort}`；`export_lot_data.py` 可导出 verdicts 与 terminal。

| 趟 | plan_steps | 步骤 verdict | 批级 review | 终态 |
|---|---|---|---|---|
| **带判据** | s1/s2 都带 `verify:{probe}` | `done / done`（probe 命中，产物落盘） | done | 完成档（无 terminal） |
| **不带判据** | s1/s2 无 verify | `unverified / unverified`（诚实标注） | **fail**（"均未提供可独立核对的产物"）→ gate → `_fail` | 失败档 |
| **abort** | s1/s2 无 verify + 确认门打开 | —（第一步悬挂） | — | **`plan.cancelled=True + terminal=cancelled`** + plan_log `{kind:cancel}`/`{kind:terminal}` |

**治理思想真机兑现**（对照总纲 G5 / G7）：
1. **无判据步诚实标 unverified**（不再静默 done）——带判据 vs 不带判据 verdict 差异清晰：`done/done` vs `unverified/unverified`。
2. **Reviewer 拒绝凭自述放行**——不带判据趟 review 判 fail（"无法独立核对产物"）→ `_fail`，正是"ReAct+ANSWER 的 L4 真空由独立复核把守"。
3. **取消档成档**——stdin `abort` → `plan.cancelled + terminal=cancelled` + plan_log 双行（修复旧"取消不成档"缺口：EOF/quit 不再 return 0 悬挂残留）。

**已知边界**：`approve_policy=allow` 会把 plan_review 确认门 mock 成 continue（HITL 与审批都吃该策略）——abort 趟需 `approve_policy` 缺省（prompt）才能人工取消；这是 mock 策略的既有语义。
