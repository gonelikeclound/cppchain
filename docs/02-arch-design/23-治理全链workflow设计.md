# 23 · 治理全链 workflow 设计（governed，problems/33）

> **前文**：《Agent 执行治理总纲》（[discuss 18](../01-discuss/18-Agent执行治理总纲.md) 吸收）+ 评估（[problems/32](../04-problems/32-副作用治理对照总纲评估与规划.md)）→ 本设计定稿。
> 用户确认方向：**单 workflow 自洽**（非横切层）+ 接受两项引擎改动 + `_cancelled` 完整落（引擎+宿主）。

## 一、目标

新建 `governed` workflow，把治理思想（总纲 L3 保障 + L4 事实 + L5 终态四档）**在一条可运行的 workflow 里自洽地走完整链**：
批准（G3）→ 执行（G4，账本三态防重放）→ 验证（UNVERIFIED 双档）→ 批级终审（Reviewer）→ 终态四档（完成/失败/超时/取消）。

## 二、四个机制定论

### 2.1 UNVERIFIED 双档（总纲 G5 · ReAct+ANSWER 诚实标未验证）
- **语义**：无 verify 判据的步骤 = 完成但未验证 → `plan_log.verdict="unverified"` + `steps[].status="unverified"`；**cursor 照推**（否则死锁）。
- 引擎改动（judge results 加 `verdict`/`outcome`；outcome 含 `auto_retry` 区分 rollback_deps）。
- 批级兜底：Reviewer 提示加纪律"unverified 必须独立核对或明确放行理由"；`done_summary` 计入 unverified（HITL 人闸可见）。
- 并行 Executor L1 同步（自查 unverified + 收拢三档 + cursor 前移）。

### 2.2 `_timeout`（loop max 触达 · 超时档）
- **保缺省 on_max→_turn_end 不动**（零回归地基）；`_timeout` 显式声明。
- 合成落地节点 `__timeout__`（镜像 `__fail__` 但**不抛异常**）：写 `plan.terminal="timeout"` + plan_log `{kind:terminal}` → goto `__end__`。

### 2.3 `_cancelled`（用户中断 · 取消档）
- **统一取消语义，取代 plan_review 的 aborted**：abort 动作 → 步标 `cancelled` + `plan.cancelled=true` + plan_log `{kind:cancel}` + goto `_cancelled` 终端（不再跳 loop 出口冒充完成）。
- 宿主补齐：main.cpp EOF/quit/abort → `resume={action:abort}` 续跑（不再 return 0 悬挂残留）。
- 爆炸半径 = pev_review.json + plan_review_test（已同步改）。

### 2.4 effect_class + 启动自检（总纲 G4 · 直调包装层单点）
- `ToolSpec.effect_class`：Read（只读，缺省）/ Idempotent（可安全重放）/ Irreversible（不可安全重放）+ `with_effect_class` setter + `kEffectClassNames` 词表。
- **自检规则**：`read_only==false` 必须显式声明 Idempotent/Irreversible，未声明（=Read）即违规 → wfr 启动 fail-fast（G4：把运行期静默遗漏提前到启动）。
- 标注：file_write→Idempotent、bash→Irreversible、submit_result→Idempotent。
- effect_class ≠ CommandClass（声明静态 vs 词面动态）；消费边界=声明+自检+审计（approve 决策零变化，irreversible 自动人审列 P1）。

## 三、governed workflow 结构

```
recall(load_history) → task(begin) → plan(Planner)
→ loop{max:6, on_max:_timeout}
    plan_review(hitl) → verify.announce(step) → model(think, chat_bash)
    → route(tool_calls→approve / text→judge / invalid→_fail)
    → approve_tools → run_tools → verify.judge(executor)
    → expr jgate($judge.result.outcome=="fail" → _fail / _continue)
→ verify.review(Reviewer 独立终审) → expr gate($review.result.verdict=="fail" → _fail / final)
→ model(final 解说) → task(end) → _turn_end
```

- **全部现成节点**（零新 node 类型）：plan/verify(三相位)/plan_review/approve_tools/run_tools/model/route/expr/task/loop。
- 演示：s1 带 probe 判据（→done）、s2 无判据（→unverified）。
- **环路**：完成=announce `_break`→review；失败=jgate/gate→_fail；超时=on_max→_timeout；取消=plan_review abort→_cancelled；全过→_turn_end —— 终态四档穷尽（总纲 G7）。

## 四、引擎改动清单（两项，已确认接受）

| 改动 | 位置 | 目的 |
|---|---|---|
| judge results 加 `verdict`/`outcome` | node_types.cpp c_verify judge | expr 门（jgate）可引用 |
| VR2 result_ok 白名单加 Verify/PlanReview | validator.cpp:596 | expr 门引用 $judge/$review.result |

## 五、验收

- 单测 9 例（governance 6 + effect_class 3）+ 既有断言更新（l1 unverified、plan_review cancelled、debug_view/loop/compiler 追加）。
- 全仓 720 全绿；governed 真机/离线跑通（problems/33）。
- 对照：同任务"带判据 vs 不带判据"verdict 差异 + abort 一趟 + export_lot_data 观察 unverified 占比与 terminal。

## 六、遗留（押后）
- irreversible 自动升审批/进 HITL payload（P1）；approve 边界的 EOF 取消落档（P1）；_timeout/_cancelled 写任务档案终态（P1）；effect_class 自检覆盖 ⑤ 动态插件（文档注明需复调 helper）。
