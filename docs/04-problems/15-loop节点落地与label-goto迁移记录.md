# 15 · loop 节点落地与 label/goto 迁移记录（wfr-㉚）

> 五个提交把循环从 `label` + `goto` 换成 `loop` 容器并删除旧机制。设计见
> [02-arch-design/16](../02-arch-design/16-loop节点与迭代分组设计.md)。

## 一、分步与提交

| 步 | 内容 | 提交 |
|---|---|---|
| ① ② | IR + 遍历助手 + validator/compiler 递归化 | `d6b62e9` |
| ③ | 调试台按 loop 迭代分组 + 宿主扫描递归化 | `647c32f` |
| ④ | 迁移四个生产 workflow + `on_max` + import/生成器递归化 | `ecd7ef4` |
| ⑤ | 删除 label/goto | `269846b` |

**顺序不是随意的**：③（宿主扫描）**必须早于**④（迁移）。`has_model_node` 与
`features.hpp` 的谓词都只扫顶层，迁移后 model/工具都在 loop body 里 —— 漏改是**静默**
错误（`wfr chat` 被误判"无需 LLM key"、eval 谓词全 false），不报错只跑错。

## 二、过程中被测试/真机逼出来的四个问题

### 1. `edge_to()` 读到了被搬空的字符串（①，测试抓的）

```cpp
// 错：聚合初始化的求值顺序是左→右，std::move(target) 先把 target 搬空，
// 再拿它算 forward 就是拿空串去比 —— 回边判定恒为 false。
NodeType::Edge edge_to(std::string target, std::string label) {
  return {std::move(target), std::move(label), !edge_is_backward(target)};
}
```

`WfLoop.continue_is_the_only_backward_target` 抓到（`forward` 期望 false 实得 true）。
修法：先算 `forward` 再 move。

### 2. VR2/VR3 拿前序序号去索引顶层数组（②）

`Ctx.index` 从"数组下标"换成"前序扁平序号"（为了支持嵌套）后，VR2 里的
`ir.nodes[it->second]` 会取错节点（越界或拿到别的）。改成按 id 取（`by_id`），
VR3 的"更早的 tool"改成扫 `flat`。这条不修的话，`$<id>.result.<f>` 在 loop body 里
会静默查错节点。

### 3. `label`/`goto` 与 `c_echo` 在同一个删除区间里（⑤）

按"从 `c_label` 删到 `c_model`"的区间删代码时，把夹在中间的 `c_echo` 一起删了。
构建立刻报 `c_echo was not declared`，从 git 取回即可。**教训**：删代码用内容块匹配，
别用"起点到终点"的区间。

### 4. `on_max`：`_break` 与"跑满 max"不能共用一个出口（④，迁移 loop_wf 时暴露）

这是**计划里漏掉的一处语义缺口**。原设计 `_break` → `loop.next`，跑满 max 也 → `loop.next`。
但 `loop_wf` 里两者结局不同：

- 查到货了（`ck.then = _break`）→ 通知；
- 查满 3 次仍没货 → **不该通知**（旧 `goto max=3` 超限是跳 kEnd）。

于是拆成 `next`（正常出口 / `_break` 落点）与 `on_max`（跑满落点，缺省 `_turn_end`）。
缺省值让三个 chat 类 workflow 不写 `on_max` 就与旧行为逐字一致。

### 5. `import` 的指针键白名单漏了 `on_max`（⑤）

`is_pointer_key` 决定 import 前缀化时改写哪些字段。`on_max` 也是节点引用，漏了会让
被 import 的 loop 里 `on_max` 指向子链节点时不加前缀 —— 静默断链。

## 三、验收

**离线**：全仓 **517 测试全绿、零告警**。

| 测试 | 覆盖 |
|---|---|
| `tests/wf/loop_test.cpp`（18 例） | 解析/序列化往返（body 保真、坏节点报错带 loop 名）、v_loop 与 VG10/11/13 负例、边的前向/回边判定、遍历助手（前序顺序 / loop_owner / flatten_index）、多轮 `_continue` 跑满 max、`_break` 早退、`on_max` 重定向、计数键是 loop 节点 id |
| `DebugView`（+3 例） | loop 平铺导出 + body 归属字段；边解析（body/next 前向、`_continue` 回边、`_break` 前向）；HTML 里的 kind-loop 配色与分组维度 |
| `WfAgentLoop` | classify 对 `label`/`goto` 两种废弃写法都拒且报错点名 loop |
| `WfIr` | loop 解析 + 废弃写法 parse 期被拒 |
| 各迁移用例 | compiler/agent_loop/history_nodes/debug_view/ir_validator/plugin_import 的 IR 字面量全部换成 loop 形态 |

