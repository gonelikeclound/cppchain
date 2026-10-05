# 11 · 组件化与命令行 Agent（拟定 M12）

> 设想：把 **agent / history / memory / tool** 都做成**可按名字引用的组件**，
> 用 JSON 定义 **agent 节点**、由 workflow 串联节点，产出**命令行 agent 工具**。
> 关键诉求：指定 agent 的**工作路径**，从工作路径提取历史消息 → **跨命令连续对话**。
>
> 状态：**设想 + 一处已实证**。其中"落盘续跑"已完成架构验证 A（§5，含真机两进程实测），
> 其余为设计。本文只定形状与取舍，不含实现。

---

## 1. 定位与边界

- **依赖**（全部已就绪，无需新底层）：

| 依赖 | 提供的东西 |
|---|---|
| M5 `state` | `Checkpointer`/`StateSnapshot`、`Store`、`StateSchema`+reducer |
| M6 `graph` | `StateGraph` → `CompiledGraph`、`Command`、条件边、每步快照、**从快照恢复续跑** |
| M7 `agent` | `Agent` 循环、`Middleware` 四钩子、`thread_id` + checkpointer 续跑 |
| M8 `graph_agent` | `as_agent_node`（把 `Agent` 包成图节点，已有） |
| M11 `workflow` | IR（JSON）→ 校验 → 编译成图 → `fire_once`；生成端 |

- **提供**（本文要定的形状）：
  ① 统一的**组件注册表**（按名字引用，编译期/校验期 fail-closed）；
  ② **agent 节点进 IR**（第 5 种节点类型）；
  ③ **history 组件**：把会话历史落到工作路径，支持跨进程续跑；
  ④ **命令行工具**形态（`--workdir` / `--session` / 单次与 REPL）。

- **不做**（押后）：

| 不做 | 归属 |
|---|---|
| M10 的 Streaming / Interrupt / Send（运行时补全） | 先于本设计，或并行 |
| 组件的**热插拔/动态加载**（.so 插件） | 开放点 |
| 多进程**并发写**同一工作路径的历史（文件锁/合并） | 开放点（§6 Q5） |
| 多模态历史、跨设备同步 | 不做 |

---

## 2. 现状盘点：家底 vs 差距

设想的四类组件里，**三类已有实现但缺"按名字引用"这层**；history 的落盘后端是唯一真缺口
（且已在验证 A 中补上）。

| 设想 | 现状 | 差距 |
|---|---|---|
| tool 组件 | ✅ `ToolRegistry` + `ToolSpec`（已是 JSON Schema） | 无 |
| agent 组件 | ⚠️ `agent::Agent` 只能用 C++ 构造 | 缺**注册表**（按名字引用） |
| memory 组件 | ⚠️ `state::Store` 只有 `MemoryStore` | 缺落盘后端 |
| **history 组件** | ⚠️ `state::Checkpointer` 只有内存实现 | **缺落盘后端 → 验证 A 已补（§5）** |
| JSON 定义 agent 节点 | ❌ `NodeKind { Tool, Expr, Label, Goto }` | 缺第 5 种节点类型 |
| workflow 串节点 | ✅ IR → 图，已能串 tool/expr/label/goto | — |
| agent 作为节点 | ⚠️ `as_agent_node` 只是编译期 C++ 适配器 | 未进 IR |
| 命令行工具 | ✅ `agent_cli`（4 策略）、`proactive`（workflow + 菜单澄清）、`session_resume`（§5） | 可作底座 |

---

## 3. 设计

### 3.0 首要原则：**只有一层历史**

这是整个设计里最容易做错、也最需要先定的一点。

- **workflow 的语义是"每次触发全新状态"**（M11 的 D1：`fire_once` 每次 `Json input = {{config…}}`）
- **对话 agent 的语义是"累积历史"**

两者直接冲突。项目里已经有答案的雏形 —— M8 的子代理明确要求
`options.thread_id 必须为空 → invoke 不碰 checkpointer，干净`，即：

