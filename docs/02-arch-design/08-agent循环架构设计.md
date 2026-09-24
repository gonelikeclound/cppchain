# 08 · Agent 循环架构设计（M7）

> 对应路线图 M7。参照 langchain `create_agent`（harness = model + tools + middleware）。
> 本层是三块地基建起来的第一个**真正的 agent**：M2 模型 + M3 工具 + M6 图/状态。

---

## 1. 定位与边界

- **依赖**：M2（ChatModel）、M3（ToolRegistry/ToolNode）、M5（StateSchema/Checkpointer）、M6（可选：作为图的一个节点跑）。
- **提供**：`Agent`（ReAct 风格循环：模型 → 工具 → 模型 …）、**Middleware 钩子**、三个卡位问题的落地件。
- **不做**：流式 agent（押后）、多 agent 编排（M8）、interrupt/审批的完整语义（M10）。

## 2. 参照：langchain 的 create_agent

- `create_agent(model, tools, system_prompt, middleware=[...])` → 产出**一个 LangGraph graph**，不是独立引擎。
- middleware 是插槽：动态提示、工具调用限额、模型请求改写等。
- 循环本质：`model.invoke(msgs)` → 有 `tool_calls` 就执行 → `ToolMessage` 回填 → 再问一次，直到没有工具调用。

## 3. 设计

```cpp
namespace cppchain::agent {

class Middleware {                      // 钩子（简版：三处，够用即可）
 public:
  virtual ~Middleware() = default;
  virtual void before_model(std::vector<message::Message>& msgs) {}          // 改消息（压缩/裁剪）
  virtual bool approve_tool_call(message::ToolCall const&, std::string& why) { return true; }  // 副作用审批
  virtual void after_tool(message::Message& tool_message) {}                 // 改工具结果（截断）
};

// 预置中间件（三个卡位问题各一个落点）
struct TruncateToolResults : Middleware;  // 超长工具返回：按字符/行截断并标注
struct TrimHistory : Middleware;          // 上下文压缩：超阈值时折叠旧消息
                                          //   · 无 summarizer → 丢弃 + 占位说明
                                          //   · 有 summarizer → 调模型生成摘要（真压缩）
struct ToolApproval : Middleware;         // 工具副作用：按名单/回调拒绝执行

struct AgentOptions {
  std::string system_prompt;
  std::size_t max_iterations = 10;
  std::string thread_id;                  // 非空 + checkpointer → 每轮落快照
};

class Agent {
 public:
  Agent(ChatModel&, tool::ToolRegistry const&, AgentOptions);
  void add_middleware(std::shared_ptr<Middleware>);
  std::shared_ptr<Checkpointer> ...  // 通过 options.checkpointer 注入
  // 跑一轮完整对话（输入 → 可能的多次工具往返 → 最终 AI 消息）
  std::expected<message::Message, ModelError> invoke(std::vector<message::Message> input);
  std::vector<message::Message> const& transcript() const;   // 完整轨迹
};
}
```

**循环（顺序，与 M6 的 super-step 同构）**

```
msgs ← [system?] + input（或从 thread 快照恢复）
for i in 0..max_iterations:
   每个 middleware before_model(msgs)          ← 压缩/裁剪发生在这里（hot path）
   ai ← model.invoke(msgs, {tools=registry.to_openai_tools()})
   msgs += ai
   若 ai.tool_calls 为空 → 返回 ai（结束）
   for tc in ai.tool_calls:
       middleware 审批 → 拒绝则 ToolMessage{status=error, "rejected: ..."}
       否则 ToolNode 执行 → 每个 middleware after_tool(msg)   ← 截断发生在这里
       msgs += msg
   若有 checkpointer+thread → put(快照)
超出上限 → 错误（不再静默继续）
```

## 4. 契约

1. **协议闭环**：每轮 tool_calls 必须有 ToolMessage 回填（复用 M1 `validate_sequence` 校验）。
2. **中间件顺序**：before_model 按注册顺序；after_tool 按注册顺序；审批任一拒绝即拒绝。
3. **拒绝语义**：拒绝不抛错，而是回喂 `status="error"` 的 ToolMessage（模型可换策略）。
4. **上限**：`max_iterations` 用尽 → `ModelError{Kind::Unknown/Parse}`（不返回半成品）。
5. **可持久化**：每轮把 `{"messages": [...]}` 存快照（M5 的 messages_state schema）。

## 5. 三个卡位问题（本层落地）

| 问题 | 落地件 | 验证方式 |
|---|---|---|
| **上下文压缩** | `TrimHistory`（阈值触发；可接 LLM 摘要） | demo 里跑长对话，对比压缩前后 token/消息数 |
| **超长工具返回** | `TruncateToolResults`（截断 + 标注被截断字节数） | demo 里造一个长输出工具，观察截断与模型可用性 |
| **工具副作用** | `ToolApproval`（拒绝 → error ToolMessage 回喂） | demo 里拒绝一次写操作，看模型如何退让 |

三者都先在本层给出**可运行的最小实现**，横向对比与深化留到 M9。

## 6. DoD

- [ ] `Middleware` 三钩子 + `Agent` 循环（含 system prompt、上限、transcript）
- [ ] `TruncateToolResults` / `TrimHistory`（含可选 LLM 摘要）/ `ToolApproval`
- [ ] checkpointer 集成：每轮快照；可从 thread 续跑
- [ ] 测试：单轮工具闭环、多轮、上限、审批拒绝回喂、截断、裁剪、快照
- [ ] demo：`examples/agent_loop`（真机：多步工具 + 三个中间件各演示一次）

## 7. 开放点

1. 压缩策略的"何时压"：按消息数 vs 按 token 估算 vs 模型上下文占用率。
2. 摘要质量与成本（多一次 LLM 调用）——值得量化（M9）。
3. 审批的记忆化（同一会话内同一工具不再重复问）。
4. 与 M6 的关系：Agent 目前是**自成一体的循环**；后续可暴露为图节点（`add_node("agent", ...)`）以支持 M8 的多 agent 图。