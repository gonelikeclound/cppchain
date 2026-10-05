# 11 · 组件化与命令行 Agent（拟定 M12）

> 设想：把 **agent / history / memory / tool** 都做成**可按名字引用的组件**，
> 用 JSON 定义 **agent 节点**、由 workflow 串联节点，产出**命令行 agent 工具**。
> 关键诉求：指定 agent 的**工作路径**，从工作路径提取历史消息 → **跨命令连续对话**。
>
> 状态：设想 + 一处已实证。落盘续跑已由**验证 A** 跑通（§6），其余为设计。
> 本文只定形状与取舍，不含实现。

---

## 1. 定位与边界

- **依赖**（全部已就绪）：

| 依赖 | 提供的东西 |
|---|---|
| M5 `state` | `StateSchema` + reducer（含 `add_messages`）、`Checkpointer`、`Store` |
| M6/M11 `graph`+`workflow` | IR（JSON）→ 校验 → 编译成图 → `fire_once`；state 传参；每步快照 |
| M7 `agent` | `Agent` 循环（多轮工具往返直到结束/出错）、`Middleware` 四钩子 |
| M8 `graph_agent` | `as_agent_node`：把 `Agent` 包成图节点（已有） |
| M11 生成端 | 意图 → IR（PlanStage → IRStage） |

- **提供**（本文要定的形状）：
  ① **agent 节点进 IR**（第 5 种节点类型），语义 = 读 `state.messages` → 多轮 LLM → 写回结果；
  ② **history 节点**（`load_history` / `save_history` 两个），把工作路径里的历史消息与
  `state.messages` 打通；
  ③ **组件按名字引用**（model / tool / agent / memory），白名单 fail-closed；
  ④ **命令行工具**形态（`--workdir` / `--session` / 单次与 REPL）。

- **不做**（押后）：

| 不做 | 归属 |
|---|---|
| M10 的 Streaming / Interrupt / Send | 先于本设计，或并行 |
| 组件热插拔（`.so` 动态加载） | 开放点 |
| 多进程并发写同一工作路径 | 开放点（§7 Q5） |
| 多模态历史、跨设备同步 | 不做 |

---

## 2. 模型：state 传参，history 与 agent 都是**普通节点**

这是本设计的核心，也是最初容易做错的地方 —— 先把形状定死，再谈实现。

### 2.1 数据流

workflow 用 **state 在节点间传参**。history 和 agent 都是**其中的节点**，没有特殊地位：

```
[load_history]  →  [agent]  →  [save_history]  →  …
  读工作路径          读 state.messages          读 state.messages
  写 state.messages   多轮 LLM 直到              写工作路径
                      finish / error
                      写 state.results[<id>]
```

逐条说明：

- **`load_history`**：从工作路径取出历史消息，**追加进 `state.messages`**。
  初始输入（本次用户问题）也经 `messages` 通道进来 —— 两者由 `add_messages` reducer 合并
  （同 id 覆盖、无 id 追加），这是 M5 现成的能力。
- **`agent`**：把 `state.messages` 里的消息**全部取出**组装成一次请求发给 LLM；
  之后进入 **M7 已有的多轮循环**（模型要工具就执行工具、再发一轮），
  直到模型给出终止信号（无工具调用 / `finish_reason` / 出错）才退出；
  最终答复写进 state 的结果槽。
- **`save_history`**：把 `state.messages` 写回工作路径。**放在流程任意位置都成立** ——
  放在 agent 前就先存下用户输入（中途崩溃也不丢），放在 agent 后则存完整一轮。

**关于"结果存 state.result"**：M11 已有的约定是按节点分槽 —— `state.results[<节点 id>]`
（tool 节点就是这么写的）。agent 节点沿用同一条，于是下游可以直接用
`$<agent节点id>.result.<字段>` 引用它，**不需要任何新机制**。本文后面把用户说的
`state.result` 一律理解为这个既有槽位。

### 2.2 为什么 history 不做成基础设施

一个自然的替代方案是："把整个 state 落盘（快照链），下次 `fire_once` 带同一个
`thread_id` 从快照恢复"。**不采用**，理由三条：

1. **它会和 D1 打架**。M11 明确"每次触发全新状态（`fire_once` 每次全新 input）"。
   靠快照恢复等于把"续跑语义"塞进一个声明"每次全新"的引擎里，两种语义会互相污染
   （恢复时 `next` 该不该用？图跑完没有？）。
2. **history 做成节点才可组合**：可以只在某些分支加载历史；可以在 agent 前后各放一个；
   不同工作路径可以并存多个 history 节点。做成基础设施，这些全没了。
3. **落盘变小**：只落对话消息，不落整个 state。若落 state，每轮全量存 `values`
   （含全部 messages）会线性膨胀，长会话下很快失控。

