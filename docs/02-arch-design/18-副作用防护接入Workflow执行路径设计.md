# 18 · 副作用防护接入 Workflow 执行路径（wfr-㊷）—— 审批节点 + 执行点防重放 + probe 探测

> 专题：**副作用防护进 wfr（Workflow 图通道）**。承接 [20-副作用全景分析](../04-problems/20-副作用全景分析.md)。
> 全景指出 `file_write` 挂不进 wfr loop 的根因是——**Workflow 图的工具执行不经过任何中间件**。
>
> 本篇是**二版**（吸收架构讨论后的折中）：审批做成**独立节点**（`approve_tools`）；
> 防重放/幂等**留在执行点内部**、以**会话日志为事实源**；崩溃缺环由 **probe 探测**
> （工具自查外部产物）兜底；副作用记录**以日志为准**。

## 1. 定位与边界

- **在做什么**：把副作用防护拆成三层接进 Workflow——**审批**（决策层，成节点）、
  **防重放/幂等**（执行层，留执行点）、**probe 探测**（崩溃兜底层，工具自描述）。
- **不在做什么**：不搬 agent 的 `Middleware` 链进 workflow；不改工具实现（除 `ToolSpec` 加一个
  probe 描述字段）；不做交互式审批（`approve_tools` 接 M10 `interrupt` 是后续迭代）；
  不动沙箱纪律（`resolve_path_in_root` 三连挡不变）。

## 2. 断层回顾（为什么 `file_write` 进不去）

| 通道 | 工具执行 | 审批 | 防重放 | 持久化 | 审计 |
|---|---|---|---|---|---|
| `Agent` 循环（库/examples） | `approve → lookup → node.run → after` 中间件链 | ✅ | ✅ | ✅ | ❌ |
| Workflow 图（wfr 命令行） | `c_tool` 直跑 handler / `c_run_tools` 跑 `ToolNode::run` | ❌ | ❌ | ❌ | ❌ |

`src/wfr/main.cpp` 注释原话：*"`file_write` 是副作用（read_only=false）且 wfr loop 没有审批中间件 → 不默认挂。"*

## 3. 目标与非目标

**目标**：
1. 审批（policy 判定）成为**图上可见的节点**，deny 可 route 分流、可接 `interrupt`。
2. 防重放/幂等**在工具执行点内部**生效，崩溃恢复绕不过它；以**会话日志**（唯一事实源）判断状态。
3. `ToolSpec` 增 `probe` 字段：工具自描述"如何检查自己的外部产物"，崩溃 `Intent` 残留时兜底判定。
4. `file_write` 挂进 wfr `chat` 工具集；`file_read`/`list_dir` 只读仍免审、不记账。
5. 审计最小可用：决策 + 探测留痕，决策细节以 `.wfr/sessions` 日志为准。

**非目标**（本期不做，留接口）：
- 交互式审批 / `interrupt` 暂停等人；
- SQLite/`Store` 后端、日志保留策略；
- 跨代理并行写同一文件的审批协商。

## 4. 为什么这样拆（节点化 vs 组件化 · 架构讨论记录）

> 这是与用户反复澄清后压实的结论，记在这里防再翻案。

### 4.1 审批 = 决策 → **做成节点**

审批的本质是"**要不要做**"的决策，不是"做了没有"的状态维护。决策放进流程天然成立：
- 图上可见：deny 后能 `route` 分流（换工具 / 找人确认 / 终止），交互审批能挂 `interrupt`；
- 调试台可观察：审批事件是图事件的一部分；
- 与 `read_only` 标签结合是**数据驱动路由**（节点静态、路由值动态），不违反 IR 静态编译纪律；
- 可选节点 + import 子 workflow：需要副作用的工作流显式贴一个 `approve_tools`，不需要的没有，
  不污染任一 IR。

### 4.2 防重放/幂等 = 状态 → **必须留在执行点**（事务边界）

防重放的本质是"**做了没有**"的状态判断，它必须和 handler 执行**处于同一段不被快照打断的
代码边界**。理由（对着 graph 的恢复语义）：

