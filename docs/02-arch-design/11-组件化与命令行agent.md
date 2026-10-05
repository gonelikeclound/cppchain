# 11 · 组件化与命令行 Agent（拟定 M12）

> 设想：把 **agent / history / memory / tool** 都做成**可按名字引用的组件**，
> 用 JSON 定义节点、由 workflow 串联，产出**命令行 agent 工具**；
> 指定 agent 的**工作路径**，从工作路径提取历史消息 → **跨命令连续对话**。
>
> 关键结论（对齐 langgraph 的 ReAct 图）：**agent 循环不是节点内部的循环，而是图上的回边** ——
> "agent 节点"只是一次模型调用，多轮往返由 `model → route → run_tools → (回边)` 显式构成。
>
> 状态：设想 + 一处已实证（落盘续跑，见 §7）。其余为设计，不含实现。

---

## 1. 定位与边界

- **依赖**（几乎全部已就绪）：

| 依赖 | 提供的东西 |
|---|---|
| M5 `state` | `StateSchema` + reducer（含 `add_messages`）、`Checkpointer`、`Store` |
| M6/M11 `graph`+`workflow` | IR → 校验 → 编译成图；条件边；**每 super-step 落快照**；`max_steps` 上界 |
| M3 `tool` | `ToolSpec`/`ToolRegistry`、**`ToolNode`**（执行 tool_calls → ToolMessage） |
| M7 `agent` | `Agent` 循环、`Middleware` 四钩子 |
| M8 `graph_agent` | `as_agent_node`（把 `Agent` 包成图节点） |
| M11 生成端 | 意图 → IR（PlanStage → IRStage） |

- **提供**：节点家族扩展（`model`/`route`/`run_tools`/`load_history`/`save_history`）+
  组件按名引用 + 命令行工具形态。
- **不做**：M10 运行时补全、组件热插拔、多进程并发写同一路径、多模态历史。

---

## 2. 参照：langgraph 怎么把 agent 循环拆成图

### 2.1 参照实现的形状

langgraph 的 ReAct agent（`create_react_agent` / 手写版）就是**两个节点 + 一条回边**：

```python
builder = StateGraph(MessagesState)          # messages 走 add_messages reducer
builder.add_node("agent", call_model)        # ← 单次模型调用，内部不循环
builder.add_node("tools", ToolNode(tools))   # ← 执行 tool_calls，产出 ToolMessage 追进 messages
builder.add_edge(START, "agent")
builder.add_conditional_edges("agent", tools_condition)   # → "tools" 或 END
builder.add_edge("tools", "agent")           # ← 回边：这才构成循环
```

- **`tools_condition`**：只看最后一条消息 —— `last.tool_calls` 非空 → `"tools"`，否则 → END。
- **循环终止** = 最后一条 AI 消息没有 tool_calls。
- **`recursion_limit`（默认 25）** 数的是**总 superstep 数**（不是 loop 计数器），超限抛
  `GraphRecursionError`。langgraph 自己提醒：`tools_condition` **不防死循环**，
  建议另加 iteration 计数。
- 另有 `create_agent`（middleware 制）—— cppchain 的 M7 `Agent` 对应的是它。

### 2.2 映射到 cppchain：大部分零件已经有了

| langgraph | cppchain 现状 |
|---|---|
| `StateGraph` + 条件边 | ✅ `StateGraph::add_conditional_edges(from, RouterFn)` |
| `MessagesState`（`add_messages`） | ✅ `StateSchema::messages_state()` |
| `ToolNode` | ✅ **`tool::ToolNode` 已存在**（M3），产物就是 ToolMessage |
| `agent` 节点（单次模型调用） | ❌ 缺：M7 `Agent::invoke` 是"模型+循环+middleware"一体的黑盒 |
| `tools_condition` | ❌ 缺：IR 表达不了"按最后一条 AI 消息有无 tool_calls 路由" |
| `recursion_limit` | ✅ `CompiledGraph::max_steps`（默认 100）是同一件事 |
| 每步可见 / 可恢复 | ✅ **M6 每 super-step 落快照** —— 拆开后循环的中间态天然可观测、可恢复 |

**结论：拆分基本是"接线"** —— 真正缺的只有 `model` 节点与那个路由（§5）。

### 2.3 拆分的收益（不只是"更像 langgraph"）

1. **middleware 不丢，位置反而变显式**：`before_model`（压缩/截断）挂 `model` 节点；
   `approve_tool_call` / `lookup_tool_result` / `after_tool`（审批/防重放/截断）挂 `run_tools`
   节点 —— 从"黑盒内部"变成**图上看得见的位置**。M7/M9 的中间件全部保留。