**代价**要说清楚：节点方案下，"历史"是 workflow 显式编排出来的，写错顺序（比如 save 放在
load 之前）不会自动被纠正 —— 但这正是 workflow 的语义，且可由校验规则兜住（§5）。

---

## 3. 现状盘点：家底 vs 差距

| 设想 | 现状 | 差距 |
|---|---|---|
| tool 组件 | ✅ `ToolRegistry` + `ToolSpec`（已是 JSON Schema） | 无 |
| agent 组件 | ⚠️ `agent::Agent` 只能用 C++ 构造；`as_agent_node` 已有但未进 IR | **进 IR** + 按名字引用 |
| memory 组件 | ⚠️ `state::Store` 只有 `MemoryStore` | 落盘后端（可与 history 共用文件设施） |
| history | ❌ 无 | **两个节点** + 工作路径读写 |
| JSON 定义 agent 节点 | ❌ `NodeKind { Tool, Expr, Label, Goto }` | 缺新节点类型 |
| workflow 串节点 | ✅ IR → 图，已能串 tool/expr/label/goto | — |
| 命令行工具 | ✅ `agent_cli`（4 策略）、`proactive`、`session_resume` | 可作底座 |

---

## 4. 设计

### 4.1 history：两个节点（选定方案 B）

**`load_history`** —— 读工作路径 → 写 `state.messages`
**`save_history`** —— 读 `state.messages` → 写工作路径

选 B（两个节点）而非 A（一个节点带 `mode`）的理由：

- **一个节点一件事**，与项目纪律一致；
- 校验规则不同：`load` 通常要求"在使用 messages 的节点之前"、`save` 通常在其后，
  分两个 kind 才能分别表达；
- 一个 kind 里用 `op` 决定字段集，会引入"该带哪些键"的歧义 —— 而 IR 的既有纪律是
  **歧义即错**（见 `ir.hpp` 对节点分类的处理）。

节点字段（草案）：

```jsonc
{ "id": "recall", "kind": "load_history",
  "path": "@config.workspace",     // 工作路径（见 §4.3 的 config 契约）
  "session": "@config.session",    // 可选：同一路径下多段会话
  "next": "think" }

{ "id": "persist", "kind": "save_history",
  "path": "@config.workspace", "session": "@config.session",
  "next": "_turn_end" }
```

`session` 存在时的文件布局（沿用验证 A 的形状）：

```
<workspace>/.cppchain/session-<session>.json     # 一个会话一个文件
```

### 4.2 agent 节点

语义（对应用户的描述，也对应 M8 已有的 `as_agent_node`）：

```
读 state.messages（全部）
  → 组装请求（system prompt + 消息 + 工具 schema）
  → 多轮循环：模型要工具就执行，再发一轮；直到 finish / 无工具调用 / 出错
  → 最终答复写 state.results[<节点 id>]
```

节点字段（草案）：

```jsonc
{ "id": "think", "kind": "agent",
  "model": "main",                  // 组件名（§4.3）
  "tools": ["fetch_price", "web_search"],   // 工具子集；见 §7 Q7
  "system_prompt": "…",
  "next": "_turn_end" }
```

**实现上几乎不用写新代码**：`as_agent_node(agent, cfg)` 已经做了"读 state 切片 →
`Agent::invoke` → 写回"。这里的差异只有两点：`input` resolver **默认取全部 messages**
（而不是切片）、输出写 `results[<id>]`。二者都是配置，不是新机制。

**agent 节点是否自带 middleware**（压缩/截断/审批）也应可配 —— 直接复用 M7/M9 的中间件，
但**由谁装配**（节点里内联 / 按名字引用一个 agent 组件）是 §7 Q7 的一部分。

### 4.3 组件化：按名字引用

四类组件：`model` / `tool` / `agent` / `memory`。宿主（CLI）启动时**静态注册**，
IR 里按**名字**引用。

- **tool** 已有 `ToolRegistry`，直接用。
- **model / agent / memory** 新增名字表（名字 → 构造配方）。
- **白名单纪律直接复用 M11 对工具的做法**：生成器提示词里的目录 == 注册表，
  validator 加规则 —— **未知组件名 = 校验期拒绝**（对标 `VN6`）。
  不引入第二套心智。

**工作路径经 `@config` 注入**：CLI 把 `--workdir` / `--session` 放进 config，
IR 里写 `@config.workspace` / `@config.session`。这**直接复用刚做完的 config 契约**
（B2/B3：`CompileOptions::config_keys` + VR4 校验）—— 写错键名在编译期就被拒，
不会运行期静默变 null。

