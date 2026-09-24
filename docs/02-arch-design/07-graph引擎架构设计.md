# 07 · Graph 引擎架构设计（M6，极简）

> 对应路线图 M6。参照 langchain LangGraph 的 StateGraph（已核实）。
> **极简起步**（路线图 B0 决策）：顺序 super-step + 内存 checkpoint；Streaming/Interrupt/Send 押后到 M10。

---

## 1. 定位与边界

- **依赖**：M1（Message/序列化）、M5（StateSchema/reducer/Checkpointer）。
- **提供**：`StateGraph`（节点/边/条件边/入口）→ `compile()` → `CompiledGraph.invoke()`，每个 super-step 落 checkpoint，支持从快照恢复。
- **不做**（押后）：

| 不做 | 归属 |
|---|---|
| 并行 super-step（同 step 多节点并发） | M10（当前顺序执行） |
| Streaming / Interrupt / Send 动态 fan-out / 时间旅行 UI | M10 |
| Functional API（`@task/@entrypoint`） | 与 M10 一并评估 |

## 2. 参照：LangGraph 的对应物

- **State/Nodes/Edges**：state 共享快照、node 做计算返回 update、edge 决定下一个节点。
- **Reducer 归约**：node 返回的 update 经 per-key reducer 合并进 state（M5 已实现）。
- **条件边**：router 读 state → 返回下一节点名（可为 END）。
- **compile(checkpointer)**：结构检查 + 挂运行时依赖；`thread_id` 决定快照归属。
- **Command**：把"状态更新"与"控制流跳转"合成一个返回值（`update` + `goto`）。
- **recursion_limit**：super-step 上限，防止无限循环。

## 3. 设计

```cpp
namespace cppchain::graph {

inline constexpr char kEnd[] = "__end__";

struct Command {
  Json update = Json::object();          // 状态更新（经 reducer 归约）
  std::vector<std::string> goto_nodes;   // 非空则覆盖出边（Command 简版）
};

struct NodeContext {
  std::string node_name;
  std::string thread_id;
  int step = 0;
};

using NodeFn = std::function<Command(Json const& state, NodeContext& ctx)>;
using RouterFn = std::function<std::vector<std::string>(Json const& state)>;

struct GraphError {
  enum class Kind { InvalidGraph, UnknownNode, RecursionLimit, NodeError };
  Kind kind; std::string message;
};

class StateGraph {
 public:
  explicit StateGraph(state::StateSchema schema);
  StateGraph& add_node(std::string name, NodeFn fn);
  StateGraph& add_edge(std::string from, std::string to);              // to 可为 kEnd
  StateGraph& add_conditional_edges(std::string from, RouterFn router);
  StateGraph& set_entry_point(std::string name);
  CompiledGraph compile(state::Checkpointer* cp = nullptr, CompileOptions opts = {});
};

class CompiledGraph {
 public:
  // 从输入开始执行；thread_id 非空且有快照时**从最新快照恢复**（输入被忽略）
  std::expected<Json, GraphError> invoke(Json const& input, std::string const& thread_id = "");
  std::size_t last_steps() const;
  std::string last_checkpoint_id() const;
};
}
```

**执行模型（顺序 super-step）**

```
state ← input（或快照恢复）
current ← entry（或快照的 next）
loop:
  cmd ← node(current)(state, ctx)
  state ← schema.apply(state, cmd.update)
  cp.put(thread_id, "", parent, state, {step, node, next})     ← 每步一个快照
  next ← cmd.goto_nodes 非空 ? 它们 : 出边(静态优先) : 条件边 router(state)
  若 next 为空或含 kEnd → 结束
  step++ 超 max_steps → RecursionLimit
```

## 4. 契约

1. **顺序执行**：每步跑一个节点；同 step 并行留到 M10（接口不变，只是当前实现顺序推进）。
2. **checkpoint 边界**：每个 super-step 之后写一次（含 `next`），恢复语义 = "回到该步边界继续"。
3. **可序列化**：state 全程 JSON（M5 契约），快照可直接 dump/parse。
4. **静态边与条件边二选一**：同一节点同时配静态边与条件边 → `InvalidGraph`（对齐 langchain 的约束）。
5. **不静默**：未知节点、图结构非法、超出步数上限 → `GraphError`（不返回"部分成功"）。

## 5. 卡位问题

- **上下文压缩**（M7）：压缩中间件就是一个 node —— 读 state.messages → 改写 → 返回 `{"messages": [...同 id 覆盖...]}`（M5 的 add_messages 已支持）。
- **工具副作用**（M7）：`Command` 是控制流原语，审批节点可据此改写 goto。

## 6. DoD

- [ ] `StateGraph`（节点/静态边/条件边/入口）+ 结构校验
- [ ] `CompiledGraph.invoke`：顺序 super-step、每步 checkpoint、步数上限
- [ ] 快照恢复：`thread_id` 续跑（含从中间快照恢复）
- [ ] `Command.goto` 覆盖出边（简化版控制流）
- [ ] 测试：线性图、条件路由（含 END）、循环 + 上限、恢复续跑、非法图、JSON 可序列化
- [ ] demo：`examples/graph_basics`（确定性 workflow + 条件路由 + 中途恢复）

## 7. 开放点

1. 并行 super-step 的实现方式（线程池 vs curl_multi 事件循环复用）。
2. `next` 的语义（多节点时的扇出）与 M10 的 Send 的关系。
3. 节点级重试/超时策略（langchain 有 retry_policy/timeout）。