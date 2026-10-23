// demo: chat_invoke —— ChatModel 的同步 / 异步(协程) 调用（离线仿真传输）
//
// langchain 参照：
//   model.invoke(messages)            → 一次调用拿到完整回复
//   await model.ainvoke(messages)     → 异步可等待
// 本 demo 用 FakeTransport（脚本化 SSE 帧）离线演示，不依赖网络与 API key。
// 接真实端点（DeepSeek 等）由 curl_multi 传输提供，见后续 examples。
#include <cppchain/http/fake_transport.hpp>
#include <cppchain/model/openai/openai_chat_model.hpp>

#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::model;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }

// 一段典型的 OpenAI 兼容流式响应（服务端会分多帧下发）
http::FakeTransport::Script chat_script() {
  http::FakeTransport::Script s;
  s.chunks = {
      sse(R"({"choices":[{"delta":{"content":"SF 今天"},"finish_reason":null}]})"),
      sse(R"({"choices":[{"delta":{"content":"晴，24°C。"},"finish_reason":null}]})"),
      sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"),
      sse(R"({"choices":[],"usage":{"prompt_tokens":18,"completion_tokens":9,"total_tokens":27}})"),
      "data: [DONE]\n\n",
  };
  return s;
}

Task<std::string> ask_async(std::shared_ptr<ChatModel> model, std::string question) {
  std::vector<message::Message> msgs{message::human_message(question)};
  auto r = co_await model->invoke_async(msgs, CallOptions{});
  if (!r) co_return "调用失败: " + std::string(kind_str(r.error().kind));
  co_return r->text();
}

}  // namespace

int main() {
  auto transport = std::make_shared<http::FakeTransport>();
  ModelConfig cfg;
  cfg.api_key = "demo-key";
  cfg.base_url = "https://api.example.test/v1";
  cfg.model = "demo-model";
  auto model = std::make_shared<openai::OpenAIChatModel>(cfg, transport);

  // ---- 1. 同步 invoke ----
  transport->push(chat_script());
  std::vector<message::Message> msgs{message::human_message("旧金山的天气？")};
  auto r = model->invoke(msgs, CallOptions{});
  if (r) {
    std::println("[同步 invoke] {}", r->text());
    std::println("             finish_reason={} usage total={}",
                 r->response_metadata.value("finish_reason", "?"),
                 r->usage ? r->usage->total_tokens : 0);
  }

  // ---- 2. 协程：co_await 异步调用 ----
  transport->push(chat_script());
  std::println("[协程 ainvoke] {}", ask_async(model, "再来一次").wait());

  // ---- 3. 错误是常态：expected 承载 ----
  transport->push(http::FakeTransport::Script{.status = 401, .chunks = {R"({"error":"invalid api key"})"},
                                              .headers = {}, .error = {}, .timeout = false, .delay_ms = 0});
  auto bad = model->invoke(msgs, CallOptions{});
  if (!bad)
    std::println("[错误处理] kind={} http={} msg={}", kind_str(bad.error().kind),
                 bad.error().http_status, bad.error().message);

  // ---- 4. 请求长什么样（可对照 curl 调试） ----
  auto const& req = transport->requests().front();
  std::println("--- 实际请求 ---");
  std::println("POST {}", req.url);
  std::println("body: {}", req.body);
  return 0;
}