2. **循环有界且显式**：langgraph 的 `tools_condition` 不防死循环、只能靠 `recursion_limit` 兜；
   cppchain 的 `label + goto` **带 `max`**，循环次数直接写在 IR 里（§5.3）。
3. **可恢复**：M11 的 goto 用 `state.loop_counts` 计数、每步落快照 —— 循环跑到第 3 轮崩了，
   能从那个快照续。

---

## 3. 模型：state 传参，一切皆节点

### 3.1 目标形状

workflow 用 **state 在节点间传参**；没有特殊节点，全部平级：

```
[load_history] → [L: label] → [think: model] → [route] ─有 tool_calls→ [run_tools] → [goto L]
                                                   └无 tool_calls→ [save_history] → END
```

逐条：

- **`load_history`**：从工作路径取历史消息，**追加进 `state.messages`**。本次用户输入也经
  `messages` 通道进来，两者由 `add_messages` reducer 合并（同 id 覆盖、无 id 追加）—— M5 现成。
- **`model`**：把 `state.messages` **全部取出**组装请求发一次 LLM，把回复**追进 `messages`**。
  **它只调用一次**，不循环。
- **`route`**：看最后一条 AI 消息有无 `tool_calls`，决定去 `run_tools` 还是收尾。
- **`run_tools`**：执行那条消息里的 `tool_calls`，把 ToolMessage 追进 `messages`。
- **回边**：回到 `model` 再发一轮。**这就是"循环"**——与 langgraph 的 `tools → agent` 等价。
- **`save_history`**：把 `state.messages` 写回工作路径。位置自由：放 `model` 前就先存下用户
  输入（中途崩了也不丢），放 `route` 后则存完整一轮。

**关于"结果存 state.result"**：M11 已有的约定是按节点分槽 `state.results[<节点 id>]`。
agent 轮的最终答复就在最后一条 AI 消息里，落盘与引用都走 `messages` 通道，不需要额外结果槽。

### 3.2 为什么不用黑盒 agent 节点

一个自然的替代是直接把 M7 `Agent`（内部自带多轮循环）包成一个节点。**不采用**：

1. **不可观测**：中间每一次工具往返都被吞在节点内部，图上看不见、也就落不了快照。
2. **不可恢复**：循环跑到一半失败，只能整轮重来。
3. **middleware 位置被隐藏**：压缩/审批挂在哪儿成了实现细节，而不是编排的一部分。
4. **不可编排**：想在"调完工具后插一个校验节点"做不到（黑盒内部插不进去）。

代价也要说清：**节点数变多**。一个 ReAct agent 从 1 个黑盒节点变成 7 个（§5.3），
IR 也长了 —— 这直接影响 §8 的 Q4（初版不让生成器产出这些节点）。

### 3.3 as_agent_node 不是废弃，是重新定位

| 场景 | 形态 | 理由 |
|---|---|---|
| **subagent-as-tool**（`ask_expert`） | **黑盒**（`as_agent_node`，M8 现状） | 目的就是**上下文隔离**：子代理内部循环越看不见越好，父只关心最终 `answer` |
| **主 ReAct 循环** | **拆开**（本设计） | 要可观测、可恢复、middleware 位置显式 |

这个区分是干净的：**黑盒适合"被委派"，拆开适合"被编排"。**

### 3.4 为什么 history 不做成基础设施

替代方案："整个 state 落盘（快照链），下次 `fire_once` 带同一 `thread_id` 从快照恢复"。不用：

1. **和 D1 打架** —— M11 明确"每次触发全新状态"，靠快照恢复等于把续跑语义塞进一个声明
   "每次全新"的引擎，两种语义互相污染。
2. **做成节点才可组合** —— 可条件加载、可在前后各放一个、多工作路径并存。
3. **落盘更小** —— 只落消息，不落整个 state。

---

## 4. 现状盘点：家底 vs 差距

| 设想 | 现状 | 差距 |
|---|---|---|
| tool 组件 | ✅ `ToolRegistry` + `ToolSpec`（JSON Schema） | 无 |
| `ToolNode` | ✅ 已存在（M3） | 需"写 messages 通道"的语义（§5.1） |
| agent 循环 | ⚠️ M7 `Agent` 是黑盒循环 | 拆成 `model` + `route` + `run_tools` + 回边 |
| history | ❌ 无 | 两个节点 + 工作路径读写 |
| memory 组件 | ⚠️ 只有 `MemoryStore` | 落盘后端 |
| 组件按名引用 | ❌ 无 | 注册表 + 白名单校验 |
| JSON 定义节点 | ⚠️ `NodeKind { Tool, Expr, Label, Goto }` | 缺 5 种（§5.1） |
| 命令行工具 | ✅ `agent_cli` / `proactive` / `session_resume` | 可作底座 |

