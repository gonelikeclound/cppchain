# 02 · ChatModel 层架构设计（M2）

> 对应路线图 M2。参照 langchain `providers-and-models`（接口面已核实）。
> 本文件定**层边界与契约**；类级细节留 `03-detail-design/`。

---

## 1. 定位与边界

- **依赖**：M1 `cppchain_message`（Message/MessageChunk/序列化）、nlohmann、libcurl、C++20 协程与线程原语。
- **提供**：ChatModel 抽象、OpenAI 兼容 wire 适配、**异步传输内核**、流式。
- **不做**（划清边界）：

| 不做 | 归属 |
|---|---|
| Tool 抽象 / ToolNode / 工具执行 | M3 |
| `with_structured_output` 真实现 | M4 |
| 多 provider 原生协议（Anthropic/Gemini） | 仅留 `provider:model` 路由扩展点 |
| 缓存 / 限流 / 复杂重试 / 批量 batch | 后续 |
| 多模态输入（image/audio/video） | 延后 |

## 2. 参照：langchain ChatModel 接口面（已核实）

- `init_chat_model("provider:model")` —— provider 路由；模型名直接透传（新模型无需升级库）。
- 统一能力面：`invoke / stream / batch / bind_tools / with_structured_output`。
- **`ChatOpenAI(model=…, base_url=…)` 接所有 OpenAI 兼容端点** —— 支撑我们"只做 OpenAI 兼容"的选型。
- 官方 caveat：`ChatOpenAI` 只保证官方规范，**第三方非标准字段不提取/不保留** → 我们以
  `Message::response_metadata` **原样透传**非标准字段（M1 已支持），比 langchain 更宽松。

## 3. 前置结论：C++26 异步设施实测（GCC 16，本机验证）

| 设施 | 标准 | 实测 |
|---|---|---|
| `std::execution`（P2300 senders/receivers） | C++26 | ❌ **未实现**（`just/when_all/task/static_thread_pool` 全无） |
| 协程机制（自研 `task<T>`） | C++20 | ✅ |
| `std::jthread` / `std::stop_token` | C++20 | ✅（取消用） |
| `std::latch` / `counting_semaphore` | C++20 | ✅ |
| `std::generator` | C++23 | ✅（同步流式用） |
| `std::future` / `std::async` | C++11 | ✅（组合弱，非首选） |

**结论**：async 接口自己做——**回调内核 + 自研 `task<T>` 协程糖**，不依赖 `std::execution`。

## 4. 分层设计

```
├─ 便捷门面（同步）   invoke() / stream()                ← 内部驱动事件循环，脚本友好
├─ 回调层（核心）     stream_async(msgs, opts, StreamHandlers) -> CancelToken
│                       on_delta(MessageChunk) / on_done(Message) / on_error(ModelError)
├─ 协程糖（可选）     co_await model.invoke_async(msgs, opts) -> expected<Message, ModelError>
│                       自研 task<T> + awaiter，包住回调内核
└─ 传输内核           libcurl multi + 单 IO 线程事件循环
                        非阻塞、socket 级回调；一线程驱动 N 路并发
```

决策依据：流式天然是事件（SSE 每个 `data:` 一个 delta）；`curl_multi` 是并发底座
（M3 并行工具调用、M8 多 subagent 并行都要它）；取消用 `std::stop_token` 包装。

## 5. 核心类型（接口草案）

```cpp
namespace cppchain::model {

struct ModelError {                       // 错误常态：expected 承载
  enum class Kind { Network, Timeout, Auth, RateLimit, Server, Parse, Cancelled, Unknown };
  Kind kind; int http_status = 0; std::string message; nlohmann::json raw;
};

struct CallOptions {                      // 调用参数（未建模的通过 extra 透传）
  std::optional<std::string> model;
  std::optional<double> temperature;
  std::optional<int> max_tokens;
  std::vector<std::string> stop;
  std::optional<std::string> tool_choice;
  nlohmann::json tools;                   // M2 仅 wire 通道（M3 引入 Tool 抽象）
  nlohmann::json extra = nlohmann::json::object();
};

struct StreamHandlers {
  std::function<void(MessageChunk const&)> on_delta;
  std::function<void(Message)>             on_done;   // 已 finalize 的完整消息
  std::function<void(ModelError)>          on_error;
};

class CancelToken;                        // 包装 std::stop_token

class ChatModel {                         // 抽象
 public:
  virtual ~ChatModel() = default;
  virtual std::expected<Message, ModelError> invoke(std::span<Message const>, CallOptions) = 0;
  virtual std::generator<MessageChunk>       stream(std::span<Message const>, CallOptions) = 0;
  virtual CancelToken stream_async(std::span<Message const>, CallOptions, StreamHandlers) = 0;
  virtual Task<std::expected<Message, ModelError>> invoke_async(std::span<Message const>, CallOptions) = 0;
};

// 工厂：只实现 openai 分支，其余留给扩展点
std::unique_ptr<ChatModel> init_model(std::string const& "provider:model", ModelConfig);

}  // namespace cppchain::model
```

