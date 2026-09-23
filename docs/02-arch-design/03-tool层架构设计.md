# 03 · Tool 层架构设计（M3）

> 对应路线图 M3。参照 langchain `tools` / `workflows-agents` 的工具调用闭环（已核实）。
> 本文件定**层边界与契约**；类级细节留 `03-detail-design/`。

---

## 1. 定位与边界

- **依赖**：M1 `cppchain_message`（Message/ToolCall/ToolMessage 协议）、M2 `cppchain_model`（tools wire 通道已跑通真机）。
- **提供**：工具规格与注册表、**工具检索（可插拔 Selector）**、`ToolNode` 等价物（并行执行 + 错误回喂）。
- **不做**：

| 不做 | 归属 |
|---|---|
| 图/状态集成、`Command(update, goto)` 真语义 | M6（Graph） |
| 工具内 `interrupt()` 审批（副作用） | M7 / M10 |
| embedding 检索实现（仅预留接口） | 后续（需 embedding 端点） |
| MCP 适配、动态工具加载 | 后续 |
| 工具 schema 自动推导（反射） | ❌ 实测不可用，见 §3 |

## 2. 参照：langchain 工具层（已核实）

- `@tool` 装饰普通函数，从 **docstring + 类型注解**推导 schema；`tool.name` / `tool.invoke(args)`。
- `llm.bind_tools([...])` → 模型输出 `tool_calls`；执行端 `tools_by_name[name].invoke(tool_call["args"])`。
- **`ToolNode`**（预置节点）：自动**并行执行**工具、错误处理、注入 state/context；推荐直接作节点。
- 工具可注入 `ToolRuntime` 读 state/context；**工具可返回 `Command(update, goto)`** 回写状态与控制流。
- 工具内可 `interrupt()` 做"执行前审批/改参数"。
- 协议闭环：`ToolMessage(content=…, tool_call_id=…)` 回填（M1 已实现 `validate_sequence` 强制）。

## 3. 前置结论：C++26 静态反射实测（GCC 16）

| 项 | 实测 |
|---|---|
| `^^` 反射运算符 | ⚠️ 仅在 `-freflection` 下存在 |
| `std::meta::identifier_of` / `parameters_of` | ❌ `is not a constant expression`（库实现为空壳） |

**结论**：**schema 无法从函数签名自动推导** → 采用**显式 Schema + handler** 路线（与"看得见每一步"的基调一致）。

## 4. 设计

```
ToolSpec        name + description + JSON Schema + handler + read_only(副作用标记)
   │
ToolRegistry    注册 / 按名查找 / 全量 schema → OpenAI tools
   │
ToolSelector    可插拔检索：All(小集合) / Keyword(自研评分) / [Embedding 预留]
   │            ← 「大批量工具查找」卡位问题的落点
ToolNode        tool_calls →（并行、有界）执行 → ToolMessage[]（错误回喂）→ 协议闭环
```

### 4.1 核心类型（草案）

```cpp
namespace cppchain::tool {

struct ToolError { std::string message; };

struct ToolContext {                  // 注入给工具（后续扩展 state/store/审批）
  std::string tool_call_id;
  core::CancelToken cancel;
};

struct ToolSpec {
  std::string name;
  std::string description;
  nlohmann::json parameters = nlohmann::json::object();   // JSON Schema
  using Handler = std::function<std::expected<nlohmann::json, ToolError>(
      nlohmann::json const& args, ToolContext& ctx)>;
  Handler handler;
  bool read_only = true;              // 副作用标记（M7 审批用）
  nlohmann::json to_openai() const;   // → {"type":"function","function":{...}}
};

// 参数提取助手（无反射，显式取值 + 明确报错）
template <typename T> std::expected<T, ToolError> arg(nlohmann::json const&, std::string_view key,
                                                      bool required = true);

class ToolRegistry {
 public:
  void add(ToolSpec spec);
  ToolSpec const* find(std::string_view name) const;
  std::span<ToolSpec const> all() const;
  nlohmann::json to_openai_tools(std::span<ToolSpec const> subset) const;
};

class ToolSelector {                  // 可插拔检索
 public:
  virtual ~ToolSelector() = default;
  virtual std::vector<ToolSpec const*> select(ToolRegistry const&, std::string_view query,
                                              std::size_t k) const = 0;
};
class AllToolsSelector;               // 小集合：全量
class KeywordSelector;                // 关键词评分（name/description 词命中加权）
// EmbeddingSelector: 预留接口，待 embedding 端点接入

struct ToolNodeOptions { std::size_t max_parallel = 4; bool catch_errors = true; };

class ToolNode {
 public:
  ToolNode(ToolRegistry const* registry, ToolNodeOptions opts = {});
  // 执行一条 AI 消息里的全部 tool_calls → ToolMessage[]（顺序与 tool_calls 一致）
  std::vector<Message> run(Message const& ai_message) const;
};
}
```

