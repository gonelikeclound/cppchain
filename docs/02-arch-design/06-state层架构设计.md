# 06 · State 层架构设计（M5）

> 对应路线图 M5。参照 langchain 的 `State`/`Reducer`/`Checkpointer`/`Store`（已核实）。
> 本文件定**层边界与契约**；类级细节留 `03-detail-design/`。

---

## 1. 定位与边界

- **依赖**：M1 消息层（序列化契约是 checkpoint 的地基）。
- **提供**：状态 schema + **每键 reducer**、`add_messages` 消息合并、**Checkpointer 接口 + 内存实现**、**Store 接口 + 内存实现**。
- **不做**：图执行接线（M6 用这些原语）、SQLite/外部持久化（换实现不换接口）、语义检索（Store 先做子串匹配）。

## 2. 参照：langchain 的对应物（已核实）

| 概念 | 要点 |
|---|---|
| **State** | schema + **每键独立 reducer**；默认 overwrite |
| **Reducer** | 二元函数 `new = f(left=累计值, right=本次更新)`；`Annotated[key, reducer]` |
| **`add_messages`** | 预置消息 reducer：**按 message id 去重**（新消息追加、同 id 覆盖）；把 dict 形式反序列化为 Message |
| **Checkpointer（短时）** | `thread_id` 主键；每 super-step 快照；五方法 `put/put_writes/get_tuple/list/delete_thread` |
| **Store（长期）** | 跨线程 kv：`namespace + key + value`；`put/get/delete/search/list_namespaces` |

## 3. 设计

```
state.hpp         StateSchema(每键 reducer) + 预置 add_messages/overwrite_reducer
checkpointer.hpp  StateSnapshot + Checkpointer 接口 + MemoryCheckpointer
store.hpp         StoreItem + Store 接口 + MemoryStore
```

**关键决策：状态用 JSON 承载 + 类型感知的 reducer**

- 状态是 `nlohmann::json` 对象（`key → value`）——直接满足**"状态必须整体可序列化"**的契约（M1 定的地基），checkpoint 无需额外转换。
- 消息用 M1 的规范 JSON 表示（`{"type":"human",…}`），`add_messages` 做**类型感知**合并：
  反序列化为 `Message` → 按 id 去重（同 id 覆盖、新 id 追加）→ 再序列化回 JSON（与 langchain 行为一致）。

```cpp
// state.hpp
using Reducer = std::function<nlohmann::json(nlohmann::json const& left,
                                             nlohmann::json const& right)>;
nlohmann::json overwrite_reducer(nlohmann::json const&, nlohmann::json const& right);
nlohmann::json add_messages(nlohmann::json const& left, nlohmann::json const& right);  // 按 id 去重

class StateSchema {
 public:
  void set_reducer(std::string key, Reducer r);
  nlohmann::json apply(nlohmann::json const& current, nlohmann::json const& update) const;  // 逐键归约
  static StateSchema messages_state();   // 预置：messages 用 add_messages
};

// checkpointer.hpp
struct StateSnapshot {
  nlohmann::json values;                     // 通道值（含 messages）
  std::vector<std::string> next;             // 下一批节点（M6 用）
  nlohmann::json config;                     // thread_id / checkpoint_id / checkpoint_ns
  nlohmann::json metadata;                   // source(input/loop/update) / step / writes
  std::optional<std::string> parent_checkpoint_id;
};

struct WriteRecord { std::string task_id; std::string channel; nlohmann::json value; };

class Checkpointer {                          // 接口：换实现不换接口（SQLite 后续）
 public:
  virtual std::string put(std::string const& thread_id, std::string const& ns,
                          std::optional<std::string> parent_id, nlohmann::json values,
                          nlohmann::json metadata) = 0;      // → checkpoint_id
  virtual void put_writes(std::string const& thread_id, std::string const& ns,
                          std::string const& checkpoint_id, std::vector<WriteRecord> writes) = 0;
  virtual std::optional<StateSnapshot> get_tuple(std::string const& thread_id,
                                                 std::string const& ns,
                                                 std::optional<std::string> checkpoint_id) = 0;
  virtual std::vector<StateSnapshot> list(std::string const& thread_id, std::string const& ns,
                                          std::size_t limit) = 0;   // 新的在前
  virtual void delete_thread(std::string const& thread_id) = 0;
};

// store.hpp —— 长期记忆（跨线程）
struct StoreItem { std::vector<std::string> ns; std::string key; nlohmann::json value;
                   std::string created_at, updated_at; };
class Store {
 public:
  virtual void put(std::vector<std::string> const& ns, std::string const& key, nlohmann::json value) = 0;
  virtual std::optional<StoreItem> get(std::vector<std::string> const& ns, std::string const& key) = 0;
  virtual bool del(std::vector<std::string> const& ns, std::string const& key) = 0;
  virtual std::vector<StoreItem> search(std::vector<std::string> const& ns_prefix,
                                        std::string const& query, std::size_t limit) = 0;
  virtual std::vector<std::vector<std::string>> list_namespaces(
      std::vector<std::string> const& ns_prefix) = 0;
};
```

## 4. 契约

1. **可序列化**：状态/快照/Store 值全部是 JSON，`dump()` 后能原样恢复。
2. **reducer 语义**：未注册 reducer 的键 = overwrite；`add_messages` 同 id **后者覆盖前者**（对应 langchain 的"同 id 覆盖"）。
3. **checkpoint 链**：`put` 返回新 id 并记录 `parent_checkpoint_id` → 支持回放/时间旅行（M6/M10 用）。
4. **namespace 隔离**：checkpointer 用 `(thread_id, checkpoint_ns)` 双键；子图/子代理将用不同 ns（M8）。
5. **不静默**：`get_tuple` 找不到 → `nullopt`；`Store::get` 不存在 → `nullopt`。

## 5. 卡位问题

「上下文压缩」的底座：状态里既有完整消息历史，压缩就是"改写 state 里的 messages"——
`add_messages` 的 reducer 语义（覆盖/追加）正是压缩中间件要操作的接口（M7 落地）。

## 6. DoD

- [ ] `StateSchema` + `overwrite_reducer` + `add_messages`（按 id 去重，含 Message 对象与 dict 混合输入）
- [ ] `Checkpointer` 接口 + `MemoryCheckpointer`（put/put_writes/get_tuple/list/delete_thread + parent 链）
- [ ] `Store` 接口 + `MemoryStore`（namespaces / 子串 search）
- [ ] 测试：reducer 语义、消息去重、快照链与回放、thread 隔离、ns 隔离、Store 搜索与命名空间
- [ ] demo：`examples/state_basics`（状态归约 → 存快照 → 恢复 → Store 跨线程记忆）
- [ ] 状态 JSON round-trip（dump → parse → 语义不变）

## 7. 开放点

1. `add_messages` 的"删除消息"（langchain 有 `RemoveMessage`）——M1 当时砍掉了，M7 压缩需要时再引入。
2. 细粒度 writes 的语义（per-node）在 M6 才有真实用处，M5 只做存取。
3. Store 的语义检索（embedding）——接口留扩展，实现待需要。