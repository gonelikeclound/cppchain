# 01 · Message 层架构设计（M1）

> 对应路线图 M1。参照 langchain_core/messages（base.py/ai.py/tool.py/content.py/utils.py 源码已核实）。
> 本文件定**层边界与契约**；类级细节（构造、实现、算法）留 `03-detail-design/`。

---

## 1. 定位与边界

- M1 是全文的**共享词汇层**：Message 被 Model、Graph State、Checkpointer、Store、Streaming 共同引用。
- **只依赖**：nlohmann/json。**不依赖** HTTP、Provider、Graph。
- **提供**：消息类型族 + ContentBlock + ToolCall + **JSON 序列化契约**。
- **不提供**（后续层的事）：OpenAI wire 协议（M2）、工具执行（M3）、状态缩减/压缩（M7）。

## 2. 参照：langchain 接口面（摘要，全部已核实）

| langchain | 要点 | 我们怎么办 |
|---|---|---|
| `BaseMessage` | 字段 `content/additional_kwargs/response_metadata/type(判别式)/name/id`；`content_blocks`/`text` 属性 | 收敛为单一 `Message`，type=Role 判别 |
| `BaseMessageChunk` | `__add__` 流式合并语义 | `MessageChunk` + `operator+`（按规则合并） |
| `Human/System/AI/Tool/Chat Message` | type 字面量 `human/system/ai/tool/chat` | Role 枚举，Chat 支持自定义字符串 |
| `AIMessage` | `tool_calls/invalid_tool_calls/usage_metadata` | Message(Role::Ai) 上的可选字段 |
| `ToolMessage` | `tool_call_id`（必填回填）、`status: success\|error` | Message(Role::Tool) 上的必填/可选字段 |
| `RemoveMessage` | 图内删除消息用的修饰消息 | 延后（M5/图内删除时引入） |
| ContentBlock 联合 | `text/reasoning/tool_call/tool_call_chunk/invalid_tool_call/image/video/audio/text-plain/file/non_standard` + `server_tool_*` | **v1 只做 text/reasoning/tool_call 三族**，其余砍 |
| `ToolCall` TypedDict | `{name, args, id, type:"tool_call"}` | `std::optional<ToolCall>` 结构体 + JSON |
| `ToolCallChunk` | `{name\|None, args\|None, id\|None, index}` | 流式累积用 |
| `UsageMetadata` | input/output/total + details{audio, cache_*, reasoning} | 简化：只保留 input/output/total/reasoning |
| 序列化 | `message_to_dict/messages_to_dict/messages_from_dict` | 自定规范 JSON（见 §5 契约） |
| 消息工具 | `filter/merge/trim_messages/get_buffer_string` | **M7 占位**，M1 不定 |

## 3. 命名空间与目录布局

```
include/cppchain/message/
  content_block.hpp   // ContentBlock 判别联合 + 各块 + ToolCall/ToolCallChunk
  message.hpp         // Message(Role 判别) + MessageChunk
  serializer.hpp      // nlohmann ADL: to_json/from_json（M1 唯一变体是 [Message, MessageChunk?]）
legacy/方案：只有 Message 参与序列化，Chunk 仅在运行期存在
tests/message/
  message_serialization_test.cpp   // round-trip + 判别
  chunk_merge_test.cpp             // 合并规则
```

C++ 模块：先传统头文件（`#pragma once`），暂不启用 `std` modules（工具链/构建复杂度不值当）。

## 4. 核心类型设计

### 4.1 Role（判别式）

```cpp
enum class Role { System, Human, Ai, Tool, Chat };  // + string 映射；Chat 附自定义 role 字符串
```

不沿用继承体系（BaseMessage 子类化），用**单一 struct + Role 判别**，对齐 langchain 的 wire 判别模型、JSON 化零成本。

### 4.2 ContentBlock（判别联合）

```cpp
namespace cppchain::message {
struct TextBlock      { std::string text;                    std::optional<std::string> id; };
struct ReasoningBlock { std::string reasoning;               std::optional<std::string> id; };
struct ToolCallBlock  { std::string id; std::string name; nlohmann::json args; int index = 0; };
struct InvalidCallBlock{ std::string id; std::string name; std::string error; };
using ContentBlock = std::variant<TextBlock, ReasoningBlock, ToolCallBlock, InvalidCallBlock>;
}
```

- 判别：variant index ↔ `type` 字面量 `"text"/"reasoning"/"tool_call"/"invalid_tool_call"`（含 `index`/`extras` 扩展槽）。
- **v1 砍掉**：`image/video/audio/text-plain/file/non_standard`、`tool_call_chunk`（chunk 只在流式运行期存在，不进 Message）、`server_tool_*`（OpenAI Responses 接上再说）。

### 4.3 Message