> graph 每执行完**一个节点**落一次快照（`graph.cpp:284`），快照 = state + 剩余待执行包；
> 恢复 = 重放剩余包，**已跑过的节点绝不重跑**。

若防重放做成**执行点前的前置节点**：
```
think → [guard：查重放+登记意图] → run_tools（写文件）
```
崩溃在 handler 执行中 → 恢复找回 guard 之后的快照 → 直接重放 `[run_tools]` →
handler **又执行一次**。防重放检查在 guard 节点里、已跑完、不会重跑 → **被绕过了**。

- 审批放节点安全（不依赖检查-执行相邻）；防重放放节点必然制造这个洞。
- "以日志判断状态"改变的是**判据来源**（介质），不改变**检查位置**：恢复重跑执行点时，
  "执行前查一次日志"在**执行点内部**才会重跑时再查一次 → 才能兜住。

### 4.3 日志是事实源，但崩溃缺环 → **probe 兜底**

日志（会话 `*.log.jsonl`）是常态事实源。但崩溃可能发生在**结果落盘之前**：日志显示
"已发起、无结果"（`Intent` 残留），无法区分"未开始"与"执行到一半"。此时问**世界现状**
——由工具自查外部产物（文件是否写入且完整、消息是否发出）。见 §6 D3。

### 4.4 副作用记录 → **以日志为准**

审计/执行记录不另起系统，会话日志（唯一事实来源）承载。挂账：`FileJournal` 的存废见 §8 开放项。

## 5. 总体设计

```
think（model）
  │  AI 返回 tool_calls
  ▼
approve_tools（节点 · 决策层）──deny→ route 分流（换工具 / 报用户 / 终止）
  │  denied → 直接产 error ToolMessage
  │  approved_ids 写进 state
  ▼
run_tools（执行点 · 执行层）
  │  对每个 approved call（先串行 prepare → 再并行 handler）：
  │    prepare：查会话日志状态 →
  │      完成   → 重放（不执行）
  │      Intent → 有 probe ? probe 判定（见 D3）：没发生→执行 / 已发生→报告现状 / 部分→fail-closed
  │              无 probe ? fail-closed 拒跑"结果未知请人工确认"
  │      未开始 → 正常执行
  │    handler 产结果 → 记回日志 + 审计
  ▼
（下一节点）
```

## 6. 关键决策

### D1 · approve_tools 节点（新 kind）

- 输入：state 里最后一条 AI 消息的 `tool_calls`；输出：`denied` 转 error ToolMessage 直接并入
  messages，`approved` 的 id 列表写进 state（供 `run_tools` 过滤执行）。
- 判定：复用 agent 层 `ToolApproval` 的 `Decider`/名单/memo 语义（`read_only` 免审照旧），
  但作为**独立的图节点**出现（节点内部判据是纯函数，可离线单测）。
- deny 不被"吞掉"：deny 的 why 回喂 + 由 route 决定后续走向（本期先报用户，交互审批后续接
  interrupt）。

### D2 · 防重放/幂等：执行点内部，以会话日志为事实源

- 检查并入 `ToolNode` 的执行前（`prepare`），状态从**会话日志**按序重建：
  | 日志所见 | 判定 |
  |---|---|
  | 该 `tool_call_id`（含参数指纹）已有 ToolMessage | 已完成 → 重放日志里的结果 |
  | 有 AI.tool_calls、无对应 ToolMessage（Intent 残留） | 进行中/结果未知 → **probe 判定**（见 D3） |
  | 无任何记录 | 未开始 → 正常执行 |
- 重放路径**不执行 handler**（`executions` 不增、审计记 `replay`）。
- 幂等键：复用 `ToolCallId`（默认，与 agent 层一致）；`ToolNameAndArgs` 留作可选项。

### D3 · probe 探测（崩溃兜底）

- 字段：`ToolSpec::probe`（可空）＝ `std::function<ProbeResult(Json const& args)>`，**工具自描述**
  如何检查自己的外部产物（`file_write` 就是磁盘 stat + 内容比对）。
