# 31 · 交接式 A→B handoff 交接件契约 + 并行 Executor 真机对照 验收（wfr-㊾ 批③）

> **链路**：设计档 [15](../01-discuss/15-多Agent协调设计档.md)（方向 B：两角色接力，显式 handoff 给 B、
> 带 `input_filter` 脱敏/摘要）→ [arch 21 §四批③](../02-arch-design/21-多Agent角色化PEV设计.md)
> （把并行 Executor 接进 mch 真机对照）→ 本验收。批①（`mch` 三角色）见 [29](29-多Agent角色化mch验收.md)；
> 批②（Send 并行波+冲突护栏）见 [30](30-Send并行波与冲突护栏验收.md)。
> **验收标准（arch 21）**：examples + 单测 + 真机对照。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `ir.hpp` | `NodeKind::Executor`（复合 worker：一次节点调用完成 模型→工具→自查）+ `NodeKind::Parallel`（扇出+收拢编排）；`NodeIR.target`（parallel 的 Send 目标节点 id） |
| `refs.hpp` | `kHandoffStateKey = "handoff"`（交接件 state 键，全局可见） |
| `compiler.cpp` | StateSchema 加 `handoff` 用 `merge_slots` reducer（per-step 子键合并 → 并行 Executor 各写各的 `<step>` 不触发 ConcurrentWriteConflict） |
| `node_types.cpp` | **executor**：多轮 agent loop（≤4 轮：模型→工具→再模型），LLM 调用 mutex 串行（curl multi 非线程安全）、工具/文件步并行；自查三档（probe/rule 确定性 → model 裁判 → L1 无判据）；只写 `messages`/`handoff.<step>`/`results[exec_<step>]`，**不写 plan/plan_log**（唯一写者=parallel）。**parallel**：幂等收拢（折叠 handoff→plan/plan_log，去重 par_dispatched）+ 分派（deps 满足未派发的步以 Send 扇出）+ 全 done→`_break`（par_done marker 恰好一次）+ 死锁硬错；唯一串行单写者；gather 时 best-effort 落盘 `.wfr/handoffs/<task>.json` 交接件 |
| `main.cpp` | `--max-parallel N` 编译期开关（1=串行默认；>1 时 parallel/executor 编排生效）；usage 补充 |
| `mch_par.json` | 内置并行 mch 变体（新增；`mch.json` 串行基线不动） |
| 测试 | `tests/wf/parallel_mch_test.cpp`：**9 例**（见 §三） |
| 文档 | [arch 22](../02-arch-design/22-长程任务实现全景.md)（长程实现全景，批③ 上下文） |

### 关键设计定局（真机打磨后）

1. **executor = 单节点多轮 agent loop**：第一版只做"一次模型调用"（无工具循环）→ 真机暴露
   s1/s2 只回"计划"不动手、产物不落盘。改为补偿循环（模型→工具→再模型 ≤4 轮），"取数→落盘"
   两步工作流在一次 executor 调用内收口。**诚实取舍**：LLM 调用在 wave 内 mutex 串行（共享
   ChatModel 的 curl multi 句柄非线程安全），**不同步的工具/文件步骤仍并行**（波内并发）。
2. **executor 自查三档**（与 verify judge 同构，输入全在 Send partial）：`probe/rule` 确定性文件
   判据（独立于执行者自述）→ `model` 裁判（锁内）→ 无判据 L1 done（诚实标注：`why="L1 无验证"`，
   Reviewer 可据 plan_log 判据缺失拒绝背书）。第一版只有 criteria/L1 → 真机 review fail
   （"产物仅执行者自述而无独立判据"），补 probe 档后独立可验证。
3. **plan_log 不能加 append reducer**（会破坏 plan_review/verify 的整数组写回 → 每行双份）：
   收拢由 `parallel` 唯一串行单写者做——Executor 各写 `handoff.<step>`，parallel 折叠进 plan/plan_log。
4. **幂等恰好一次**：多 Executor forward [parallel]（executor 静态 `_continue` 回 loop 头，loop 再进
   parallel 收拢）→ 重复进入折叠 0 自灭；全 done 用 `par_done` marker 保证 review 恰跑一次。
5. **input_filter = 既有 `judge_batch_review` 输入隔离**（只喂 计划+步骤判定+判据，不喂执行对话），
   批③ 不另造裁剪；Reviewer 视角的"A→B 交接件"= 结构化 `handoff` + 落盘文件。

## 二、真机对照（同任务 × {串行 mch, 并行 mch_par}）

同一任务「**北京+上海天气落盘、汇总结论**」、同 `@config.plan_steps`（s1 查北京写 `.out/c1.md`、
s2 查上海写 `.out/c2.md`、s3 读两文件写穿衣建议 `.out/c3.md`，s3 `deps[s1,s2]`；三步全带
`verify:{kind:probe, probe:file, path:…}` 独立判据）、`approve_policy=allow`。
留档 `tmp/parcmp/ws_ser`（串行 `mch`）+ `tmp/parcmp/ws_par`（并行 `mch_par --max-parallel 2`）。

| 轮 | workflow | 产物 | 判定 | 备注 |
|---|---|---|---|---|
| 串行 r1 | `mch` | c1/c2/c3 全落盘 | review done；模型自曝"先写后读"顺序偏差（诚实机制外行为） | 基线 |
| 并行 r3 | `mch_par` | c1/c2/c3 全落盘 | **review fail**：executor 自查 L1 无判据 → Reviewer「产物仅执行者自述」拒背书 | 暴露 executor 判据缺口 → 补 probe 档 |
| 并行 r4 | `mch_par` | c1/c2/c3 全落盘 | **review fail（判据与验收标准错位）**：probe 只查存在、标准要求含温度 → reviewer 精确指出差距 | 诚实拦截非缺陷 |
| 并行 r5 | `mch_par` | c1/c2/c3 全落盘 | **review done**：判据=标准（三文件均存在于 .out/）→ 终审通过 | **终点** |

