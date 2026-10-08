# 16 · loop 节点与迭代分组（wfr-㉚）

> 把循环从「`label` + `goto` 两个分离原语」换成**一个容器节点**：`loop` 自带 body 子树，
> 调试台按迭代（loop1 / loop2 / loop3）分组管理日志。`label`/`goto` 已删除。

## 1. 为什么换掉 label + goto

旧机制的形状（chat 的 ReAct 循环）：

```
recall → L(label,next:think) → think → echo_think → route → act → echo_tool → loop(goto L, max 4)
```

`label` **不是图节点**（编译期被替换成"其后第一个非 label"），`goto` **是**图节点
（读 `loop_counts[gotoId]` 计数、`Command.goto_nodes` 做有界回跳）。三个具体问题：

1. **循环体没有边界**：`L ... loop` 只是"两个节点 + 一条回边"，图上看不出哪儿是循环体。
   调试台的"循环分组"绑死在**回边的 from 节点**（`debug_view.cpp` 原先那行
   `edges.filter(e=>!e.forward).map(e=>e.from)`），换一条回边就换一个分组，语义靠猜。
2. **节点归属无处安放**：日志 schema 与 debug payload 里没有任何嵌套/父子维度；循环
   迭代内的节点在日志树里与循环外的节点混在同一层。
3. **未爆的语义缝**：validator 的 `resolve_target`/`resolve_labels` **忽略** label 的显式
   `next`，而 compiler 的 `cont`/`e_label` **优先用它** —— 只是当时所有可读 IR 里两者恰好
   相等才没触发。删掉 label，这个缝随之一并消失。

## 2. 核心设计

**loop 是真实图节点，自带 body 子树；"计数 + 分支"由节点头用 `Command.goto_nodes` 完成**
（与 `c_goto` 同一手法：只注册节点、不加静态边，路由全由 `goto_nodes` 给）。

```jsonc
{ "id":"loop", "kind":"loop", "max":4, "next":"_turn_end",
  "body":[ think, echo_think, route, act, echo_tool ] }   // echo_tool.next = "_continue"
```

三条支撑性判断（都已核实）：

- **不需要内部回边节点**：loop 节点头返回 `goto_nodes`，而 `graph` 里 `goto_nodes` 的
  路由优先级最高（`graph.cpp`）。
- **`NodeType::edges()` 不需要作用域**：它只吃一个节点，所以回边判定可以纯粹看指针值
  （`target == "_continue"` → `forward=false`）。加一个共用助手 `edge_to()`，让
  `e_tool`/`e_expr`/`e_route`/`e_next` 都走它 —— **VG2（前向无环）的判据因此一行不用改**。
  作用域只用在"解析"上：validator、编译期的 `ctx.resolve`、debug_view 三处。
- **不 mangle 节点 id**（全图唯一，不做 `loop__id` 前缀）：调试台的图、会话日志、
  `state.results` / `loop_counts` 的键都按 id 对齐，加前缀会让"图上看到的 id"与"日志里的
  id"分叉。代价是不同 loop 里的同名节点要自己取名 —— 只允许一层 loop，代价可控。

**IR 保留嵌套**（`NodeIR::body`），不引入第二份 IR；"展开"只是编译期的一次递归遍历。
共享遍历助手在 `include/cppchain/workflow/ir_walk.hpp`：
`for_each_node(ir, fn)`（带 `LoopScope` 作用域栈）、`flatten_index(ir)`（前序扁平序号，
**loop-free 时与旧数组下标逐字节相同**，所以换掉"数组下标"不改变既有行为）、
`loop_owner` / `loop_scopes` / `edge_is_backward`。

## 3. 精确语义

**loop 节点头（先判后加）**：

```
cur = state.loop_counts[loop_id]（缺省 0）
cur >= max → goto on_max（缺省 _turn_end）
否则        → update{loop_counts:{loop_id: cur+1}}; goto body[0]
```