> **agent 节点自己不持有历史；历史是 workflow 的 state。**
> agent 节点只做"读本节点的输入切片 → 跑 → 写回"（`AgentNodeConfig` 的
> `input` resolver + `output_key`/`append_to_messages` 就是为此设计的）。

若接受这条，架构自洽：**只有一层历史（workflow state）、一个落盘点（Checkpointer）、
history 组件只是它的读取视图**。反之若让每个 agent 节点各带一份 history，就会出现
"两个真相源"，恢复时不知道信谁——这是必须避免的。

### 3.1 组件化：注册表 + JSON 按名引用

**形状**：宿主（CLI）在启动时把 C++ 对象注册进一张表；IR 里按**名字**引用。

```
组件种类（初版 4 类）：model / tool / agent / memory
  其中 tool 已有 ToolRegistry；新增 agent 组件表（名字 → 构造配方）
```

IR 里的引用形如：

```jsonc
{ "id": "researcher", "kind": "agent",
  "component": "explorer",          // ← 组件名，必须在注册表里
  "input": {"from": "messages"},    // 输入切片
  "output_key": "findings",         // 写回哪个槽（或 append_to_messages）
  "next": "summarize" }
```

**白名单纪律直接复用 M11 的做法**：生成器提示词里的组件目录 == 注册表，
validator 加一条规则（对标 `VN6` 对工具的做法）——**未知组件名 = 校验期拒绝**，
而不是运行期才发现。这样"组件不存在"和"工具不存在"走同一条纪律，不引入第二套心智。

**不做热插拔**：组件在启动时静态注册（`main` 里几十行），不是 `.so` 动态加载。
理由：编译期类型安全 + 避免 ABI 问题；本项目是学习/研究性质，动态加载收益不抵复杂度。

### 3.2 agent 节点进 IR

`NodeKind` 增加 `Agent`，`NodeIR` 增加对应字段（`component` / `input` / `output_key` /
`append_to_messages` / `system_prompt` 覆盖）。编译期映射：

```
Agent 节点  →  graph::as_agent_node(agent, cfg)   ← M8 已有，无需新写
```

**这是本设计里 IR 面最大的一次扩张**，牵动三处，必须一起改否则不一致：

| 要改 | 内容 |
|---|---|
| `主动智能/11-ir-syntax.md` | §3 节点类型 + §7 链约束（agent 节点也要有 `next`） |
| `workflow/ir.cpp` 的 `classify` | 判定优先级插入 `kind:"agent"`（与 label/goto 同级显式键） |
| 生成器 | 提示词里的节点写法 + 工具目录之外再给**组件目录** |

**注意生成端的连锁**：M11 的生成器目前只会产出 tool/expr/label/goto。加了 agent 节点后，
"什么意图该生成 agent 节点"变成一个**新的生成策略问题**——初版建议**不教生成器用 agent 节点**
（只让手写 IR 能用），避免把生成质量问题和 IR 表达力问题搅在一起。等手写路径跑顺了再放开。

### 3.3 history 组件 = 落盘的 workflow state（验证 A 的结论）

**定义**：history 组件不是独立存储，而是"`Checkpointer` 按工作路径落盘"这一件事的**视图**。
`--show` 读的就是 `state.messages`。

**文件布局**（验证 A 采用，实测可行）：

```
<workdir>/.cppchain/history.json      # 快照链（values/next/config/metadata/parent）
```

- **按工作路径隔离**：换 workdir 即换会话（实测：另一个 workdir 读到 0 条）✓
- **落盘单位是快照链**，不是消息日志 → 因此不仅"续聊"，还能**续跑图**（`next` 字段保留了
  待执行节点，"回到该步边界继续"）
- **原子写**：临时文件 → `rename`（照搬 `agent::FileJournal`）
- **fail-closed**：文件损坏时**抛异常**，不静默以空历史启动 —— 否则等于悄悄丢掉用户的会话历史

**与 memory 组件的分界**：history 是"这次会话的消息"（随 workdir，生命周期与对话一致）；
memory 是"跨会话的事实"（`Store`，建议**全局或按用户命名空间**，不随 workdir）。
两者不要混：把长期事实塞进 history 会导致换个目录就"失忆"。

