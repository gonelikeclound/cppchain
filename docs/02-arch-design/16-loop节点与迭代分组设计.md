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
  回边）、`_break` → loop 的 next（前向）。
- 前端分组维度改成 `G.nodes.filter(n => n.kind==='loop')`；`loop_counts` 的键此时就是
  **loop 节点 id**，迭代号直接可读。过滤再收紧一层：只有该 loop body 里的节点才属于
  这次迭代，免得 loop 之后的节点也落进"第 max 轮"。
- 图仍**平铺**（不做容器框）：loop 是普通节点 + 一条回边。边用**正交圆角折线**画（见 §5.4）。

实测 chat 一轮（真机）：

```
节点:  recall(load_history) · loop(loop) · think(model,loop=loop) · echo_think(echo,loop=loop)
       · route(route,loop=loop) · act(run_tools,loop=loop) · echo_tool(echo,loop=loop)
回边:  echo_tool → loop
日志:  iter1 = [loop, think, echo_think, route, act, echo_tool]
       iter2 = [loop, think, echo_think, route]
```

### 5.2 图上标出本轮**实际走过**的边

静态图把 IR 里**所有**边都画出来，看不出这一轮到底走了哪条 —— 调 ReAct 循环时最想知道的
就是"这轮回边走了几次、最后是从哪儿收尾的"。现在：

- **数据**：`debug_view_logs` 为每个 run 附一份 `edges: [{from,to,count}]`，来源是引擎的
  `enter` 序列（相邻两个执行节点 = 走了一条边）。终端边（`_turn_end`/`_fail`）不产生
  enter 记录，用两个只在"明确"时才采信的来源补：① 最后一个执行节点的 exit update 里
  发布了 `target`（route 就是这么发布的）；② 该节点在图上只有**一条** terminal 出边
  且本轮最后一条记录是成功 exit。两条都不满足 → 不猜。
- **呈现**：走过的边加粗变蓝并标 `×N`（次数），未走过的保持灰/红细线；图例末尾有一行说明。
- **联动**：切换"会话 / 轮次"时只更新边的类与角标（不重建 SVG），所以节点选中态与布局
  都不受影响。

实测（真机 chat 一轮）：

```
__input__ → recall ×1     recall → loop ×1      loop → think ×2
think → echo_think ×2     echo_think → route ×2 route → act ×1
act → echo_tool ×1        echo_tool → loop ×1   route → _turn_end ×1
```

`echo_tool → loop ×1` 就是那一轮的回边；`route → _turn_end ×1` 是收尾边（靠 route 发布的
target 补上的）。

### 5.3 页面自取数据：`--graph-only` + 拖入日志（wfr-㉟）

原先会话必须在**生成网页时**一起内嵌 —— 想看另一个会话、或刚跑出来的轮次，就得重新生成。
现在：

- `wfr debug <wf> --graph-only` 只按 workflow 出页面（payload 里 `records` 为空），
  打开后把 `.wfr/sessions/*.log.jsonl` **拖进左栏**（或点击选择，可反复追加）即可；
  也可以**填一个日志 URL** 让页面直接去读（同一个 `ingestTexts`，会话名从 URL 末段推）。
  ⚠️ URL 这条受浏览器同源策略限制：`file://` 打开的页面发跨源 `fetch` 会被按 CORS 拦掉
  （file 源是 `null`）—— 所以失败时提示里会明确写出这一条，而不是静默吞掉；日志放在同源
  http 服务下时才走得通。
- 不传 `--graph-only` 时行为不变（默认 `--all`，把会话内嵌进页面）—— 静态单文件那条路仍在。

**关键决定：日志的推导搬到页面里的 JS，且是唯一实现。**

要推导的四件事（① 逐条 `loop_counts` 快照 ② 每条记录的 `loop_id`/`iter` ③ 本轮走过的边与
次数 ④ 按 run 分组）原先在 C++ 的 `debug_view_logs`（150 行）。但"拖入日志"这条路径**必须
在浏览器里推导**，于是只有两个选择：写第二份 JS（口径会分叉），或把 C++ 那份搬过去。

选了后者：**JS 是唯一实现**，C++ 侧只剩 `debug_view_graph`（IR → 图，结构性、必须留在
C++）与日志读取。于是静态导出与拖入**共用同一份推导**，没有第二份口径；副作用是那 4 个
gtest 用例要挪到 node 里（`tests/js/debug_derive_test.mjs`，ctest 里找得到 node 才注册）。

为什么不上 wasm（也评估过）：本机没有 emscripten/wasm-ld；`debug_view.cpp` 经
`node_type.hpp` 依赖整个节点注册表；要把闭包瘦下来得先重构；而它唯一不可替代的优势
（单一实现）用"整体搬到 JS"能更彻底地拿到。逻辑本身只有 ~60 行簿记，没有算法/精度问题。

