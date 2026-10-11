# 18 · 副作用防护接入 Workflow 执行路径（wfr-㊷）—— 审批 / 防重放 / 审计

> 专题：**副作用防护进 wfr（Workflow 图通道）**。承接 [20-副作用全景分析](../04-problems/20-副作用全景分析.md)：
> 全景指出 `file_write` 挂不进 wfr loop 的根因是——**Workflow 图的工具执行不经过任何中间件**，
> agent 层那套纵深（`ToolApproval` + `SideEffectGuard` + `FileJournal`）只在 `Agent` 循环通道生效。
>
> 本篇把整套语义搬进 Workflow 工具执行点，补一块**缺失的通道**而不是加一层新墙。

## 1. 定位与边界

- **在做什么**：给 Workflow 图的两个工具执行点（`c_tool` 单工具节点、`c_run_tools` 批量节点）
  接上**审批 + 防重放 + 审计**，并顺带闭环 `file_write` 入 wfr loop（`chat` 工具集 + 审批策略）。
- **不在做什么**：不搬 agent 的 `Middleware` 链进 workflow（那是为循环设计的钩子序列，语义太重）；
  不改工具实现；不做 HITL 交互式审批（先做**策略式审批**，交互式/`interrupt` 后人场是后续迭代）；
  不动沙箱纪律（`resolve_path_in_root` 三连挡保持不变）。

## 2. 断层回顾（为什么 `file_write` 进不去）

| 通道 | 工具执行 | 审批 | 防重放 | 持久化 | 审计 |
|---|---|---|---|---|---|
| `Agent` 循环（库/examples） | `approve → lookup → node.run → after` 中间件链 | ✅ | ✅ | ✅ | ❌ |
| Workflow 图（wfr 命令行） | `c_tool` 直跑 handler / `c_run_tools` 跑 `ToolNode::run` | ❌ | ❌ | ❌ | ❌ |

`src/wfr/main.cpp` 注册处注释原话：*"`file_write` 是副作用（read_only=false）且 wfr loop 没有审批中间件 → 不默认挂，要挂得先接审批。"*
── 这次就是"接审批"。

## 3. 目标与非目标

**目标**：
1. Workflow 两个工具执行点都能被一套**可注入防护**包住：审批 →（命中则重放/拒绝）→ 执行 → 记账 + 审计。
2. `file_write` 挂进 wfr `chat` 工具集，受审批管控；`file_read`/`list_dir` 只读仍免审、不记账。
3. 决策留痕（审计）最小可用：`{ts, tool, args, decision, who, why}`；执行记忆（防重放）复用 `FileJournal` 语义。
4. 与 agent 层**零重复实现**：防护逻辑抽成独立件，两通道共用。

**非目标**（本期不做，留接口）：
- 交互式审批 / `interrupt` 暂停等人（用 M10 的 `interrupt` 原语接是后续迭代）；
- SQLite/`Store` 后端、日志保留策略；
- 子代理并行写同一文件的跨代理协商审批。

## 4. 方案对比与选型

| 方案 | 做法 | 优点 | 代价 |
|---|---|---|---|
| **A · 把 Middleware 链搬进 workflow** | workflow 工具执行点持有 `std::vector<Middleware*>`，逐个调 | 复用现有类，改动小 | 中间件为 `Agent` 循环设计（`before_model`/`after_tool` 全是消息变换），搬过来多半语义不匹配；治标 |
| **B · 单工具执行点内嵌 `ExecutionGuard`** ⭐ | 新抽一个**只服务"单次工具调用"** 的防护组件（审批→查重→执行→记账→审计），`c_tool`/`c_run_tools` 各包一层 | 语义精确匹配"执行一次工具"；两通道、两节点复用同一组件；将来 agent 层也可下沉 | 需把 agent 层两件事（approve / side-effect）拆出公共面 |
| **C · 宿主层在编译后包** | wfr 在 `compile_workflow` 之后、运行期外层再包一层 | 宿主完全掌控 | 图内联回边（goto/loop）绕过外层；漏网点 |

**选 B**。理由：Workflow 工具执行是**单点发生**（一次 `tool_call` 一把处理器调用），不是"一轮循环"，
防护的天然粒度就是"单次调用"，而不是"中间件序列"。抽 `ExecutionGuard`（单次工具调用的防护门）作为
公共件，`c_tool` 与 `c_run_tools` 共享，将来 agent 层的 `ToolNode` 也可下沉同一件。

## 5. 总体设计

```
                    Workflow 编译（compile_workflow）
                          │  CompileOptions::tool_policy
                          ▼
             ┌─────────────────────────────┐
             │  ExecutionGuard（一次调用的防护门） │
             └────────────┬────────────────┘
                       injected into
             ┌────────────┴─────────────┐
             ▼                          ▼
        c_tool（单工具节点）        c_run_tools（批量节点）
        每把 handler 包一层    → 逐 call 包（批量内先串行审批 → 再按 ToolNode 并行执行）
```

三个子件：

