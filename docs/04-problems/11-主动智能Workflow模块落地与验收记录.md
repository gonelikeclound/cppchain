# 11 · 主动智能 Workflow 模块落地与验收记录（`cppchain_workflow`）

> 规范来源：仓库根 `主动智能/11-ir-syntax.md`（IR 语法）与 `12-generation-process.md`（生成过程）。
> 本文件是 cppchain 侧落地记录的验收结论：**离线单测全绿、零告警**
> （M11 写作时 40/40、全仓 323/323；后续复验与判据修订见 §5）。

## 1. 落地了什么

新增一个 leaf 模块 `cppchain_workflow`（`src/workflow/`，依赖 graph/tool/model/state），
把「意图 → IR → 编译 → 触发执行 → 验收」整条链搬进 C++26：

| 文件 | 职责 |
|---|---|
| `workflow/ir.hpp/.cpp` | IR 数据模型 + JSON 解析 + 节点类型判定（§3 优先级，歧义即错） |
| `workflow/refs.hpp/.cpp` | `$_`/`$<id>.result.<f>`/`@config.<k>` 解析、模板替换、状态布局 |
| `workflow/expr.hpp/.cpp` | 受限表达式（§6，递归下降；无函数/无数组）+ 宽松求值（D3） |
| `workflow/validator.hpp/.cpp` | 结构/触发/节点/引用/图结构全量校验（V1–VN/VT/VR/VH/VG） |
| `workflow/compiler.hpp/.cpp` | IR → `StateGraph` 编译、label 消解、goto 有界回跳、`fire_once`/`simulate_fires` |
| `examples/support/fakes.hpp` | 假工具工厂（D4）：10 个目录工具，行为从 `FakeToolSinks` 可注入（纯 header，仅 demo/评测用，不进库） |
| `workflow/generator.hpp/.cpp` | 生成端：PlanStage 三态 → IRStage 一次性 + 修理 → 澄清会话续跑 |
| `examples/support/features.hpp` | 评测特征谓词 + 12 例固件（纯 header，仅 demo/评测用，不进库；转写 10-eval-cases.md） |

## 2. 关键决策（D1–D4 + 一处修正）

- **D1 触发仅为元数据**：trigger 只做解析/校验（VT1–VT7：仅 `time`+`schedule`、
  5 字段 cron、禁 `condition`/`filter`），不做 OS 调度；触发 = `fire_once` 手动/模拟，
  每次全新状态（`examples/wf_eval` 有 3 连发的调度模拟段）。
- **D2 编译期整体求解 label**：不要求 label 先于 goto。全图建边（`next`/`then`/`else`
  前向 + `goto` 回边）后，每个 label 的续接 = 数组中其后第一个非 label 节点；
  所有指向 label 的指针替换为续接，label 不注册成图节点。goto 用 `state.loop_counts`
  计数，超过 `max` 跳终止；`max` 界定循环次数，引擎 `max_steps` 兜底。
- **D3 宽松 expr**：`numberish` 先数字后"可解析为 double 的字符串"；`==`/`!=`/`<` 等
  对 `null` 安全、不可比即 `false`；`contains`=数组成员 eq / 字符串子串，`in` 对称。
- **D4 假工具可注入**：`fetch_price`/`web_search`/`http_get`/`llm_extract`/`llm` 的行为
  从 `FakeToolSinks` 取（未注入给默认罐头/离线 stub）；`send_notification` 追加到
  `sinks.notifications`（断言"发了几次"）；OS 级工具（`query_content` 等）**故意不注册**，
  validator 经 `VN6` 自动拒绝 → 生成器提示词里的工具目录 == 注册表，白名单天然一致。
- **修正 `_last_exec` 语义**：只有 tool 节点更新 `_last_exec`（它是"最近产出输出的节点"），
  expr/goto/label 不碰 —— 否则 `$_.result.price != null` 这类规范写法会把 expr 指向
  自己的空槽。

## 3. 度量与验收