- 调用时机：**仅**命中 `Intent` 残留（日志显示已发起无结果）时调用；正常执行不探测。
- 判据表（**必须区分完成度**，不能只答"存在/不存在"）：

  | ProbeResult | 含义 | 动作 |
  |---|---|---|
  | `Complete` | 产物存在且完整（大小/内容签名匹配） | 视为已完成 → 不重跑，**如实报告现状** |
  | `Partial` | 产物存在但不完整/是残缺（写一半） | **部分发生** → 不重跑、不掩蔽，回"部分写入请人工检查"（fail-closed） |
  | `Absent` | 产物不存在 | 视为未发生 → 允许执行 |
  | `Unknown` | 无法探测 / probe 自身失败/被打断 | 回落旧 `Intent` 行为：拒跑 + "结果未知请人工确认"（fail-closed） |
- **probe 本身必须是只读**：不能写文件/发消息——否则 probe 自己成了副作用（防重放递归）。
  probe 在执行点内部被视为非副作用：不记账、不触发自身探测。
- 无 probe 的工具 → `Intent` 时永远 fail-closed（安全默认：宁可请人，不可重跑）。

### D4 · run_tools 的"先串行 prepare、再并行 run"

`ToolNode` 用 `std::async` 批内并行；日志读取/状态重建/意图登记/审计**非线程安全**。故执行点内：
串行段 `prepare`（逐 call：状态重建 + probe 判定 + 决定）；并行段 `run`（handler 直跑，不碰
日志）。与 detail/07 原"两步 API"一致。

### D5 · 只读免审 & 不记账

`spec->read_only == true` → 审批免审、（若进 execute 路径）不查日志不记账、不审计。`file_read`
/`list_dir` 天然无副作用、无需防护。

## 7. 与 agent 层的关系

- 审批判定复用 `ToolApproval` 的语义（Decider/名单/memo），但 agent 层对象**不动**；
  `approve_tools` 节点内部按同一逻辑写一种节点实现。
- 防重放的状态源从 `FileJournal` 换到**会话日志**——wfr 侧不再另立记账；agent 层的
  `SideEffectGuard`/`FileJournal` 是否一并收编，见 §8 开放项（本期**不改** agent 层）。
- agent 层将来可下沉 `ExecutionGuard`；本期不动，避免 585 回归。

## 8. 变更清单（预估）

| 位置 | 内容 |
|---|---|
| `include/cppchain/tool/tool.hpp` **改** | `ToolSpec` 增 `probe` 字段（可空） |
| `include/cppchain/tool/probe.hpp` **新** | `ProbeResult` 类型 + 判定 helper |
| `include/cppchain/tool/exec_guard.hpp` **新** | 执行点防护：`prepare`（状态重建 + probe）/`run`（handler）/记录 |
| `workflow/node_type.hpp` **改** | 注册 `approve_tools` 节点 kind；`CompileContext` 透传日志句柄 |
| `workflow/node_types.cpp` **改** | `c_approve_tools`；`c_tool`/`c_run_tools` 挂 `exec_guard` |
| `wfr/main.cpp` **改** | 内置 `approve_tools` 策略；`chat` += `file_write` |
| `tests/tool/exec_guard_test.cpp` **新** | 防重放/probe/只读免审/事务边界 |
| `tests/wf/approve_tools_test.cpp` **新** | 节点审批 + 分流 + 与 run_tools 联调 |
| `tests/wf/side_effect_workflow_test.cpp` **新** | 双执行路 + 崩溃恢复探针 |

## 9. 验收口径（沿用 problems/03 §5 的"验证陷阱"纪律）

- **必须断言被测路径真的被走到**：deny/allow/replay/executions/审计计数 + 磁盘断言，
  不看模型嘴上说什么。
- 真机用**中危工具**演示；高危工具模型自己会拒、中间件没被走到 = 假象。
- probe 的 `Partial`/`Absent`/`Unknown` 三格各一个离线测试（残缺文件 / 不存在 / probe 抛错）。
- 崩溃恢复探针：写文件到一半模拟中断 → 重启 → 断言不二次执行（replay 或 probe 兜住）。
- 全仓回归保持全绿（当前 585）。

---

> 详细接口/数据流/测试点见 [03-detail-design/07](../03-detail-design/07-副作用防护工作流接入详细设计.md)。