### 4.2 决策（已对齐）

| 决策 | 结论 |
|---|---|
| 工具声明 | **显式 Schema + handler**（反射不可用）；配 `arg<T>()` 取值助手 |
| 工具检索 | **可插拔 Selector**，关键词起步（无依赖），embedding 留接口 |
| 错误语义 | **错误回喂模型**：失败 → `ToolMessage{status="error"}`；可配置改为中断 |
| 并行 | **同一条 AI 消息的多个 tool_calls 并行执行**（有界线程），结果顺序与 `tool_call_id` 严格对应 |

## 5. 契约

1. **schema → wire**：`ToolSpec::to_openai()` 产出 `{"type":"function","function":{name,description,parameters}}`，直接填入 M2 的 `CallOptions::tools`。
2. **协议闭环**：`ToolNode::run(ai)` 的输出必须满足 M1 `validate_sequence({ai, tool...})`。
3. **错误回喂**：`ToolMessage.status = "error"`，内容含可读错误（供模型自愈）；`catch_errors=false` 时抛出。
4. **并行安全**：handler 可能并行执行 → 契约要求 handler **线程安全**（文档标注）；结果按 `tool_calls` 原序返回。
5. **副作用标记**：`read_only=false` 的工具在本层不拦截，但标记会传递给 M7 的审批中间件。

## 6. 卡位问题：大批量工具查找

- M3 交付 `AllToolsSelector`（基线）与 `KeywordSelector`（检索），并用 `examples/tools_retrieval` **量化**：
  工具数 N 从 10 → 1000 时，全量 schema 的 prompt 体积/估算 token 与检索后的对比。
- 结论沉淀 `04-problems/01-大批量工具查找.md`（本层直接产出该文档的首版数据）。

## 7. 目录与构建

```
include/cppchain/tool/{tool.hpp, registry.hpp, selector.hpp, tool_node.hpp}
src/tool/{tool.cpp, tool_node.cpp}    # registry/selector 为 header-only
tests/tool/{tool_spec_test.cpp, selector_test.cpp, tool_node_test.cpp}
examples/tools_basic/  examples/tools_retrieval/
```
CMake：`cppchain_tool` 依赖 `cppchain_model`（需要 Message/序列化）。

## 8. DoD（已全部达成 ✅）

- [x] `ToolSpec`（显式 schema + handler + `arg<T>()`）与 `to_openai()` 正确
- [x] `ToolRegistry` 注册/查找/全量导出
- [x] `ToolSelector` 接口 + `AllToolsSelector` + `KeywordSelector`（含 CJK 2-gram + IDF，评分可测）
- [x] `ToolNode`：并行执行（有界）、错误回喂、结果顺序与 `tool_call_id` 对应、协议闭环
- [x] 测试 26 项全绿（schema/wire、selector 评分与 IDF、ToolNode 六类行为、并行时序）
- [x] demo：`tools_basic`（闭环 + 错误回喂）、`tools_retrieval`（体积 + precision/recall）
- [x] [`04-problems/01-大批量工具查找.md`](../04-problems/01-大批量工具查找.md) 首版（含实测数据与踩坑复盘）

## 9. 开放点

1. 并行实现：`std::async` vs 复用 curl_multi 事件循环（后者对 HTTP 工具更优，但需与 M2 传输协作）。
2. `ToolNode` 是否需要"部分结果"语义（一个工具失败 vs 全部失败）。
3. 关键词评分的具体算法（词命中/字段加权/IDF）——先简单可解释，数据说话。