| 子件 | 职责 | 复用/新抽 |
|---|---|---|
| **审批** | 名单/回调/Decider + 只读免审 + 记忆化（拒绝→不再重复问） | 复用 `agent::ToolApproval` 的 `Decider`/memo 语义，新抽**公共接口** |
| **防重放** | 幂等键三态（未见过/Completed/Intent/Failed），可持久化 | 复用 `agent::ToolExecutionJournal` + `FileJournal`（已是公共件，直接拿） |
| **审计** | 每次判定落地：`{ts, tool, args_hash, decision, who, why}` | 新抽：轻量追加写 `.wfr/audit/*.jsonl`（原子写 + fail-closed） |

## 6. 关键决策

### D1 · 防护粒度 = 单次工具调用，不是中间件序列
`ExecutionGuard` 暴露**一个执行入口**（而非三个钩子）：
`execute(call, spec, args) → ToolMessage`。内部顺序固定：
`审批 → 命中重放? →（登记意图）→ handler →（记结果 + 审计）`。
两通道与两种节点都只调这一个入口——这样**审批/防重放/审计三件事在一个事务边界内**，不重不漏。

### D2 · 审计与执行记忆分开
- **执行记忆**（防重放）：`FileJournal` 已有"意图在**执行前**落盘 + 结果原子写"。
- **审计**（谁在何时批准了什么）：独立追加写，**decision 明确**，供复盘"可置信"。
两者不合并：journal 是执行态（机器重放用），audit 是决策史（人/复盘用）。
去向：wfr 侧落到 `.wfr/audit/`；不落 audit 时放宽为纯内存计数（离线测试可断言计数）。

### D3 · wfr 挂载策略
- `chat` 工具集 += `file_write`（同 `ft_cfg` 沙箱根 = 工作区，越界由现有三连挡拒）。
- 默认策略：`DenyByDefault` 之外的**白名单回调**（只允许写工作区内 `.wfr/` 之外？先简单：只允许写 `<root>` 内已 resolve 的路径——即**命中去重与越界拒之外的合法路径放行**）。
  具体回调在实现时定档（名单 vs 回调），但**默认不给 file_write 开执行白名单**（复盘较稳）。
- `file_read`/`list_dir`（`read_only=true`）→ 审批免审 + 不记账（沿用 agent 层纪律）。

### D4 · `tool_call_id` 幂等键语义
- `c_tool`：`tctx.tool_call_id = node_id`（**节点 id 即标识**，同节点同参数每次重跑 = 同键 → 天然防重复）。
- `c_run_tools`：模型生成的 `call.id`（不稳定，模型重试会换 id）→ 键策略选 `ToolNameAndArgs`（同参同工具 = 同键）
  还是 `ToolCallId`？—— 沿 problems/03 §6.2 的口径：**由使用者按工具幂等性决定**。本期先保持
  `ToolCallId`（与 agent 层默认一致），幂等键分类（`ToolSpec` 上标 `idempotent`）进 detail 作为开放项。

## 7. 与 agent 层的关系

- **零重复**：`ExecutionGuard` 引用 `ToolExecutionJournal`/`FileJournal`（已是公共头文件）；
  审批的 `Decider` 语义从 `ToolApproval` 抽到**抽象接口**，`ToolApproval` 变它的一个实现。
- agent 层**不动行为**：`ToolApproval` 保留（作为 `approve` 抽象的一个实现），`SideEffectGuard` 保留
  （它管"防重放"一件大事，Workflow 需要的"审计"由 `ExecutionGuard` 侧补齐）。
- 将来可下沉：agent 的 `ToolNode` 可改用 `ExecutionGuard` 执行（行为等价），但本期**不改**，避免回归。

## 8. 变更清单（预估）

| 位置 | 内容 |
|---|---|
| `include/cppchain/tool/exec_guard.hpp` **新** | `ExecutionGuard`：审批接口 + 防重放 + 审计三合一 |
| `src/tool/exec_guard.cpp` **新** | 实现 |
| `workflow/node_type.hpp` | `CompileContext` 加 `ExecutionGuard const* guard` 槽位 |
| `workflow/compiler.{hpp,cpp}` | `CompileOptions.tool_policy`（可复用组件指针）；`c_tool`/`c_run_tools` 挂 guard |
| `wfr/main.cpp` | 组装 `ExecutionGuard`；`chat` += `file_write`；`.wfr/audit` 目录 |
| `tests/tool/exec_guard_test.cpp` **新** | 单测（审批/重放/审计/只读免审/事务边界） |
| `tests/wf/side_effect_workflow_test.cpp` **新** | workflow 集成：c_tool 与 c_run_tools 两条路都被 guard 拦截 |

## 9. 验收口径（沿用 problems/03 §5.2 的"验证陷阱"纪律）

- **必须断言被测路径真的被走到**：拒绝/放行/重放计数 + 审计行数，而不是只看模型嘴上说什么。
- 真机用**中危工具**演示（策略禁止但模型愿调），高危工具模型自己会拒、中间件没被走到 → 假象。
- 全仓回归保持全绿（当前 585）。
- 离线确定性：`ExecutionGuard` 全链路用 `FakeTransport` 跑通（不依赖网络）。

---

> 详细接口/数据流/测试点见 [03-detail-design/07](../03-detail-design/07-副作用防护工作流接入详细设计.md)。