### 3.4 命令行工具形态

```
cppchain-cli [--workdir DIR] [--session NAME] [--reset] [--show] "<意图>"
  --workdir   工作路径（缺省 cwd）→ 决定 history 落点
  --session   同一 workdir 下多段会话（缺省 "cli"）；即 Checkpointer 的 thread_id
  --show      不起模型，只打印已存历史（离线可用，验证 A 已实现）
  --reset     清空该 (workdir, session) 的历史
  无 --show    走 workflow：PlanStage → IRStage（如需）→ compile → fire_once
```

两条使用路径，**建议先做第一条**：

1. **手写 IR 路径**（推荐起步）：`--workflow path/to/wf.json` 直接跑一份现成 IR，
   agent 节点由人手写。可控、可测，且立刻能验证"组件化 + 跨命令续跑"这个组合。
2. **生成路径**（后置）：`"<意图>"` 交给 M11 生成器产出 IR 再跑。依赖 §3.2 末尾说的
   生成策略问题，风险高得多。

---

## 4. 契约

- **IR 扩展**：`NodeKind::Agent`；新增校验规则（编号待定，建议接在 `VN` 段）：
  · 组件名必须在注册表里（对标 VN6）
  · `input`/`output_key` 的形状合法；`output_key` 若被下游 `$` 引用，需能被 `output_schema` 式声明覆盖
  · agent 节点同样必须有 `next`（单链约束不变）
- **文件布局**：`<workdir>/.cppchain/history.json`；建议加进 `.gitignore` 或由工具自动写
  `.cppchain/.gitignore`（避免用户把会话历史误提交）
- **落盘格式**：`{version, counter, snapshots{key→[…]}, writes{key→[…]}}`。
  ⚠️ **`std::optional` 字段必须显式写 string 或 null** —— nlohmann 对 optional 没有内建支持，
  直接塞会走"数组构造路径"（`parent_checkpoint_id` 曾因此在文件往返中全部丢失，见 §5.3）
- **失败语义**：历史文件损坏 → 默认抛异常；显式 `OnCorrupt::StartEmpty` 才允许从空开始

---

## 5. 已验证：架构验证 A（落盘续跑）

目的是在写设计之前**先证伪最大的风险**：持久化语义到底能不能支撑"跨命令连续对话"。
结论：**成立**。

### 5.1 实现（最小改动）

- `MemoryCheckpointer` 加 `dump()/load()`（对标 `MemoryJournal` 已有的同名方法）
- `FileCheckpointer`：内嵌 `MemoryCheckpointer` 承载数据结构，构造时载入、每次变更后原子落盘
- **`Agent` 侧一行没改** —— `Agent::invoke` 本来就会按 `thread_id` 从 checkpointer 恢复
  `messages`。换的只是 Checkpointer 实现，这正是"分层可插拔"的验证点

### 5.2 真机两进程实测（`examples/session_resume`，同一 workdir）

```
进程 1：恢复 0 条 → 发往模型 2 条 → 落盘（磁盘留 3 条）
        "我叫东俊，正在用 C++26 复刻 langchain，项目叫 cppchain。请记住。"
── 全新进程 ──
进程 2：恢复 3 条 ← 上一次进程留下的
        发往模型 4 条
        "我叫什么？我在做什么项目？" → "你叫东俊，正在用 C++26 复刻 langchain，项目叫 cppchain。"
换 workdir → 0 条（隔离成立）
```

`--show` 可**完全不起模型**地打印磁盘上的历史 —— 即"从工作路径提取历史消息"。

### 5.3 过程中查出的真 bug（值得记下来）

落盘文件里出现 `"parent_checkpoint_id": []`。追下去发现 **engaged 的 parent 也是 `[]`** →
加断言 → 测试失败：**nlohmann 对 `std::optional` 没有内建支持，直接塞会走"数组构造路径"**
（`message/serializer.hpp` 早有此警告，但状态层没有那句话）——nullopt 与 engaged 都变成 `[]`，
回读按 `is_string()` 守卫就全丢，**快照链的 parent 跨文件往返后全部消失**。

