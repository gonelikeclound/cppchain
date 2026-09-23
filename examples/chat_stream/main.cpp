// demo: chat_stream —— 流式增量 + 取消（离线仿真传输）
//
// langchain 参照：
//   for chunk in model.stream(messages): ...        # 同步迭代增量
//   回调式：on_delta / on_done / on_error + 取消
// 本 demo 演示三件事：按帧打印增量、回调式异步流、中途取消。
#include <cppchain/http/fake_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <chrono>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <vector>

using namespace cppchain;
using namespace cppchain::model;

namespace {
std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }

// 分片下发：文本若干片 + 工具参数分片
http::FakeTransport::Script stream_script(int delay_ms) {
  http::FakeTransport::Script s;
  s.delay_ms = delay_ms;
  s.chunks = {
      sse(R"({"choices":[{"delta":{"content":"正在查询 "},"finish_reason":null}]})"),
      sse(R"({"choices":[{"delta":{"content":"旧金山天气…"},"finish_reason":null}]})"),
      sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
          R"("function":{"name":"get_weather","arguments":""}}]}}]})"),
      sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
          R"("function":{"arguments":"{\"city\":"}}]}}]})"),
      sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
          R"("function":{"arguments":"\"SF\"}"}}]}}]})"),
      sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"),
      "data: [DONE]\n\n",
  };
  return s;
}
}  // namespace

int main() {
  auto transport = std::make_shared<http::FakeTransport>();
  ModelConfig cfg;
  cfg.api_key = "demo-key";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "demo-model";
  auto model = std::make_shared<openai::OpenAIChatModel>(cfg, transport);
  std::vector<message::Message> msgs{message::human_message("旧金山天气？")};

  // ---- 1. 同步迭代：边到边打印（像打字机） ----
  transport->push(stream_script(0));
  std::print("[同步流式] ");
  for (auto const& chunk : model->stream(msgs, CallOptions{}))
    for (auto const& b : chunk.content)
      if (auto const* t = std::get_if<message::TextBlock>(&b)) std::print("{}", t->text);
  std::println("");

  // ---- 2. 回调式 + 收敛：拿到工具调用 ----
  transport->push(stream_script(0));
  StreamHandlers h;
  h.on_delta = [](message::MessageChunk const& c) {
    for (auto const& t : c.tool_call_chunks)
      if (t.has_args) std::print("[args+{}]", t.args);
  };
  h.on_done = [](message::Message m) {
    std::println("\n[收敛] 工具调用: {}", m.tool_calls.empty() ? "(无)" : m.tool_calls[0].name);
    for (auto const& tc : m.tool_calls)
      std::println("         id={} args={}", tc.id, tc.args.dump());
  };
  h.on_error = [](ModelError e) { std::println("[错误] {}", kind_str(e.kind)); };
  auto token = model->stream_async(msgs, CallOptions{}, std::move(h));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  (void)token;

  // ---- 3. 中途取消：不再有任何回调 ----
  transport->push(stream_script(40));  // 每片间隔 40ms
  int deltas = 0;
  bool finished = false;
  StreamHandlers h2;
  h2.on_delta = [&deltas](message::MessageChunk const&) { ++deltas; };
  h2.on_done = [&finished](message::Message) { finished = true; };
  h2.on_error = [&finished](ModelError) { finished = true; };

  auto tok2 = model->stream_async(msgs, CallOptions{}, std::move(h2));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));  // 等 1~2 片到达
  tok2.cancel();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 等后台收尾
  std::println("[取消] 收到 {} 片增量后取消, 终态回调触发? {}", deltas, finished ? "是" : "否");
  return 0;
}