**迁移期的一条关键测试（已按计划删除）**：同一语义分别用旧 `label`+`goto` 与新 `loop`
构建，跑同一组桩数据，断言缺货/有货两种情形下**工具调用次数与通知次数完全一致**
（缺货 4 次 0 通知；有货 1 次 1 通知）。它在 ④ 时通过，证明等价性；⑤ 删掉旧写法后
不可解析，故移除 —— 它的使命就是"把重新实现循环从看着对变成对得上"。

**真机**：

```
wfr chat  "北京天气怎么样？" → 💭 思考 → 🔧 get_weather → 回答（ReAct 循环经 loop 跑通）
wfr chat2 --set history_mode=keep_evidence "上海呢？" → 历史压缩开关在 loop 形态下仍工作
wfr debug chat --session s1 → 节点 recall/loop/think/echo_think/route/act/echo_tool，
                              唯一回边 echo_tool→loop，日志分 iter1/iter2 两组
```

## 四、迁移后的资产形态

| 文件 | 迁移前 | 迁移后 |
|---|---|---|
| `share/wfr/workflows/chat.json` | 8 节点（含 label L + goto loop） | 2 顶层节点（recall + loop），body 5 个 |
| `share/wfr/workflows/chat2.json` | 9 节点 | 3 顶层（recall + echo_hist + loop） |
| `examples/workflow_cli/wf.json` | 6 节点 | 2 顶层（recall + loop），body 3 个 |
| `examples/wfr_demo/.wfr/workflows/loop_wf.json` | 4 节点（label + poll + ck + goto） | 1 顶层（loop），body 2 个；`next:"n"` + `on_max:"_turn_end"` |

## 四点五、日志树按迭代分组（追加）

原先"循环分组"只是**页签筛选**（点 1/2 过滤），树本身仍是 run → workflow → 节点。
现在树直接按迭代分层：`run → ↺ loop:N → (workflow) → 节点 → 记录`，"（循环外）"收
`__input__`/`recall` 这类不属任何循环的记录。

关键实现点：**迭代号由 C++ 侧算**（`debug_view_logs(records, ir)` 给每条记录写
`loop_id`/`iter`）。原因见下面这条陷阱。

### 陷阱：loop 节点头的 enter 在自增之前

循环的日志形状是

```
第 1 轮： loop enter（此时计数还是空的） → loop exit（计数 = 1） → body 节点……
第 2 轮： loop enter（计数还是 1）      → loop exit（计数 = 2） → body 节点……
```

所以节点头的 **enter 与 exit 跨在自增的两侧**：按"当前快照"贴标签会把 enter 归到上一轮
（第 2 轮的 enter 会被算成第 1 轮）。前端只有每条记录的快照，看不出这个时机。
C++ 侧拿得到完整序列，于是对**节点头**取"此后第一次自增后的值"，enter/exit 便都落在
第 N 轮；body 节点取快照里的计数即可。跑满 max 后的那次节点判定没有自增，回落到快照值
（= max），作为第 max 轮的收尾。

### 陷阱 2：内嵌 JS 是字符串，少个花括号 C++ 看不出来

改这段 JS 时我的替换区间**吞掉了尾部 4 行**（`shown+=disp; box.appendChild(rd); }); …`），
也就是 `RUNS.forEach` 与 `renderLog` 的闭合 —— C++ 编译**零告警**、单测全绿，而浏览器里
整个调试台会因语法错误**整页空白**。

发现的姿势：调试台的内嵌脚本可以在 Node 里用最小 DOM 垫片**真跑**（`vm.runInContext` +
假的 `document`），然后把 `#loglist` 的子树打印出来 —— 这比盯 HTML 源码可靠得多，
也是这次验证"树真的长对了"的手段。

已加回归：`DebugView.embedded_js_braces_are_balanced` 对脚本做字符串感知的括号平衡体检
（基准值 +1，是模板里 CSS/JS 交界处那个先出现的 `{`；**只要改出不平衡这个数就会变**）。

## 四点六、route 的分支决定进日志（追加）

原先 `c_route` 的节点体返回空 `Command{}`，分支决定只存在于 router 内部 —— 日志里
完全看不出“这轮为什么进了 `act` 而不是收尾”，而这恰恰是调 ReAct 循环最想看的。

改法：**节点体算出决定并发布，router 读它**。