> 教训：这个 bug 是在**测试全绿**的状态下发现的 —— 原用例只在同进程内断言了 parent，
> 恰好绕过文件往返。**是去看真实落盘文件、而不是相信测试绿灯，才抓到它。**

---

## 6. 待定问题（需拍板）

| # | 问题 | 选项 | 倾向 | 影响面 |
|---|---|---|---|---|
| Q1 | history 的粒度 | 每 workflow 一份 / 每 agent 节点一份 | **一份**（§3.0） | 决定整个状态模型 |
| Q2 | 工作路径怎么定 | cwd 隐式 / `--workdir` 显式 | **显式优先、缺省 cwd** | 影响"目录即会话"的直觉是否成立 |
| Q3 | 落盘形态 | 快照链 / 消息日志 | **快照链**（验证 A 已跑通） | 决定能否"续跑图" |
| Q4 | 生成器要不要产出 agent 节点 | 要 / 初版不要 | **初版不要**（§3.2 末） | 决定生成质量的归因清晰度 |
| Q5 | 并发写同一 workdir | 不支持 / 文件锁 / 合并 | **先不支持**，检测到 `.tmp` 冲突即报错 | 多终端同时用会踩 |
| Q6 | memory 组件的作用域 | 随 workdir / 全局 | **全局或按用户**（§3.3 末） | 影响"长期记忆"的语义 |

---

## 7. 分阶段实施（建议）

| 阶段 | 内容 | 依赖 | 风险 |
|---|---|---|---|
| **A** ✅ | `FileCheckpointer` + 跨进程续跑验证 | 无 | 已完成 |
| **B** ✅ | 本文（形状与取舍） | A | — |
| **C** | 组件注册表 + `NodeKind::Agent` + 校验规则 + 手写 IR 的 CLI 路径 | B | IR 面扩张，需同步 IR 语法文档与生成器提示词 |
| **D** | `--session` 多会话、`.cppchain/` 忽略规则、`--show` 并入 CLI | C | 小 |
| **E** | 教生成器产出 agent 节点（Q4 放开） | C + 生成端评测方法 | 生成质量与 IR 表达力互相纠缠 |

---

## 8. DoD（草案）

- [ ] `NodeKind::Agent` + IR 解析 + 校验规则（未知组件名拒绝，对标 VN6）
- [ ] `compile_workflow` 把 agent 节点映射到 `as_agent_node`
- [ ] 组件注册表（model/tool/agent/memory 四类，启动时静态注册）
- [ ] CLI：`--workdir` / `--session` / `--reset` / `--show` + 手写 IR 路径
- [ ] `<workdir>/.cppchain/` 不被误提交（自带 `.gitignore` 或文档明示）
- [ ] 离线单测：组件名白名单、agent 节点编译、跨 workdir 隔离
- [ ] 真机：同一 workdir 两次独立进程，第二轮答出只有第一轮才知道的信息
- [ ] IR 语法文档（`主动智能/11-ir-syntax.md`）与生成器提示词同步

---

## 9. 开放点

1. **并发写**（Q5）：两个终端同时跑同一 workdir 会互相覆盖。文件锁（`flock`）或
   append-only 日志 + 合并都可行，但要先想清楚"合并"的语义。
2. **历史体积**：快照链每轮全量存 `values`（含全部 messages）→ 长会话下文件线性增长。
   `TrimHistory` 中间件已经在压缩发往模型的视图，但**落盘的是全量**。
   需要"落盘也压缩"（保留最近 N 轮 + 摘要）时另行设计。
3. **历史可读性**：现格式以快照为中心，人能看懂但不够友好（`snapshots["cli|"]` 这种复合键）。
   若定位成"用户可编辑的会话文件"，格式要重新设计。
4. **组件热插拔**：动态加载（`.so`）留作开放点，见 §3.1 的理由。
5. **与 M10 的关系**：Streaming/Interrupt 会改变"一轮"的边界，可能影响落盘时机。
