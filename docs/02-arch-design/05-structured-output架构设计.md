# 05 · Structured Output 架构设计（M4）

> 对应路线图 M4。参照 langchain 的 `with_structured_output`（schema 驱动输出，常作 route/judge）。
> 本文件定**层边界与契约**；类级细节留 `03-detail-design/`。

---

## 1. 定位与边界

- **依赖**：M1（Message/序列化）、M2（ChatModel；tools wire 通道已跑通真机）。
- **提供**：schema 驱动的结构化输出（两种策略）、最小 JSON Schema 校验、失败回喂重试、route/classify 便捷件。
- **不做**：

| 不做 | 归属 |
|---|---|
| 完整 JSON Schema 校验（$ref/oneOf/allOf/format…） | 只做**子集**：type/required/properties/enum/items |
| 图条件路由的接线 | M6（本层只产出"决策"） |
| 结构化输出的流式 | 不需要（结构化是一次成形） |

## 2. 前置结论：端点实测（DeepSeek，2026-09）

| 机制 | 实测 | 结论 |
|---|---|---|
| `response_format: {"type":"json_schema", strict}` | ❌ 400 `This response_format type is unavailable now` | **不可用**，不能指望服务端强制 schema |
| `response_format: {"type":"json_object"}` | ✅ 200，返回合法 JSON | 可用，但 schema 只靠 prompt 约束 |
| **强制工具调用** `tool_choice={function}` | ✅ 探针 5/5 满足 schema | 服务端按 schema 塑形（不依赖模型自觉） |

→ 与 `std::execution`/反射同一类事实：**标准/文档写了 ≠ 端点实现了**。设计必须建立在实测上。

### 2.1 对照实验（受控，各 5 次，`examples/structured_output` ③ 段）

| 策略 | schema 一次成型率 | 平均耗时 |
|---|---|---|
| ToolCall | **5/5** | 786 ms |
| JsonObject | **5/5** | 589 ms |

**如实说明**：探针阶段只测了 ToolCall，据此写成"更可靠"是**过度解读**；受控对照显示
在本端点 + 本模型 + 本 schema 下**两者无差异**。默认仍取 ToolCall（理由是"服务端塑形、
不依赖模型自觉"这一**机制**上的稳健性），但**不能声称实测更准**。
差异是否在某些模型/更复杂 schema 上出现，需要更多实验——这本身是后续可做的题。

## 3. 设计

```
StructuredOutput（门面）
├─ 策略① ToolCall    （默认）把 schema 包成一个工具 + tool_choice 强制选择
│                      → 从 tool_calls[0].args 取结果（服务端按 schema 塑形）
├─ 策略② JsonObject  response_format=json_object + prompt 里描述 schema
│                      → 解析 text() 为 JSON
├─ 校验            最小 JSON Schema 子集校验（type/required/properties/enum/items）
└─ 重试            失败（解析失败/校验不过）→ 把错误回喂为一条 user 消息重试，最多 N 次
```

```cpp
enum class StructuredMode { ToolCall, JsonObject };

struct StructuredOptions {
  StructuredMode mode = StructuredMode::ToolCall;  // 默认；对照实验与本策略持平(见 §2.1)
  int max_retries = 2;                             // 失败回喂重试次数
  std::string tool_name = "structured_output";     // ToolCall 策略用的工具名
};

// 最小 JSON Schema 校验：返回 nullopt 表示通过，否则返回人类可读的违规说明
std::optional<std::string> validate_json(nlohmann::json const& schema,
                                        nlohmann::json const& value);

class StructuredOutput {
 public:
  StructuredOutput(ChatModel& model, nlohmann::json schema, StructuredOptions opts = {});
  std::expected<nlohmann::json, ModelError> invoke(std::span<Message const> msgs, CallOptions opts);
  // route/judge 便捷件：schema = {enum: labels} 的特例
  std::expected<std::string, ModelError> classify(std::span<Message const> msgs,
                                                  std::vector<std::string> labels, CallOptions opts);
};
```

**CallOptions 增加一个字段**（wire 侧最小改动）：

```cpp
nlohmann::json response_format = nullptr;   // 非 null 时并入请求体（json_object 策略用）
```

## 4. 契约

1. **成功**：返回**已通过 schema 校验**的 JSON；校验不过绝不返回（宁可报错）。
2. **重试**：把失败原因作为一条 user 消息回喂（"上次输出违反 schema：…，请重新输出"），
   并在消息序列里保留上一次的 assistant 输出（让模型看到自己的错误）。
3. **耗尽**：达到 `max_retries` 仍失败 → 返回 `ModelError{Kind::Parse}`，`message` 含最后一次违规说明。
4. **不静默**：`classify` 的 label 必须命中 enum；否则同样走重试/报错。

## 5. 卡位问题

本层是 M6 条件路由、M7 评判（judge）的前置件；「工具副作用」无关，不在此层处理。

## 6. DoD（已全部达成 ✅）

- [x] `StructuredOutput` 两种策略 + `CallOptions::response_format` wire 支持
- [x] 最小 JSON Schema 校验（type/required/properties/enum/items，含嵌套）
- [x] 失败回喂重试（含重试请求内容可断言）
- [x] `classify` route/judge 便捷件
- [x] 测试 11 项：两种策略的请求形状、解析、校验失败→重试→成功、重试耗尽、enum 越界
- [x] demo：`examples/structured_output`（真机：抽取 + 分类 + **两策略遵循度对照**）

## 7. 开放点

1. 重试策略：固定次数 vs 退避；是否区分"解析失败"与"校验失败"的不同提示词。
2. 是否支持 `strict` 模式的服务端探测（未来端点若支持 json_schema，自动切换）。
3. 结构化输出与工具调用的**语义混淆**风险（ToolCall 策略下，模型可能真的去"调用"这个假工具）——需在 prompt 里说明或使用独立命名空间。