---

## 5. 设计

### 5.1 节点家族（5 个新 kind）

| kind | 语义 | 写哪里 | 复用 |
|---|---|---|---|
| `model` | 一次 LLM 调用，回复追进 messages | `messages` | 从 M7 `Agent::invoke` 抽出"调模型"那一段 |
| `route` | 按**封闭词表**的条件路由（§5.2） | — | 编译成 `add_conditional_edges` |
| `run_tools` | 执行最后一条 AI 消息的 tool_calls | `messages` | **`tool::ToolNode` 已实现**，只差通道语义 |
| `load_history` | 工作路径 → messages | `messages` | 新增（§7 的落盘纪律可复用） |
| `save_history` | messages → 工作路径 | — | 新增 |

**命名注意**：`run_tools` 刻意不叫 `tools`/`tool` —— 与 M11 已有的 `tool` 节点区分：
`tool` 节点是**业务语义**（调某个工具，产物进 `results[<id>]`），
`run_tools` 是**对话语义**（跑模型刚请求的那批工具，产物进 `messages`）。两者不可混用。

### 5.2 `route`：封闭词表（选定方案 a）

```jsonc
{ "id": "route", "kind": "route",
  "when": "last_ai_has_tool_calls",   // ← 封闭词表，初版只有这一个
  "then": "act", "else": "persist" }
```

**为什么是封闭词表而不是扩 expr 语言**：expr 的定位是"标量比较"，明确**无函数**
（`11-ir-syntax.md` §6）。而 `tool_calls` 是消息级结构、不是 `$<id>.result.<字段>` 能表达的。
加内建谓词等于扩语言；用封闭词表则**未知 `when` 直接在编译期拒绝**，与项目
"白名单 / 歧义即错"的纪律一致（对标 `VN6` 对未知工具的做法）。

后续若需要新条件（如"连续 N 轮调同一个工具就停"），往词表里加，不改文法。

### 5.3 完整 IR 样例（ReAct + 历史）

```jsonc
{ "id": "wf_cli", "trigger": {…},
  "nodes": [
    { "id": "recall", "kind": "load_history",
      "path": "@config.workspace", "session": "@config.session", "next": "L" },

    { "id": "L", "kind": "label" },                       // ← 循环头

    { "id": "think", "kind": "model",
      "model": "main", "tools": "research_toolset",        // 组件名，§5.4
      "next": "route" },

    { "id": "route", "kind": "route", "when": "last_ai_has_tool_calls",
      "then": "act", "else": "persist" },

    { "id": "act", "kind": "run_tools", "next": "loop" },  // ← 不直接指回 L（见下）

    { "id": "loop", "kind": "goto", "goto": "L", "max": 8 },   // ← 回边 + 有界

    { "id": "persist", "kind": "save_history",
      "path": "@config.workspace", "session": "@config.session", "next": "_turn_end" }
  ]}
```

**为什么必须有 `loop` 这个 goto 节点**（不显然，但关键）：M11 的校验规则 **VG2 禁止前向边成环**
（"循环只能通过 label + goto 表达"）。若让 `act.next = "L"`，解析后是 `act → think` 的前向边，
与 `think → route → act` 构成**前向环 → 校验拒绝**。必须由 `goto` 节点产生**回边**，
前向边才保持无环。这是"7 个节点"里那一个的来源。

`max: 8` 就是 langgraph `recursion_limit` 的**显式版**：循环次数写在 IR 里，
配合引擎的 `max_steps` 双保险。

### 5.4 组件化：按名字引用

四类组件 `model` / `tool` / `agent` / `memory`；宿主（CLI）启动时**静态注册**，IR 按名引用。

- **tool** 已有 `ToolRegistry`，直接用；**model/agent/memory** 新增名字表。
- **白名单纪律复用 M11 对工具的做法**：生成器提示词目录 == 注册表，未知名字**校验期拒绝**
  （对标 `VN6`）。不引入第二套心智。
