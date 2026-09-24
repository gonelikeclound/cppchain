# 01 · Message 层详细设计（M1 实现）

> 承接 `02-arch-design/01-message层架构设计.md`。本文记录**已定案**的类型设计、序列化契约实现、
> chunk 合并算法与实现踩坑。示例见 `examples/messages_*` 四个聚焦 demo，测试见 `tests/message/`。

## 1. 最终类型清单（`cppchain::message`）

| 类型 | 说明 |
|---|---|
| `Role` | `System/Human/Ai/Tool/Chat` + `role_str/role_from_str` |
| `UsageMetadata` | `input/output/total/reasoning_tokens`（简化版） |
| `TextBlock` / `ReasoningBlock` | `text`/`reasoning` + 可选 `id` |
| `ToolCall` | `{id, name, args: json}` —— **兼任 content 里的 tool_call 块**（同一数据两种视图，与 langchain 一致） |
| `InvalidToolCall` | `{id, name, error}` |
| `ContentBlock` | `std::variant<TextBlock, ReasoningBlock, ToolCall, InvalidToolCall>` |
| `Message` | 单一 struct + `Role` 判别；`tool_call_id/status`、`tool_calls/invalid_tool_calls/usage`、`custom_role` 为按角色的可选槽 |
| `MessageChunk` / `ToolCallChunk` | 流式增量；`operator+` 合并；`finalize_chunk` 收敛 |
| `validate_sequence` | tool_calls 回填不变式校验 |
| `to_json/from_json` | nlohmann ADL，判别式规范化 |

## 2. arch §9 开放点 → 定案

1. **ToolMessage 拆不拆** → **不拆**。单一 `Message` + Role 判别：JSON 化零成本、无虚继承；tool 专用字段以 `optional` 收敛，访问器强制约束。
2. **ToolCallChunk args 累积方式** → **增量 JSON 片段字符串，按序拼接**，`finalize_chunk` 时一次性 `json::parse`；解析失败/缺 name → 归入 `invalid_tool_calls`。
3. **元数据是否建模** → **原样透传 json**（`response_metadata`/`additional_kwargs` 不做字段级建模）。
4. **额外定案**：`MessageChunk` **不进**规范序列化（流式运行期对象，收敛为 `Message` 后才可序列化）。

## 3. 序列化契约（实现）

- **规范 JSON**：判别键 `type`；`Message` 顶层含 `content/id/name/status/tool_call_id`，条件字段 `tool_calls/invalid_tool_calls/usage/response_metadata/additional_kwargs/custom_role` 仅在非空/存在时输出。
- **normalize**：`content` 统一为块数组；纯文本 = `[TextBlock]`。
- 未知 `type` / 未知块类型 → `std::invalid_argument`（不静默）。
- `round-trip`：`get<Message>(to_json(m)) == m`（依赖 `Message::operator==` 默认化）。

## 4. chunk 合并算法（`operator+` 语义）

text 块相邻拼接；不同块追加。tool_call_chunks 按 `index` 累积：name/id 首见非空即取、args 片段 `+=` 拼接；usage 相加；id/name 取左优先；`tool_call_id/status` 右覆盖。`finalize_chunk`：排序后逐 chunk 收敛为 `ToolCall`（name 缺失或 args 解析失败 → `InvalidToolCall`）。

## 5. 实现踩坑（全部已绕过，勿重踩）

1. **nlohmann 不原生支持 `std::optional`**（走数组构造路径编译失败）。→ 在 serializer.hpp 补通用 `nlohmann::adl_serializer<std::optional<T>>` 特化。
2. **C++20 设计符初始化必须按声明顺序**（GCC 硬报 `designator order ... does not match`）。→ 测试/demo 改用小助手函数 + 成员赋值。
3. **`-Wmissing-field-initializers`**（`-Wextra` 下）→ 给 `id` 加默认成员初始化器（NSDMI）消除；设计符缩略写法尽量避免。
4. **nlohmann 在 C++26 下自身告警**（`std::is_trivial` deprecated）→ 第三方头标 `SYSTEM BEFORE PUBLIC` include 压制，保住"零告警"目标。
5. **`gtest_discover_tests` 报错** → 需先 `include(GoogleTest)`（CMake 模块）。
6. **规范 JSON 的可选字段会显式写 `null`**（如 `"id": null`）——nlohmann 的 `value(key, default)`
   只在**键缺失**时用默认值，值为 null 时会返回 null 并在转 string 时抛 `type_error`。
   → 提供公共助手 **`message::opt_string(j, key)`**；凡读可选字段一律用它（本项目 demo 曾两次踩此坑）。
7. **`Message` 不能默认化 `operator<=>`**（成员含 nlohmann::json，无间距比较）→ 默认化 `operator==` 即可（json 有 `==`）。

## 6. 验证结论

- `ctest`：**15/15 通过**（round-trip、判别、合并、不变式、type 传播回归）。
- `examples/` 四个聚焦 demo 全绿（exit 0）：`messages_basic` / `messages_serialize` /
  `messages_tool_flow` / `messages_stream_chunk`。
- 流式片段收敛出合法的 `get_weather(city=SF)`；协议反例（缺 ToolMessage）被 `validate_sequence` 拦截。
- **demo 反哺修复**：`messages_stream_chunk` 暴露出 `MessageChunk::operator+` 不传播 `type`
  （acc 默认 Human 导致合并后角色丢失），已修复并补回归测试。

## 7. 后续（接入 M2 时再评估）

- 多模态块（image/video/audio/file）、`server_tool_*` —— M2 接 OpenAI Responses 再定。
- `filter/merge/trim_messages` 三件套 —— M7 上下文压缩时落地，但消息遍历/改写接口已在 Message 层成型。