**离线（`tests/wf/`，`workflow_tests`，40 例）**
- expr：宽松比较、`contains`/`in`、truthiness、parse 错误族（函数/数组/悬空 `&&`/缺字段）。
- refs：整值引用保留 JSON 类型、模板替换、损坏片段检测（`@config` 前缀 bug 在首跑即被测试抓出并修复）。
- IR/validator：`minimal_price_ir` 与 §9 真实产物正向通过；VT3/VT5（6 字段/`@`/`L` 等 bad cron）、
  VN1/VN2/VN6、VR1/VR2、VG1/VG2 自环/VG7 死节点/VG8 死 label — 全部按编号拒绝；
  label 消解两个正向形态（循环头 / 汇合目标）通过。
- compiler：线性价格流（price<800 通知 / ≥800 静默、`$_` `@config` 生效）、轮询 `max=4`
  （恰 4 次循环、无通知）、`retry` 中成功、`fallback` 走降级、无 fallback 运行时 fail-closed、
  坏 IR 编译期拒绝、`simulate_fires` 多次触发状态独立。
- generator（`ScriptedChatModel` 纯离线）：plan→IR 生成、clarify/reject 三态分派、
  澄清会话 + `submit_answers` id 校验（错答报错、对答续跑）、plan 重试耗尽、IR 修理一轮、
  预算耗尽快照拒绝。

**在线（`examples/wf_eval`，真机 DeepSeek，`./run-demo.sh -n wf_eval`）**
对 12 例跑完整管线并打矩阵：golden 要求 Generated + 特征全中 + 可编译可触发；
boundary 要求 Reject 理由非空 / Clarify 追问命中 `must_ask` 关键词；
末尾补 3 连发调度模拟（通知逐次新增、loop_counts 归零）。

## 4. 明确的边界与后续留意

- **通知诚实性不可静态证明**：validator 不查"声称已下单"；它只在生成器提示词里要求、
  由 `examples/support/features.hpp` 的 `声称已下单` 谓词在评测层审计。
- **`$_` 的字段声明检查按数组上游做**（VR3）：对汇合/循环内的运行序是保守近似，fail-closed。
- **跨触发防重放**（同一副作用通知不得重复）未进 v1：`sinks.notifications` 逐次累计、
  由测试断言计数；与 `agent::SideEffectGuard/journal` 的对接留作后续。
- **clarify 会话存 `Store` 的 `wf/session` 命名空间**：现为进程内 MemoryStore，
  换持久后端即是换 `Store` 实现。

## 5. 复验：判据修订、A/B 方法与"n<10 不作判据"

### 5.1 判据修订（依据 IR 的实际表达能力，不看模型行为）

改期望值有"教模型应试"的风险，故每条都**先证伪原判据**再动：

- **`wf_expressiveness_gap`：Reject → Generated（移入 golden）**。原判据"结果为非空属表达力
  缺口"不成立 —— IR 规范 §6 明确给出 `!= null` / `!= ""` 两个惯用法，实测 `!= ""` 能正确区分
  有内容与空串。**能力存在，用例在考一个假命题。**
  新增 `非空判断` 特征谓词（归一化空白后精确匹配两个惯用法；`price != 800` 这类纯业务比较
  不算），让这条 golden 真的断言到东西，而不是"生成了点什么"。
- **`wf_llm_extract_summary` → `wf_count_gap`**：它与 golden 的 `wf_llm_extract_notify` 几乎
  逐字相同，只差一个**可表达**的"非空"条件 —— 判据站不住。改为真缺口：需统计集合大小，
  而 IR 的 expr 只有标量比较（无函数 / 无数组 / 无 `len()`），`web_search` 返回整页文档
  也无法可靠计数。
- 边界用例 5→4：一条判据错误、一条改换缺口，**没有新增凑数用例**。
- 根因修正：`12-generation-process.md` 的判据原把"集合非空"列为表达力缺口（与
  `11-ir-syntax.md` §6 直接冲突）。现写明真缺口只有三类：**触发语义**（事件唤醒）、
  **集合大小/计数**、**算术与跨触发状态**；并明确列出"不构成缺口"的业务条件。