**传输抽象**（可替换 —— 仿真/真 libcurl 同接口）：

```cpp
namespace cppchain::http {
struct HttpRequest { std::string url, body; std::vector<std::string> headers; };
struct HttpCallbacks { std::function<void(std::string_view)> on_body_chunk;   // 增量字节
                       std::function<void(int status)> on_status;
                       std::function<void(std::string)> on_error; };
class Transport { public: virtual CancelToken send(HttpRequest, HttpCallbacks) = 0; };
}
```

## 6. 契约

1. **请求**：M1 `Message` → OpenAI `messages`（`role`/`content`/`tool_calls`/`tool_call_id`）；
   `ai.tool_calls` 映射为 assistant 的 `tool_calls`，`Role::Tool` 映射 `role:"tool"` + `tool_call_id`。
2. **响应**：`choices[0].message` → `Message(Role::Ai)`；`tool_calls` → `ToolCall`（args 非法 →
   M1 的 `InvalidToolCall`）；`usage` → `UsageMetadata`；`finish_reason` 与非标准字段 →
   `response_metadata` 透传。
3. **SSE**：逐行解析 `data: {…}` / `data: [DONE]`，忽略心跳/空行；delta → `MessageChunk`
   （文本块增量 + `tool_call_chunks` 携带 `index` 关联）；终止由 M1 `finalize_chunk` 收敛为 `Message`。
4. **取消**：`CancelToken` 触发 → 从 multi 摘除 handle → **不再触发任何回调**（含 on_error）。
5. **错误映射**：网络→`Network`；超时→`Timeout`；401/403→`Auth`；429→`RateLimit`；5xx→`Server`；
   JSON/SSE 解析失败→`Parse`；取消→`Cancelled`。

## 7. 目录与构建

```
include/cppchain/model/{chat_model.hpp, options.hpp, error.hpp, task.hpp}
include/cppchain/model/openai/{openai_chat_model.hpp, wire.hpp, sse.hpp}
include/cppchain/http/{transport.hpp, curl_multi_transport.hpp, fake_transport.hpp}
src/model/…   src/http/…
tests/model/… tests/http/…
examples/chat_invoke/  examples/chat_stream/
```
CMake：`cppchain_model` 依赖 `cppchain_message`；`CPPCHAIN_BUILD_HTTP=ON` 时启用真 libcurl 传输，
默认可用 **FakeTransport**（脚本化 SSE 帧）→ **不被 libcurl 安装阻塞**，装好后只换传输实现。

## 8. 卡位问题：超长工具返回（M2 早期实验点）

流式长输出是"超长工具返回"问题域的第一现场。M2 先做**观察**：`examples/chat_stream` 记录
长输出的到达节奏与内存占用，为 `04-problems/` 首篇攒素材（是否边流边截断/暂存/落盘，留到专题时定）。

## 9. DoD

- [ ] `ChatModel` 抽象 + OpenAI 兼容适配 + 传输抽象（含 FakeTransport）
- [ ] libcurl multi 传输（IO 线程事件循环、取消、超时）
- [ ] 离线测试全绿：请求构造 / 响应解析 / SSE 分片（含跨包截断）/ chunk 累积 / 取消 / 错误映射
- [ ] 协程糖 `task<T>` + `invoke_async` 可用（含一个 await 示例）
- [ ] demo：`chat_invoke`、`chat_stream`
- [ ] 对 **DeepSeek** 联调一次（含 `deepseek-reasoner` 的 `reasoning_content` → `ReasoningBlock` 映射验证）

## 10. 开放点（进 detail-design 时定）

1. 事件循环归属：全局单例 vs 每模型实例一个（影响并发与生命周期）。
2. `Task<T>` 的调度：awaiter 直接驱动 vs 投递到 IO 线程。
3. DeepSeek `reasoning_content` 的映射细节（实测后再定）。
4. `batch` 是否在 M2 提供（倾向：不提供，留到需要时）。