# 13 · Graph 运行时补全设计（M10：Streaming / Interrupt / Send / 时间旅行）

> 对应路线图 M10（弹性的运行时补全）。参照 langchain LangGraph 的 stream /
> interrupt / Command(sends) / 时间旅行（fork）。
> 写实文档：接口与执行语义均按 `src/graph/graph.cpp` / `include/cppchain/graph/graph.hpp` 的实现陈述。

---

## 1. 定位与边界

- **依赖**：M1（序列化）／M5（state）／M6（极简引擎）——全部在既有层内扩展；
  新增类型均为纯数据结构（struct + `nlohmann::json`）+ `std::function` 回调，**graph 层零外部依赖持续成立**。
- **提供**：图运行时三大能力——
  | 能力 | 一句话 |
  |---|---|
  | **Streaming** | 注入事件 sink，逐"包"吐 `node_start` / `node_end` / `values`，节点内经 `ctx.emit` 自报流式事件 |
  | **Interrupt** | 节点内 `interrupt(payload)` 悬挂 → 落 checkpoint → 外部 `resume` → **该节点从头重跑**，调用点返回答复 |
  | **Send** | `Send(node, partial_state)` 动态 fan-out：独立包 + 局部输入，结果经 reducer 归约回主 state（顺序执行，接口按可并行设计） |
  | **时间旅行** | `invoke(thread, {fork: {source_thread, checkpoint_id}})`：从**历史 checkpoint** 恢复（state + next）并继续，新轨迹落新线程、原轨迹不污染；分叉点即新链的 parent |
- **不做**（押后或明确不做）：真并行线程池、agent 层审批改造、把 Send/Interrupt 接入 workflow IR、`c_model` 切流（模型层 stream 与事件通道已就绪，接入 = 改 node_types 一处，见 §10 开放点）。

## 2. 问题：极简引擎（M6）的三个缺口

| 缺口 | 现象 | M10 落点 |
|---|---|---|
| 没有事件出口 | invoke 黑盒跑完，进展不可见 | `InvokeOptions.sink/mode`（回调注入） |
| 没有"暂停等外部" | 审批只能是同步回调，无真 HITL | `interrupt()` 原语 + resume |
| 只能单路径 | `current` 每步只跑 front 且被整条替换，**多节点 fan-out 会被丢弃** | `current` 升级为**包队列** |

## 3. 设计：一个执行通道，四个特性共用

```cpp
// graph.hpp 新增（§4 列出全部类型；此处为骨架）
struct InvokeOptions {
  EventSink sink = nullptr;                       // 注入即流；null = 零开销
  EventMode mode = EventMode::Updates;            // Updates|Snapshot|Deltas
  std::optional<Json> resume;                     // 对上次 interrupt 的答复
  std::optional<ForkSpec> fork;                   // 时间旅行分叉（§7）
};
struct ForkSpec { std::string source_thread; std::string checkpoint_id; };

enum class EventMode : std::uint32_t { Off, Updates, Snapshot, Deltas, All };

struct StreamEvent { std::uint64_t seq; std::string type, node, node_kind;
                     int step; Json payload; bool ok;
                     std::optional<std::string> status; };
using EventSink = std::function<void(StreamEvent const&)>;

class InterruptSignal { /* 不继承 std::exception —— 防 catch(std::exception) 误吞 */ };
struct Send { std::string node; Json state; };
struct Command { Json update; std::vector<std::string> goto_nodes; std::vector<Send> sends; };
Json interrupt(Json const& payload, NodeContext& ctx);  // 首调抛 / 重跑返回 resume 值
```

**执行模型（invoke 四态）** —— `current` 从 `vector<string>` 升为 `deque<state::Packet>`
（`Packet{node, optional<Json> state}`）：

```
① 恢复态：有快照 → state=values、current=next（若无 resume 且挂着 → 保持挂着返回）
② 执行态：pop 一个包 → 输入视图 = pkt.state ? *pkt.state : state →
           emit node_start → node fn（可抛 InterruptSignal / 普通异常）→
           schema.apply(update) → emit node_end(+values)
③ 路由态：本包产出的目标**追加到队尾**（super-step 语义，非替换！）：
           goto > sends > 静态边 > 条件边
④ 每包落一个快照（next = 剩余全部待执行包），++last_steps（**max_steps 按包计数**）
```

关键点：
- **普通单路径 = 全局状态包的特例**：`route` 产 1 个全局包 → 与 M6 行为逐位等价
  （既有测试逐条仍绿的证据，见 §8）。
- **追加而非替换**：M6 的 `current = *next` 会丢掉剩余包；改为队尾追加后，一次
  Send 展开的所有目标都先跑完（同一批内 FIFO）。