### 5.2 A/B：提示词里自相矛盾的两条规则

同一条 `system_prompt` 相隔 5 行互相打架 —— 前句说"结果非空"要下沉为 expr 判断节点，
后句说"结果为非空"是表达力缺口必须 reject。已修：删掉冲突例子、"允许降级"→"应当降级"。

A/B 设计：两组**只差那两处提示词**（`git diff` 2 增 6 删，其余代码完全相同），各 5 轮，同一端点。

| | golden | boundary |
|---|---|---|
| 旧提示词 | 27/40 (68%) | 10/20 (50%) |
| 新提示词 | **31/40 (78%)** | 9/20 (45%) |

**结论：+10pp 落在噪声内，不显著**（两比例检验 z≈1.0、p≈0.32；单用例
`wf_dual_source_fallback` 2/5 vs 4/5 的 Fisher p≈0.52）。要在此效应量上达到显著，
每组需上百轮。故**提示词保持新版**（依据是"消除自相矛盾"这一客观事实，不依赖本实验），
但**不能声称它提高了通过率**。

### 5.3 方法论结论：n<10 的矩阵读数不具备判据效力

同一组调优过程中，数据在 n=1、n=3、n=5 上给出过**互相矛盾**的结论：

| 观察量 | 小样本（n=1/3）的结论 | n=5 的结论 |
|---|---|---|
| `wf_manual_order` 是否能降级 | "措辞修正有效（1/3 降级）" | 0/5，**无效** |
| `wf_bad_trigger_cond` | "被过度纠正了（✗✓✗）" | 4/5，**未过度纠正** |
| golden 总分 | "5/7，未提高" | 6/8，方向性 +10pp（不显著） |

**纪律**：单次运行不得用于判定提示词或模型的优劣；≥5 轮也只足以识别**稳定失败**，
要判"改善/劣化"需要显著更大的样本。分不清时，**优先看失败形态**（错误是否可读、
是否走 on_error、是否被记成 `Failed`）而不是通过率。

### 5.4 跨组稳定失败（10 轮全败，是真信号）

| 用例 | 现象 | 定性 |
|---|---|---|
| `wf_manual_order` | 模型拒绝，且理由**自陈**"按规则应降级而非拒绝" | 规则读懂了但不照做 → 模型行为，非措辞问题 |
| `wf_count_gap` | 生成而非拒绝 | 真缺口（IR 算不了集合大小），模型不该生成 |
| `wf_ambiguous_morning` | 从不澄清"早上" | 澄清触发不足 |

### 5.5 复验期间同时修掉的缺陷

- **工具 handler 抛异常时 `on_error.retry/fallback` 被整体绕过**：`compiler.cpp` 直接调
  `spec->handler`，异常穿透节点，重试与降级根本没机会执行（IR 明写 `fallback` 也照样崩）。
  现与 `ToolNode::execute_one` 的纪律对齐：异常与 `unexpected` 同等对待。真机效果
  golden 1/7 → 4/7。
- **假工具取参写法**：`fakes.hpp` 一律用 `args.value(key, default)` —— 键存在但值为 `null`
  或类型不符时 nlohmann **抛异常**（真机表现为 `type must be string, but is null / but is
  array`，跨多个用例反复出现）。而 `null` 是合法状态（refs 契约：目标槽缺失 → json null）。
  真实工具全部用 `tool::arg<T>`，假工具是唯一异类。现统一改齐，异常变成可读、可回喂的
  `ToolError` 并正常汇入 on_error 路径。
- **验收矩阵的"假通过"三连**：`wf_eval` 退出码恒为 0（0/7 全灭也报"通过"）；生成失败
  （模型调用失败/预算耗尽）被映射成 `Rejected`，与"模型明确拒绝"不可分 —— 全 TLS 失败
  的一轮里 4 个边界用例照样打 ✓；`model_calls_budget` 是**对象生命周期**预算而非单次生成
  预算，复用同一个 generator 跑 12 例时前 5 例就把预算吃光。三者已修（退出码 /
  `Kind::Failed` / 入口重置计数）。