- **工作路径经 `@config` 注入**：CLI 把 `--workdir`/`--session` 放进 config，IR 写
  `@config.workspace`/`@config.session` —— **直接复用刚做完的 config 契约（B2/B3）**，
  写错键名编译期就被 VR4 拒掉，不会运行期静默变 null。
- **不做热插拔**：启动时静态注册，不是 `.so` 动态加载（编译期类型安全、无 ABI 问题）。

### 5.5 命令行形态

```
cppchain-cli [--workdir DIR] [--session NAME] [--reset] [--show] [--workflow FILE] "<意图>"
  --workdir   工作路径（决定 history 落点）
  --session   同一路径下多段会话（缺省 "cli"）
  --show      不起模型，只打印已存历史（离线可用；验证 A 已实现同款）
  --reset     清空该 (workdir, session) 的历史
  --workflow  直接跑一份手写 IR（**推荐起步路径**）
  无 --workflow  走 M11 生成器：意图 → IR → 编译 → fire_once
```

---

## 6. 契约

- **IR 扩展**：新增 `NodeKind::{Model, Route, RunTools, LoadHistory, SaveHistory}`；
  五者与既有节点一样受单链约束（该有 `next` 的必须有）。
- **新增校验规则**（编号待定，接 `VN` 段）：
  · 组件名（`model`/`tools`）必须在注册表里（对标 VN6）；
  · `route.when` 必须在**封闭词表**里（未知 → 拒绝）；
  · `route.then/else` 必须指向存在的节点（VG1 复用）；
  · `load_history` 的 `path`/`session` 形状合法（VR 段规则复用）；
  · **顺序检查**：`load_history` 必须出现在消费 messages 的节点之前（硬拒绝，见 §8 Q8）。
- **文件布局**：`<workspace>/.cppchain/session-<name>.json`（沿用验证 A 的形状）。
  建议同时写 `.cppchain/.gitignore`（内容 `*`），避免会话历史被误提交。
- **落盘纪律**（沿用验证 A）：**原子写**（临时文件 → `rename`）；**fail-closed**
  （损坏默认抛异常，不静默以空历史启动）。
- ⚠️ **`std::optional` 字段必须显式写 string 或 null**：nlohmann 对 optional 无内建支持，
  直接塞会走"数组构造路径"（曾导致快照链的 parent 在文件往返中全部丢失，见 §7.3）。

---

## 7. 已验证：验证 A（落盘续跑）

### 7.1 结论

"把历史落到工作路径，两次**独立进程**能续上同一段对话"—— **成立**。真机实测
（`examples/session_resume`，同一 workdir）：

```
进程 1：恢复 0 条 → 发往模型 2 条 → 落盘
── 全新进程 ──
进程 2：恢复 3 条 ← 上一次进程留下的 → 发往模型 4 条
        "我叫什么？我在做什么项目？" → 答出只有上一轮才知道的信息
换 workdir → 0 条（隔离成立）
```

### 7.2 在本设计中的定位

验证 A 用的是 `FileCheckpointer`（落盘**快照链**）。按 §3 的模型，
**落盘单位是对话消息列表，不是整个 state**：

| A 的产物 | 定位 |
|---|---|
| 原子写 / fail-closed / 损坏检测 | ✅ **直接复用**（纪律与数据形态无关） |
| `std::optional` 序列化的坑 | ✅ **同样会踩**，必须带着教训写消息文件 |
| `FileCheckpointer` + 快照链 | ⚠️ 通用设施（对"续跑图"仍有价值），但**不在本设计关键路径上** |

**A 证明的是"跨进程落盘读回可行"，不是"必须用快照链"。**

### 7.3 顺带查出的真 bug（教训要留着）

落盘文件出现 `"parent_checkpoint_id": []` → 追下去发现 **engaged 的 parent 也是 `[]`** →
加断言 → 测试失败：**nlohmann 对 `std::optional` 没有内建支持，直接塞会走"数组构造路径"**
（`message/serializer.hpp` 早有此警告，状态层没有那句话），nullopt 与 engaged 都变成 `[]`。

> 教训：这个 bug 是在**测试全绿**的状态下发现的 —— 原用例只在同进程内断言了 parent，
> 恰好绕过了文件往返。**是去看真实落盘文件、而不是相信测试绿灯，才抓到它。**

---

## 8. 待定问题