- **包输入视图不主动合并**：`pkt.state` 仅供该节点读取；想持久化的键写进自己的
  update（LangGraph 同款"任务写共享通道"），reducer 负责归约。

## 4. Streaming

- **事件种类与时机**（每次包执行）：
  | type | 时机 | payload |
  |---|---|---|
  | `node_start` | 进入节点前 | `{}` |
  | `node_end` | `apply` 后 | `cmd.update`（status `ok`）或 `{"error":...}`（`!ok`） |
  | `values` | node_end 后，`mode & Snapshot` 时 | 完整 state |
  | `interrupt` | 悬挂时 | interrupt payload |
  | `delta`（节点自定） | 节点内 `ctx.emit("delta", ...)`，`mode & Deltas` 才转发 | 自定 |
- `NodeContext.emit` 分配 seq（与引擎事件同一单调序列）；sink 空 / 无 Deltas 位 → 静默（零开销）。
- 事件信封 `{seq, type, node, node_kind, step, payload, ok, status}` 与 workflow 层
  `session_log` 的形状对齐（docs/12 §6.3），投影适配器据此直投，不改日志内部编码。
- workflow 接入：`FireOptions.stream` 把 sink 传给 invoke（全模式）；启用后
  `LoggingStateGraph` wrapper 结构日志**让位**（`SessionSink.streaming`），防双写。

## 5. Interrupt（真 HITL 审批的地基）

- **语义**（LangGraph 对齐）：`interrupt(payload)` 首次调用抛 `InterruptSignal` →
  引擎捕获 → 该节点**无 update 落盘**（快照存节点入口完整 state），`next` = 剩余包 +
  重跑包（放最后、**保留原包输入视图**），payload 随 `put_writes` 落盘 → invoke 以
  "挂着"返回（调用方用 `last_interrupt()` 区分）→ 外部 `resume` 再 invoke → 恢复快照，
  对重跑包注入 `ctx.pending_resume` → `interrupt()` 返回该值 → 节点继续。
- **幂等契约**：重跑 = 节点从头执行，**interrupt 之前的副作用必然重复**（不是默认，
  是约定）。副作用写 interrupt 之后、或做成幂等。测试与 demo 均锁住该行为。
- 约束：
  - `interrupt` 需 `checkpointer != null && thread_id 非空`，否则 `InterruptUnavailable`
    （挂起来没地方存、没法恢复）；
  - 线程未悬挂但有 resume → `ResumeNotInterrupted`；
  - `InterruptSignal` 不继承 `std::exception` → 节点的 `catch(std::exception)` 拦不到它；
  - **resume 按 index** 本轮简化为"悬挂节点名"（单活跃中断）；并行多中断的按 index 分发押后。
- 新增错误档：`InterruptUnavailable` / `ResumeNotInterrupted`（`graph_error_kind` 同步）。

## 6. Send（动态 fan-out）

- `Command.sends` 非空（且 `goto_nodes` 为空）→ 路由 = 仅 sends（覆盖出边）；`goto` 优先于 `sends`。
- 每个 `Send(node, partial_state)` 展开为一个**独立包**：节点输入 = `*pkt.state`（局部视图，
  见 §3），结果照常经 per-key reducer 归约回主 state。
- `send.node` 未注册 / 为 `kEnd` → `UnknownNode`。
- **多级扇出**：Send 目标的节点也可再 Send/跳转，包队列天然支持（`Send.multi_level_fanout` 锁住）。
- **归拢/barrier**：send 的 worker 各自跑完后"如何汇合到汇聚节点"需要 barrier 语义，
  **本轮不做**（归约在共享 state 由 reducer 累积；真并行超步一并设计时再补）。
- **顺序执行**：同一批包 FIFO 顺序推进；`Packet`/`deque` 结构天然可并行，`max_parallel`
  线程池押后（用户确认"先顺序，留并行接口"）。

## 7. 时间旅行分叉（fork）

- **API**：`InvokeOptions.fork = ForkSpec{source_thread, checkpoint_id}`。
  `checkpoint_id` 为空 = 该线程最新快照；通常指定历史 id。
- **执行语义**：恢复该快照的 `values + next` 并继续跑（input 被忽略，与普通恢复一致）；
  新快照落到**本次的 thread_id**（可不同于来源线程），原线程轨迹不污染；
  **分叉点即新链的 parent**（`parent_checkpoint_id` 跨线程可追溯）。
- **fail-closed**：分叉点（线程/checkpoint id）不存在 → `CheckpointNotFound`，不默默
  退回"最新/全新开跑"（防走错轨迹）。
- **与 interrupt 的叠加**：分叉点若正站在 interrupt 上 → 语义与普通恢复一致
  （无 resume 保持挂着、有 resume 继续），`get_writes` 从来源线程取 payload。