拖入时的约定（与 `--all` 采集同一套）：文件名 `session-<名>.log.jsonl` → 会话名，
`run` 加会话前缀，于是多会话并排可选；同一会话多次拖入按 `seq` 归并。

### 5.4 边的画法：正交圆角折线，且不压节点

原先是从中心到中心的三次贝塞尔（回边绕左侧大弧）。问题是：曲线会**压过别的节点**，
而且"从中心出发"意味着线段一开始就在框里。现在改成**正交圆角折线**，走线只用两类位置：

| 位置 | 用途 | 为什么安全 |
|---|---|---|
| **行间隙带**（两行之间 y∈[行底, 行底+76] 的中线） | 全部横向段 | 行带里才有节点框，间隙带里没有 |
| **两侧通道**（所有节点框左右各外扩 26px） | 跨多层的纵向段 | 通道在所有框之外 |

于是"线与节点不重叠"是一条**可证的性质**，而不是调参凑的：

- 相邻层：下 → 横（间隙中线）→ 下，三个拐角都倒圆角（`Q` 二次贝塞尔，半径按相邻段长
  的一半夹住）；同层：绕本行**下方**的间隙（不横穿本行的节点）。
- 跨多层（只可能出现在**终端边**与**回边**上 —— 前向边的层差恒 ≤1，因为层是 BFS 最小
  距离）：走右侧通道（前向）/ 左侧通道（回边），纵向段在那条通道里跨过中间各行。
- 共线拐点会被去掉：直上直下的边就是一条 `M… L…`，不再有多余命令。

页面上有测试守着这条性质：夹具刻意构造出**全部五种走线**（相邻层 / 同层 / 跨层前向到
终端 / 单层回边 / 跨层回边），把每条边的折线重建出来逐段与所有节点框求交，断言**零穿越**，
并断言路径里有 `Q`（圆角）而没有 `C`（曲线）。真机 chat 图上另跑一遍：13 条边、0 穿越。

### 5.5 连线上的文字也不能重叠

文字原先取"最长那段的中点"，多条边的横向段落在**同一条行间隙**里时中点常常挨得很近，
于是叠在一起。现在把标签也纳入 §5.4 的规矩并统一避让：

- **锚点优先落在横向段的中点** —— 横向段都在行间隙里，于是文字天然不压节点，也便于按
  "带"分组。整条都是竖线（直上直下的边，源与目标同列）时才贴到线的右侧。
- 然后跑一遍 `placeLabels`：按 y 把锚点分到各条行间隙**带**，带内用轨道错开
  （0 / ±22 —— 间隙高 76、标签半高 9+余量 2，恰好三层都落在间隙内），同一轨道再撞就沿
  x 往右推。**贴竖线的标签也走同一套避让** —— 它同样会与别人撞（见下面的坑）。
- 渲染因此改成**两趟**：先算折线与锚点 → 统一避让 → 再建元素。逐个画必然重叠，因为
  同一条带里的标签互相不知道。

坑：一开始"贴竖线右侧"的标签被我从避让里排除了（想着它待在空处），结果真机 chat 上
`cases.tool_calls`（源与目标同列 → 走竖线分支）与 `cases.text` 直接叠在一起。

测试：`debug_derive_test.mjs` 用例 10 造出"多条边的横向段挤在同一条间隙"的图
（一个节点同时连向同层三个节点），断言**两两不重叠**且**不压节点**；真机 chat 上另有
一道检查（6 个标签、0 重叠、0 压节点）。

### 5.6 引出/接入点按走向选"最近的面"

原先所有边都从**下边中点**引出、从**上边中点**接入（回边则相反）。但线不一定主要往上下走：
`loop_wf` 的 `poll→ck` 是"横差 158、层差 1"——线主要**横向**走，却从下边钻出来，看着别扭。

现在按主要走向选面：

| 情形 | 引出 | 接入 | 走线 |
|---|---|---|---|
| 横向位移 > 纵向位移（同层，或跨层但差了一列以上） | 源**朝向目标那一侧的中点** | 目标**朝向来源那一侧的中点** | 出侧面 → 进**列间隙通道** → 通道内直上/直下 → 进目标侧面 |
| 纵向为主（同列跨层、回边等） | 下边中点（回边为下…见上） | 上边中点 | 仍走 §5.4 的行间隙 + 两侧通道 |

**列间隙通道**是这套走线的关键：这个布局每一行共用同一套 x 网格，所以"列与列之间那条
20px 的缝"（中心在 `x+W+GAP/2`）在**所有行**里都是空的 —— 纵向沿它跑多远都不会压到节点。
相邻列时就是"出侧面 → 缝里直下 → 进对面侧面"三段；跨列时中间那段横向退回到行间隙带里走。

实测（loop_wf）：`loop→poll` 右中点→左中点、`poll→ck` 左中点→右中点、其余纵向边仍是
下/上中点；13/8 条边全部零穿越。

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