```
· route（3 条）
    - enter   route
    - info    route [info] {"matched":true,"on":"last_ai_outcome","outcome":"tool_calls","target":"act"}
              route: last_ai_outcome = tool_calls  → act          ← 人可读诊断
    - exit·ok route {"results":{"route":{...,"outcome":"tool_calls","target":"act"}}}
```

三个设计点：

1. **为什么让节点体算而不是 router 算**：router 在 `state = schema_.apply(state, cmd.update)`
   **之后**才跑（`graph.cpp` 的 super-step），所以节点体先算好发布、router 再读天然可行；
   反过来则诊断会落到 exit 之后，顺序不对。
2. **发布的是原始目标名**：`_turn_end` 而不是解析后的 `__end__` —— 与 workflow JSON、
   图上的边标签一致，日志里读得懂；router 那边用解析后的表按同一个 outcome 再查一次。
3. **顺带扩白名单**：`results` 的发布者多了 route，VR2 的可引用 result 白名单相应加
   `NodeKind::Route`，于是下游/echo 能用 `$<route id>.result.target`。

## 四点七、图上标出本轮走过的边（追加）

静态图画的是 IR 里的**全部**边；这一轮到底走了哪条、各几次，原先看不出来。现在每个 run
附一份 `edges: [{from,to,count}]`（从引擎 `enter` 序列推：相邻两个执行节点 = 一条边），
前端把走过的边加粗变蓝 + 标 `×N`，切轮次时只更新类与角标、不重建 SVG。

两个实现要点：

1. **首个执行节点只记起点，不产生边** —— 一开始我把它写成 `("__input__", 首个节点)`
   的自环对，凭空多出一条不存在的边（被"数一数走了几条"的测试抓到）。
2. **终端边得单独补**：`_turn_end`/`_fail` 没有 enter 记录，靠相邻对推不出来。两个来源，
   都只在"明确"时才采信 —— ① 最后节点的 exit update 里发布了 `target`（route 走的就是
   这条，见四点六）；② 该节点在图上只有一条 terminal 出边、且本轮最后一条记录是成功
   exit。两条都不满足就不猜（宁可少标，不可标错）。

## 四点八、页面自取数据 + 推导搬进 JS（追加）

`--graph-only` 出页面、日志靠拖入；推导（分组/迭代号/边使用）从 C++ 搬到页面 JS，
成为**唯一实现**（详见 arch/16 §5.3）。两点值得记：

1. **评估过 wasm**：本机没有 emscripten/wasm-ld，`debug_view.cpp` 经 `node_type.hpp`
   依赖整个注册表，要瘦身得先重构；而 wasm 唯一不可替代的优势（单一实现）用"整体搬到
   JS"更彻底。逻辑本身 ~60 行簿记，无算法/精度问题 —— 不值得为它引入一套工具链。
2. **URL 直读**：左栏除拖入外还能填日志 URL（`fetch` 回来走同一个 `ingestTexts`，会话名
   从 URL 末段推）。受同源策略限制 —— `file://` 页面发跨源 `fetch` 必被 CORS 拦，所以失败
   提示里把这条写清楚了，并提示"拖文件进来"这条永远可用的退路。
3. **护栏换工具**：原先在 gtest 里放了个**手写**的括号平衡扫描来守"内嵌 JS 少个花括号"。
   这次加正则字面量（`/^session-/` 一类）把它带偏、报了假警 —— 手写扫描器解析不了 JS。
   已删掉它，改用**真解析器**（node 侧的 `new vm.Script`），既是权威判定又不必维护词法。
   （那次假警值得记：**不可靠的护栏比没有护栏更糟**，它会教人忽略告警。）
4. **搬的过程里又踩了一个"取第一条还是最后一条"**：终端边的补标要取**最后一个节点最后
   一条 exit** 的 `results.target`；我写成"取第一条匹配"后，真机上一轮的 target 被当成
   收尾边 —— `route→act` 被算成 2 次、`route→_turn_end` 整个丢掉。是"把真日志灌进页面
   跑一遍"发现的（不是单测），现已在 `tests/js/debug_derive_test.mjs` 用例 5 钉住。

## 五、遗留

- 不支持嵌套循环（VG11）、不做 for-each。
- `load_history` 禁止放进 body（VG13）—— 需要时放开即可，代价是 messages 膨胀。
- 图仍是平铺的（不做容器框）；若要"框住 body"，前端布局算法要改成递归嵌套布局。
- 计数随快照持久化，checkpoint 续跑从上次计数继续（与旧 goto 相同）。