| max=4，body 内退出条件恒不满足 | 旧 `goto max=4` | 新 `loop max=4` |
|---|---|---|
| body 执行次数 | 4 | 4 |
| 计数终值 | `loop_counts[goto]==4` | `loop_counts[loop]==4` |
| 迭代号可见性 | 无 | body 第 i 轮时 `loop_counts[loop]==i` ← 调试分组依据 |
| 超限落点 | `kEnd` | `on_max`（缺省 `_turn_end` → kEnd） |

**行为等价**（body 次数、终止方式、计数终值一致），唯一差异是多 1 个节点头 super-step。

### 两个出口必须分开（next / on_max）

初版让 `_break` 与"跑满 max"共用一个 `loop.next`，迁移 `loop_wf` 时暴露了问题：
轮询场景里两者的结局**不同** —— 查到了才通知（`_break` → notify），查满 3 次仍没货
**不该**通知（旧 goto 超限是跳 kEnd）。于是拆开：

| 字段 | 含义 | 缺省 |
|---|---|---|
| `next` | 循环的**正常出口**（`_break` 去这里） | 必填 |
| `on_max` | **跑满 max 仍没 break** 时的落点 | `_turn_end`（= 旧 goto 超限跳 kEnd） |

缺省值让三个 chat 类 workflow 无需写 `on_max` 就与旧行为逐字一致。

### 保留名

| 名 | 语义 |
|---|---|
| `_continue` | 下一轮（回 loop 节点头；**算回边**，forward=false） |
| `_break` | 跳出循环（去 loop 的 `next`） |
| `_turn_end` | **仍结束整个 workflow**，不是结束本迭代（chat 的 `route.text` 依赖它） |
| `_fail` | 以错误终止（不变） |

### `$_` / `_last_exec`

**语义不变**：仍只有 tool 节点写 `_last_exec`，所以 `$_` = 最近跑过的 tool 输出，在循环里
跨迭代延续（每轮 `merge_slots` 右胜左覆盖）。

## 4. 校验规则

| 规则 | 内容 |
|---|---|
| VN15 | loop：`max>0`、`body` 非空、`next` 必填且不得是保留名、`on_max` 不得是保留名 |
| VG10 | `_continue`/`_break` 只能出现在 loop body 里；body 节点的指针不得指向 body 外的普通节点（多半是漏写 `_break`） |
| VG11 | body 里不得再含 loop（**不支持嵌套**） |
| VG13 | `load_history` **不得**放进 body（每轮 replay 都会把整段历史再追加一遍，messages 会随迭代平方级膨胀；沿用 VG9 的"硬拒绝"风格） |
| VG9（改） | 改成**按作用域**判定：顶层 + 每个 body 各算一处。作用域里"有消费者但没有 load_history"**不报** —— 历史可以在祖先作用域加载（chat 正是 recall 在 loop 之前） |

## 5. 调试台：分组维度从"回边"改成"loop 节点"

### 5.1 日志树：按**执行顺序**的真实层级

日志树不是"扁平 + 筛选项"，而是照执行顺序长出来的层级：

```
▾ [run] session-s1/r-1（60 条 · 24 节点）
  · __input__（2 条）          ← 在 loop 之上（执行序在前）
  · recall（3 条）
  ▾ ↺ loop（4 轮 · 55 条）     ← 容器名 = **真实的 loop 节点 id**
    ▾ loop/1（15 条）          ← 每一轮一个子组
      · loop（2 条）           ← 循环节点头自己的记录（enter/exit）也在本轮里
      · think（3 条）
      · echo_think（3 条）
      · route（2 条）
      · act（2 条）
      · echo_tool（3 条）
    ▾ loop/2（15 条）
    …
```

- **顶层按执行顺序**：每个"实体"（普通节点 / loop 容器）在**首次出现**时占位，所以
  `__input__`、`recall` 排在 loop 之上，循环之后的节点排在 loop 之下（不再有"循环外"兜底组
  被排到最后）。
