# 11 · 主动智能 Workflow 模块落地与验收记录（`cppchain_workflow`）

> 规范来源：仓库根 `主动智能/11-ir-syntax.md`（IR 语法）与 `12-generation-process.md`（生成过程）。
> 本文件是 cppchain 侧落地记录的验收结论：**离线单测 40/40 全绿（全仓 323/323）、零告警**。

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