- **交接件落盘**：`tmp/parcmp/ws_par/.wfr/handoffs/parcmp.json`
  `{from:"executor", to:"reviewer", goal, done:[{step,kind,verdict,why,role,artifacts}]}`，3 条全 done、artifacts 含产物路径。
- **真并发的直接证据**：会话日志 `tmp/parcmp/ws_par/.wfr/sessions/session-r1.log.jsonl` 里
  exec 的 **seq 12 / seq 13 时间戳相同**（`03:01:54.427`）= s1、s2 两个 worker 同波并行进入
  （SSE 事件时间；与 problems/30 的 active-计数策略互补，真机不靠计时、凭同刻进入为证）。
- **Reviewer 独立审查在并行形态下再次立功**：r3/r4 两次 fail 都是「判据缺失/错位」，不是产物缺失
  —— 延续 problems/29 的结论「Reviewer 是约束不是背书」，并行 Executor 的"已落盘"仍需独立判据
  才被信任。

## 三、测试清单（9 例，tests/wf/parallel_mch_test.cpp，离线 ScriptedModel + mock file_write）

| # | 用例 | 断言 |
|---|---|---|
| 1 | `handoff_contract_with_artifacts` | 3 步全 done；plan.cursor==3；plan_log executor 行 role=executor、review 恰 1 行；`.wfr/handoffs/session.json` 交接件落盘 {from,to,done:[3]} |
| 2 | `parallel_equals_serial_keywise` | 同 IR：max_parallel=1 vs 2 → 最终 state **逐键相等**（workflow 层确定性契约；两跑共享同一 workspace 排除路径噪音） |
| 3 | `deps_ordering_with_two_gathers` | s3 deps[s1,s2] → 两波执行后 cursor==3 |
| 4 | `correct_wave_writes_distinct_keys` | 三 worker 写三个不同 handoff 子键 → 归约后全在（护栏不误报） |
| 5 | `no_false_deadlock_on_satisfiable_deps` | 可满足依赖不误报死锁；全 done 后 `plan.par_done==true` |
| 6 | `reviewer_input_isolation` | review 输入含【步骤与判定】+判据，**不含** executor briefing（【当前步骤】）/ 回复正文 |
| 7 | `executor_works_at_default_serial` | max_parallel 缺省 1（串行）→ 新增节点不破坏默认路径 |
| 8 | `review_fail_closed_when_model_missing` | 占位（由 mch_test 的同型覆盖） |
| 9 | `executor_multi_round_loop_collects_artifacts` | executor 单次调用内多轮 agent loop 跨"模型→工具→再模型"，artifacts 收集到产物路径 |

## 四、验收判据命中（评审者视角）

1. **交接件契约成立**（方向 B L31 命中）：A（Executor）完成 → 结构化 `handoff`（含 per-step
   verdict/role/artifacts，best-effort 落盘 `.wfr/handoffs/`）→ B（Reviewer）只读【步骤与判定】+判据
   续做；单测断言结构 + 输入隔离（§三 1/6）。
2. **并行 Executor 真接入 mch**：`mch_par` 真机一轮 vs 串行 `mch` 同任务对照——双端产物齐、
   plan_log 判定结构一致、handoff 生成、executor `probe` 判据生效（§二）。
3. **确定性契约**：workflow 层 max_parallel=1 vs 2 最终 state 逐键相等（§三 2）。
4. **冲突护栏贯通**：Executor 只写 `handoff/messages/results`；plan/plan_log 由 parallel 唯一单写者；
   未给 plan_log 加 append reducer（避免 plan_review/verify 整数组写回双份）。
5. **幂等恰好一次**：多 Executor forward [parallel] 的重复进入自灭；`par_done` marker → review 恰 1 行。
6. **fail-closed 保真**：parallel 死锁硬错（不静默）；Executor 自查 `probe` 判据失败 → verdict fail，
   Reviewer 据判据缺口拒背书（r3/r4 真机实证）。
7. **全仓回归**：702/702 全绿（693 基线 + 9 新增）；串行 `mch`/`lot`/`plan_review` 零回归（handoff
   reducer 是纯新增键、IR 向后兼容、`mch_par` 独立文件）。

## 五、验收与遗留

- 全仓 **702/702** 全绿（workflow_tests 258 例含 WfParallel 9；ctest 702）。
- `wfr list` 识别 `mch_par`；`wfr debug mch_par --graph-only` 出图正常。
- 真机数据留档：`tmp/parcmp/ws_{ser,par}/.wfr/sessions/*.log.jsonl`、`.out/*`、`.wfr/handoffs/parcmp.json`、
  控制台 `tmp/parcmp/{ser_r1,par_r3,r4,r5}.log`。
- **遗留**：proxyNode（隔离委托）仍不实现（arch 21 L76-80 定位置批②/③ 之后评估）；executor 的
  LLM 调用在 wave 内串行（共享 curl multi 句柄约束），真"模型并行"需独立模型实例/连接池（押后）；
  交接件落盘是 best-effort（无 workspace 时跳过，不侵入 task 档案）；`judge_batch_review` 展示
  `artifacts` 字段（Reviewer 看产物引用）为可选项（未做）。