- **循环体与各轮迭代都在 loop 之下**：`loop/1`、`loop/2` … `loop/N` 是容器的子组，
  循环体节点再挂在各自那轮下面。
- `workflow` 那一层**只在一次 run 里出现多个 workflow 时**才渲染（import 子链才需要）。
- 摘要都带条数：容器 `↺ loop（4 轮 · 55 条）`、迭代 `loop/1（15 条）`、节点 `think（3 条）`。

- `workflow` 那一层**只在一次 run 里出现多个 workflow 时**才渲染（import 子链才需要），
  单链场景不再多一层噪音。
- 分组键 `loop_id` 与迭代号 `iter` 由 **C++ 侧**（`debug_view_logs`）算好写进每条记录，
  前端只做分组 —— 因为 **loop 节点头的 enter 发生在自增之前**，只有拿着完整序列的
  C++ 侧知道它该归到"将要跑的那一轮"（取"此后第一次自增后的值"）；body 节点取本条
  之后快照里的计数即可。没有归属的记录（`__input__`、`recall`、loop 之后的节点）
  归到 `（循环外）`。
- 组的排序：按（loop id, 迭代号）升序，`（循环外）` 永远排最后。
- 摘要行带条数：`↺ loop:1（15 条）`，run 行仍是 `总条数 · 节点数`。

原有的"循环分组"页签保留（点 `1`/`2` 把整棵树筛到某一轮），与树内分组互补。

- `debug_view_graph` 递归导出：body 里的节点多带一个 **`loop` 归属字段**（复用 import
  `origin_wf` 那套"节点级扁平归属"）；loop 自身与 loop 之后的节点不带。
- 边就地解析成图目标：`_continue` → loop 节点头且 **forward=false**（前端据此画绕行
  弧线）、`_break` → loop 的 next（前向）。
- 前端分组维度改成 `G.nodes.filter(n => n.kind==='loop')`；`loop_counts` 的键此时就是
  **loop 节点 id**，迭代号直接可读。过滤再收紧一层：只有该 loop body 里的节点才属于
  这次迭代，免得 loop 之后的节点也落进"第 max 轮"。
- 图仍**平铺**（不做容器框）：loop 是普通节点 + 一条回边弧线。

实测 chat 一轮（真机）：

```
节点:  recall(load_history) · loop(loop) · think(model,loop=loop) · echo_think(echo,loop=loop)
       · route(route,loop=loop) · act(run_tools,loop=loop) · echo_tool(echo,loop=loop)
回边:  echo_tool → loop
日志:  iter1 = [loop, think, echo_think, route, act, echo_tool]
       iter2 = [loop, think, echo_think, route]
```

## 6. 开放点

1. **不支持嵌套循环**（VG11 硬拒）。要支持的话调试分组要变成 `loop1.loop2` 的路径形式。
2. **不做 for-each**：body 内取不到"当前遍历项"（`loop_wf` 靠内置的 `item_id` 模拟）。
3. **VG13 收窄了"body 里任意节点类型"**：若实测需要把 load_history 放进循环体，放开即可
   （代价是 messages 膨胀）。
4. **`on_max` 缺省是"静默收工"**：跑满上界不算错误。若某类 workflow 需要"跑满即失败"，
   可显式 `on_max: "_fail"`。
5. **计数随快照持久化**：checkpoint 续跑会从上次计数继续（与旧 goto 相同）。

## 7. 与其他文档的关系

| 想看 | 去读 |
|---|---|
| 迁移过程与验收数据 | [04-problems/15](../04-problems/15-loop节点落地与label-goto迁移记录.md) |
| IR 语法规范（规范来源） | `主动智能/11-ir-syntax.md` |
| 节点接口/注册表/edges 驱动 | [12](12-节点接口与注册表.md) |
| wfr 命令行与调试台 | [14](14-wfr命令行工具与调试可视化设计.md) |
| 循环的思想来源（agent 循环 = 图上的回边） | [11](11-组件化与命令行agent.md) |