| # | 问题 | 选项 | 倾向 |
|---|---|---|---|
| Q1 | history 粒度 | — | ✅ **已定**：history 是节点，state 是唯一真相源（§3.1） |
| Q2 | 工作路径怎么定 | cwd 隐式 / `--workdir` 显式 | **显式优先、缺省 cwd** |
| Q3 | 落盘形态 | — | ✅ **已定**：消息列表（非快照链） |
| Q4 | 生成器要不要产出这些节点 | 要 / 初版不要 | **初版不要** —— 拆解后一个 ReAct 要 7 个节点 + 回边 + 路由，生成难度陡增；先只走手写 IR |
| Q5 | 并发写同一 workdir | 不支持 / 文件锁 / 合并 | **先不支持**，检测到即报错 |
| Q6 | memory 作用域 | 随 workdir / 全局 | **全局或按用户** |
| Q7 | `model` 节点的工具集怎么给 | 内联列表 / 按名字引用 tool 组件 / 整个注册表 | **按名字引用**（内联会让工具集散落在 IR；整个注册表则失去 M3 工具检索的意义） |
| Q8 | `load_history` 顺序不对 | 硬拒绝 / 告警 | **硬拒绝**（与"歧义即错"一致） |
| Q9 | `save_history` 落盘时机 | 每次执行落一次 / 仅结束时 | **每次落一次**（崩溃不丢），但会放大 Q5 |
| Q10 | **`route` 词表要不要扩** | 只 `last_ai_has_tool_calls` / 加"连续 N 轮同工具即停" | **先只一个**，按需加 |

---

## 9. 分阶段实施（建议）

| 阶段 | 内容 | 依赖 | 风险 |
|---|---|---|---|
| **A** ✅ | 落盘续跑验证（跨进程读回） | 无 | 已完成 |
| **B** ✅ | 本文（形状与取舍） | A | — |
| **C** | `model`/`route`/`run_tools` 三节点 + 循环（ReAct 打通，**先不接 history**） | B | IR 扩张；需同步 IR 语法文档 |
| **D** | `load_history`/`save_history` + CLI（`--workdir`/`--session`/`--show`/`--workflow`） | C | 小 |
| **E** | 组件注册表完善（model/agent/memory）+ memory 落盘 | C | 小 |
| **F** | 教生成器产出这些节点（Q4 放开） | C+D + 生成端评测方法 | 生成质量与 IR 表达力互相纠缠 |

**C 阶段刻意先不接 history** —— 先把"图上的 agent 循环"本身打通并验证（能跑、有界、
每步可恢复），再叠历史。两件事分开验证，出问题好归因。

---

## 10. DoD（草案）

- [ ] `NodeKind::{Model, Route, RunTools, LoadHistory, SaveHistory}` + IR 解析 + 校验规则
- [ ] ReAct 循环跑通：`model → route → run_tools → goto` 有界回跳，每步落快照
- [ ] `route.when` 封闭词表 + 未知值编译期拒绝
- [ ] `run_tools` 复用 `tool::ToolNode`，产物写 `messages` 通道
- [ ] middleware 可挂到 `model`（before_model）与 `run_tools`（审批/after_tool）
- [ ] `load_history`/`save_history` + `<workspace>/.cppchain/` 布局与被忽略
- [ ] `--workdir` 经 `@config` 注入并被 B2 的 config 契约校验
- [ ] CLI：`--workdir`/`--session`/`--reset`/`--show`/`--workflow`
- [ ] 离线单测：循环有界、route 词表、history 节点、跨 workdir 隔离
- [ ] 真机：同一 workdir 两次独立进程，第二轮答出只有第一轮才知道的信息
- [ ] IR 语法文档（`主动智能/11-ir-syntax.md`）同步；生成器提示词暂不动（Q4）

---

## 11. 开放点

1. **并发写**（Q5）：两个终端同跑一个 workdir 会互相覆盖。`flock` 或 append-only + 合并，
   但先要定"合并"的语义。
2. **历史体积**：消息列表随轮数增长。`TrimHistory` 只压缩"发往模型的视图"，落盘仍是全量。
3. **IR 体积**：一个 ReAct 要 7 个节点。若手写太啰嗦，可考虑一层"宏展开"
   （IR 里写 `agent_loop` 简写，编译期展开成 7 个节点）—— 但这会把结构又藏起来，需权衡。
4. **history 与 memory 的分界**：history = 这次会话的消息（随 workdir）；
   memory = 跨会话的事实（`Store`，建议全局）。把长期事实塞进 history 会导致
   "换个目录就失忆"。
5. **组件热插拔**：动态加载留作开放点（§5.4 的理由）。
6. **与 M10 的关系**：Streaming/Interrupt 会改变"一轮"的边界，可能影响 `save_history`
   的落盘时机（Q9）。