**不做热插拔**：组件在启动时静态注册（`main` 里几十行），不是 `.so` 动态加载。
理由：编译期类型安全、无 ABI 问题；本项目是学习/研究性质，动态加载收益不抵复杂度。

### 4.4 命令行工具形态

```
cppchain-cli [--workdir DIR] [--session NAME] [--reset] [--show] [--workflow FILE] "<意图>"
  --workdir   工作路径（决定 history 落点）
  --session   同一路径下多段会话（缺省 "cli"）
  --show      不起模型，只打印已存历史（离线可用；验证 A 已实现同款）
  --reset     清空该 (workdir, session) 的历史
  --workflow  直接跑一份手写 IR（推荐起步路径）
  无 --workflow  走 M11 生成器：意图 → IR → 编译 → fire_once
```

两条路径，**建议先做第一条**：手写 IR 可控、可测，能立刻验证"组件化 + 跨命令续跑"这个组合；
生成路径要等 §7 Q4（生成器要不要产出 agent / history 节点）定了才谈得上。

---

## 5. 契约

- **IR 扩展**：新增 `NodeKind::LoadHistory` / `NodeKind::SaveHistory` / `NodeKind::Agent`；
  三者都必须有 `next`（单链约束不变）。
- **新增校验规则**（编号待定，接在 `VN` 段）：
  · 组件名必须在注册表里（对标 VN6）；
  · `load_history` 的 `path`/`session` 必须是合法的引用或字面量（VR 段规则可复用）；
  · **顺序检查**：`load_history` 必须出现在"消费 messages 的节点"之前 ——
    否则读了也没人用（保守告警还是硬拒绝，见 §7 Q8）；
  · agent 节点的 `tools` 必须在注册表里。
- **结果槽**：agent 节点的产出写 `state.results[<节点 id>]`（沿用 M11 约定），
  下游用 `$<id>.result.<字段>` 引用。
- **文件布局**：`<workspace>/.cppchain/session-<name>.json`。
  建议同时写一个 `.cppchain/.gitignore`（内容 `*`），避免用户把会话历史误提交。
- **落盘纪律**（沿用验证 A）：**原子写**（临时文件 → `rename`）；
  **fail-closed** —— 文件损坏默认抛异常，不静默以空历史启动。
- ⚠️ **`std::optional` 字段必须显式写 string 或 null**：nlohmann 对 optional 没有内建支持，
  直接塞会走"数组构造路径"（曾导致快照链的 parent 在文件往返中全部丢失，见 §6.3）。

---

## 6. 已验证：验证 A（落盘续跑）

### 6.1 结论

"把历史落到工作路径，两次**独立进程**能续上同一段对话"—— **成立**。真机实测
（`examples/session_resume`，同一 workdir）：

```
进程 1：恢复 0 条 → 发往模型 2 条 → 落盘
── 全新进程 ──
进程 2：恢复 3 条 ← 上一次进程留下的 → 发往模型 4 条
        "我叫什么？我在做什么项目？" → 答出只有上一轮才知道的信息
换 workdir → 0 条（隔离成立）
```

### 6.2 验证 A 在本设计中的定位（**重要修正**）

验证 A 当时用的是 `FileCheckpointer`（落盘**快照链**）。按 §2 修正后的模型，
**落盘单位是对话消息列表，不是整个 state** —— 所以：

| 验证 A 的产物 | 在本设计中的定位 |
|---|---|
| 原子写 / fail-closed / 损坏检测 | ✅ **直接复用**（这些是纪律，与数据形态无关） |
| `std::optional` 序列化的坑（§6.3） | ✅ **同样会踩**，必须带着这个教训写消息文件 |
| `FileCheckpointer` + 快照链 | ⚠️ **不在本设计的关键路径上**。它是通用设施（对"续跑图"仍有价值），但 history 节点不需要它 |

换句话说：**A 证明了"跨进程落盘读回"这件事可行，并顺带排掉了一个真实的序列化陷阱；
但它没有证明"必须用快照链"** —— 恰恰相反，修正后的模型更简单。

### 6.3 顺带查出的真 bug（教训要留着）

落盘文件出现 `"parent_checkpoint_id": []` → 追下去发现 **engaged 的 parent 也是 `[]`** →
加断言 → 测试失败：**nlohmann 对 `std::optional` 没有内建支持，直接塞会走"数组构造路径"**
（`message/serializer.hpp` 早有此警告，但状态层没有那句话），nullopt 与 engaged 都变成 `[]`，
回读按 `is_string()` 守卫就全丢。

> 教训：这个 bug 是在**测试全绿**的状态下发现的 —— 原用例只在同进程内断言了 parent，
> 恰好绕过了文件往返。**是去看真实落盘文件、而不是相信测试绿灯，才抓到它。**

---

## 7. 待定问题（需拍板）