- **workflow 接入**：`FireOptions.fork` 透传至 invoke（需编译期注入 checkpointer）。
- 测试：`tests/graph/time_travel_test.cpp`（4 项，含线程隔离 / fail-closed / 悬挂分叉点）；
  demo：`examples/m10_fork`（先跑完整轨迹 → 改环境 → 从中间 checkpoint 分叉得不同未来）。

## 8. Checkpoint 契约升级：`next` 从名字列表 → 包队列

- `StateSnapshot::next` 由 `vector<string>` 升为 `vector<Packet>`；`Checkpointer::put` 签名同步。
- 落盘格式 v1→v2：v2 的 next 为 `{"node":…, "state": …}` 对象数组（state 为空 = 全局包）；
  **load 兼容 v1**（字符串数组 → `Packet{node, nullopt}`），旧快照直接可读，不迁移不丢弃
  （`FileCheckpointer.legacy_v1_next_string_array_loads_as_packets` 锁住）。
- `optional<Json>` 显式写 null（继承 memory_backends 的既有纪律，见其 dump 注释）。
- recovery 语义不变："回到该步边界继续"——now restores `current = snap->next`。

## 9. 测试与验证

- **graph 层新增 21 项**：Streaming 5（边界事件/快照开关/delta 转发/零 sink 等价/错误事件）、
  Interrupt 8（悬挂与完整 state/resume 重跑与副作用重复/无 resume 保持挂/已跑完带 resume 报错/
  需 checkpointer/不被泛 catch 吞/跨实例恢复）、Send 8（fan-out 归约/局部输入视图/目标未知/
  goto 优先/send 覆盖静态边/多级扇出/checkpoint 承载包/super-step 部分悬挂恢复）+ **Fork 4**
  （新线程继续/未知分叉点 fail-closed/未知来源线程 fail-closed/悬挂分叉点需 resume）。
- **workflow 层新增 4 项**（WfM10）：fire_once 收集事件（All=每节点 3 事件）、stream
  模式 wrapper 让位（双源互斥）、project_stream_to_log 信封映射、interrupt 记录不参与 replay。
- **既有 386 项逐条仍绿**（普通图 = 全局包特例的行为位等价证据）；全仓 **415 全绿**、零告警。
- **demo ×4（离线）**：`m10_streaming`（事件时间线 + delta）、`m10_interrupt`（审批悬挂/
  resume/幂等计数）、`m10_send`（map-reduce fan-out）、`m10_fork`（从中间 checkpoint 分叉得不同未来）。

## 10. DoD 与开放点

### DoD
- [x] `Packet` 化 next + v1/v2 兼容加载（P0）
- [x] 包队列执行器 + Streaming 事件 sink（P1）
- [x] `interrupt()` / `resume` + 2 新错误档 + 幂等契约测试（P2）
- [x] `Command.sends` / Send 独立包 + reducer 归约 + checkpoint 承载（P3）
- [x] workflow 接入：`FireOptions.stream` + wrapper 让位 + `project_stream_to_log` +
      `session_log.engine_interrupt`（P4）
- [x] 时间旅行分叉：`InvokeOptions.fork` + 新错误档 + 线程隔离测试 + demo（P6-⑦）
- [x] 4 个纯离线 demo（P6）
- [x] 既有测试逐条仍绿 + 新增 29 项；全仓 415 全绿、零告警

### 开放点
1. **分叉后改写分叉点 state**（`update_state`）：目前 fork 用快照原样继续；LangGraph 允许
   在分叉点改写部分 state 再跑（"改一个字段探新路"），需 `get_state/update_state` 类接口。
2. **真并行**：同批包并行线程池（`max_parallel`）——接口与数据结构已就绪，且需配套 barrier。
3. **`c_model` 切流**：模型层 `stream` + delta 事件通道已就绪；node_types.cpp 的 `c_model`
   默认走阻塞 invoke，Deltas 模式切流留作开放项（需可流式测试模型基建）。
4. **Send × workflow IR**：IR 的 refs（`$_`/`@config`）在包内上下文（每包独立局部输入）的
   解析面需单独评估 —— 本轮 Send 只在 graph 层原语。
5. **resume 按 index**：单活跃中断 = 悬挂节点名；并行多中断需 LangGraph 式 task 粒度索引。
6. **Event 并发语义**：真并行超步到来时 sink 可能跨线程回调 —— 文档约定"sink 需线程安全、
   StreamEvent 值类型可拷贝"，不预埋锁。
7. **max_steps 按包**：一个 super-step N 个包耗 N 步（既有线性/条件图无差异），文档已注明。