```cpp
struct Message {
  Role type;                                   // 判别（system/human/ai/tool/chat）
  std::vector<ContentBlock> content;           // 纯文本日减为单个 TextBlock（见 §5 normalize）
  std::optional<std::string> id;
  std::optional<std::string> name;
  nlohmann::json response_metadata = {};       // 原生响应元数据（原样透传）
  nlohmann::json additional_kwargs = {};
  // Role::Ai 专用
  std::vector<ToolCall> tool_calls;             // ToolCall = {name,args,id}
  std::vector<InvalidToolCall> invalid_tool_calls;
  std::optional<UsageMetadata> usage;
  // Role::Tool 专用
  std::optional<std::string> tool_call_id;      // 必填回填
  std::string status = "success";               // "success" | "error"（v1 先用 string）
  // Role::Chat 专用
  std::optional<std::string> custom_role;
};
```

复用同一 struct 覆盖全部角色：**协议不变式靠访问器收敛**（如 `as_tool()` 要求 Role::Tool + tool_call_id 存在），比继承树省事且 JSON 化直接。

### 4.4 MessageChunk（流式）

```cpp
struct MessageChunk { /* 同 Message 骨架，但 tool_call 用 ToolCallChunk 陆续累积 */ };
MessageChunk operator+(MessageChunk const& l, MessageChunk const& r);  // 合并规则：
//  - text/reasoning: 拼接
//  - tool_call_chunk: 按 index 累积 name/args
//  - id/index/同名块合并；usage_metadata 相加（对齐 langchain merge）
```

chunk 是否进 AnyMessage 的变体：**v1 不进**，`Message`/`MessageChunk` 明确分开（chunk 仅流式内部用）。

## 5. 序列化契约（M1 的核心契约）

**规范 JSON**（内部状态/持久化/跨组件交换用）：

```jsonc
{ "type": "human",
  "content": [ { "type": "text", "text": "hi" } ],     // 或 {"type":"tool_call","id":"c1","name":"f","args":{...}}
  "id": "u_123", "name": null,
  "tool_call_id": null, "status": "success",
  "usage": { "input_tokens":1, "output_tokens":2, "total_tokens":3, "reasoning_tokens":0 } }
```

- **判别键 `type`**（与 langchain 判别模型一致）；缺失/未知 type → 解析错误（不静默）。
- **normalize**：`content` 允许 `string`，落库/传输统一展开为 `[TextBlock]`；空 = `[]`。
- 契约要求：**状态与消息必须整体 JSON 可序列化**（后续 Checkpointer/Store/Streaming 全依赖这条）。
- OpenAI wire 形态是**另一种视图**，由 M2 的 adapter 负责双向转换，M1 不生产它。

## 6. 协议不变式（测试要锁的）

1. **回填**：出现 tool_calls 的 Ai 消息，其后必须紧跟一组 ToolMessage（按 tool_call_id 一一对应）——本层提供校验工具，M3 强制执行。
2. **round-trip**：`to_json(from_json(x)) == x`，对全部类型/边界（空 content、缺失 optional、非法 type）。
3. **chunk 合并**：text/reasoning 拼接、tool_call 按 index 累积、usage 求和；合并结果收敛为合法 Message。
4. **判别完整性**：`type` 非法或字段缺必需 → 抛明确错误而非默认值。

## 7. 对上层承诺的契约（M2+ 依赖）

| 消费者 | 依赖 |
|---|---|
| M2 ChatModel | 输入/输出即 Message/MessageChunk，JSON 化后可入 wire adapter |
| M3 Tool | ToolCall→ToolMessage 关联、invalid_tool_calls 暴露 |
| M5 State/Checkpointer | 消息可整体 JSON 序列化，供快照与 add_messages 缩减 |
| M7 上下文压缩 | Message 层预留 `filter/merge/trim` 扩展点，本层只定 Message 遍历/改写接口 |

## 8. DoD（完成定义）—— 已全部达成 ✅

- [x] 四类块 + Message/Role/ToolCall/USage + MessageChunk(合并) 类型落地，`-std=c++26` 编译零警告
- [x] round-trip + 判别 + 合并 rule 测试全绿（`tests/message/`，15 项）
- [x] `examples/` 四个聚焦 demo：`messages_basic`（类型族）/ `messages_serialize`（序列化+落盘）/ `messages_tool_flow`（工具协议+不变式）/ `messages_stream_chunk`（流式合并）
- [x] `src/CMakeLists` 增加 `cppchain_message` 静态库目标；smoke 保留

## 9. 开放点（进 03-detail-design 时定）

1. `Message`/`ToolMessage` 是否拆独立 struct（现推荐合一 + Role 判别，理由：JSON 化简单、无虚继承）
2. `ToolCallChunk` 的 args 累积：JSON 片段 vs 累积字符串（对齐响应的增量协议再定）
3. `response_metadata`/`additional_kwargs` 是否真的原样 json（避免过度建模，先透传）