| # | 问题 | 选项 | 倾向 | 影响面 |
|---|---|---|---|---|
| Q1 | ~~history 粒度~~ | — | ✅ **已定**：history 是节点，state 是唯一真相源（§2） | — |
| Q2 | 工作路径怎么定 | cwd 隐式 / `--workdir` 显式 | **显式优先、缺省 cwd** | 影响"目录即会话"的直觉 |
| Q3 | ~~落盘形态~~ | — | ✅ **已定**：消息列表（非快照链） | — |
| Q4 | 生成器要不要产出 agent / history 节点 | 要 / 初版不要 | **初版不要**（只走手写 IR） | 决定生成质量与 IR 表达力问题是否互相纠缠 |
| Q5 | 并发写同一 workdir | 不支持 / 文件锁 / 合并 | **先不支持**，检测到即报错 | 多终端同时用会踩 |
| Q6 | memory 组件的作用域 | 随 workdir / 全局 | **全局或按用户** | 影响"长期记忆"语义 |
| Q7 | **agent 节点的工具集怎么给** | 内联列表 / 按名字引用 tool 组件 / 用整个注册表 | **按名字引用**（见下） | 决定"组件"这层做多重、IR 暴露多少字段 |
| Q8 | `load_history` 顺序不对时 | 硬拒绝 / 保守告警 | **硬拒绝**（与"歧义即错"一致） | 影响校验器严格度 |
| Q9 | `save_history` 落盘时机 | 每次节点执行落一次 / 仅流程结束时 | **每次落一次**（崩溃不丢） | 与 Q5 的并发问题相关 |

**Q7 展开**（最能牵动形状的一条）：内联列表最简单但把工具集散落在 IR 里，改一处要动所有
节点；用整个注册表则失去"这个 agent 只该看到这几个工具"的表达力（而 M3 的工具检索正是
为了解决工具过多）；**按名字引用一个 tool 组件**（`"tools": "research_toolset"`）居中，
且与"组件化"的初衷一致 —— 故倾向它。

---

## 8. 分阶段实施（建议）

| 阶段 | 内容 | 依赖 | 风险 |
|---|---|---|---|
| **A** ✅ | 落盘续跑验证（跨进程读回） | 无 | 已完成 |
| **B** ✅ | 本文（形状与取舍） | A | — |
| **C** | `load_history`/`save_history`/`agent` 三种节点 + 校验规则 + 组件注册表 + 手写 IR 的 CLI | B | IR 面扩张，需同步 IR 语法文档与生成器提示词 |
| **D** | `--session` 多会话、`.cppchain/` 忽略规则、memory 落盘 | C | 小 |
| **E** | 教生成器产出这些节点（Q4 放开） | C + 生成端评测方法 | 生成质量与 IR 表达力互相纠缠 |

---

## 9. DoD（草案）

- [ ] `NodeKind::{LoadHistory, SaveHistory, Agent}` + IR 解析 + 校验规则（组件白名单对标 VN6）
- [ ] `compile_workflow` 把三种节点映射到图节点（agent 复用 `as_agent_node`）
- [ ] 组件注册表（model / tool / agent / memory），启动时静态注册
- [ ] `--workdir` 经 `@config.workspace` 注入，并被 B2 的 config 契约校验
- [ ] CLI：`--workdir` / `--session` / `--reset` / `--show` / `--workflow`
- [ ] `<workspace>/.cppchain/` 不被误提交
- [ ] 离线单测：组件白名单、三种节点编译、load→agent→save 的链路、跨 workdir 隔离
- [ ] 真机：同一 workdir 两次独立进程，第二轮答出只有第一轮才知道的信息
- [ ] IR 语法文档（`主动智能/11-ir-syntax.md`）与生成器提示词同步

---

## 10. 开放点

1. **并发写**（Q5）：两个终端同时跑同一 workdir 会互相覆盖。`flock` 或 append-only + 合并
   都可行，但要先想清楚"合并"的语义。
2. **历史体积**：消息列表会随轮数增长。`TrimHistory` 只压缩"发往模型的视图"，
   落盘仍是全量 —— 需要"落盘也保留最近 N 轮 + 摘要"时另行设计。
3. **history 与 memory 的分界**：history = 这次会话的消息（随 workdir）；
   memory = 跨会话的事实（`Store`，建议全局）。把长期事实塞进 history 会导致
   "换个目录就失忆"。
4. **历史可读性/可编辑性**：若定位成"用户可编辑的会话文件"，格式要重新设计
   （现在以程序读写为主）。
5. **组件热插拔**：动态加载留作开放点，见 §4.3 的理由。
6. **与 M10 的关系**：Streaming/Interrupt 会改变"一轮"的边界，可能影响 `save_history` 的落盘时机（Q9）。
