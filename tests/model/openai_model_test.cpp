// M2 测试：OpenAI 兼容 ChatModel 行为（全部基于 FakeTransport，离线确定性）
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/model/openai/openai_chat_model.hpp"

using namespace cppchain;
using namespace cppchain::model;
using cppchain::message::Message;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

// 典型流式脚本：文本两片 → finish → [DONE]
http::FakeTransport::Script text_script() {
  http::FakeTransport::Script s;
  s.chunks = {
      sse(R"({"choices":[{"delta":{"content":"Hel"},"finish_reason":null}]})"),
      sse(R"({"choices":[{"delta":{"content":"lo"},"finish_reason":null}]})"),
      sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"),
      kDone,
  };
  return s;
}

struct Fixture {
  std::shared_ptr<http::FakeTransport> transport = std::make_shared<http::FakeTransport>();
  std::shared_ptr<openai::OpenAIChatModel> model;

  Fixture() {
    ModelConfig cfg;
    cfg.api_key = "test-key";
    cfg.base_url = "https://api.example.test/v1";
    cfg.model = "test-model";
    model = std::make_shared<openai::OpenAIChatModel>(cfg, transport);
  }
};

}  // namespace

TEST(OpenAIModel, invoke_accumulates_streamed_text) {
  Fixture f;
  f.transport->push(text_script());

  std::vector<Message> msgs{message::human_message("hi")};
  auto m = f.model->invoke(msgs, CallOptions{});

  ASSERT_TRUE(m.has_value()) << m.error().message;
  EXPECT_EQ(m->text(), "Hello");
  EXPECT_EQ(m->type, message::Role::Ai);
  EXPECT_EQ(m->response_metadata.at("finish_reason").get<std::string>(), "stop");
}

TEST(OpenAIModel, request_shape_and_headers) {
  Fixture f;
  f.transport->push(text_script());

  std::vector<Message> msgs{message::system_message("be nice"), message::human_message("hi")};
  CallOptions opts;
  opts.temperature = 0.5;
  (void)f.model->invoke(msgs, opts);

  ASSERT_EQ(f.transport->requests().size(), 1u);
  auto const& req = f.transport->requests()[0];
  EXPECT_NE(req.url.find("/chat/completions"), std::string::npos);
  bool has_auth = false;
  for (auto const& h : req.headers)
    if (h == "Authorization: Bearer test-key") has_auth = true;
  EXPECT_TRUE(has_auth);

  auto body = nlohmann::json::parse(req.body);
  EXPECT_TRUE(body.at("stream").get<bool>());
  EXPECT_EQ(body.at("messages").size(), 2u);
  EXPECT_DOUBLE_EQ(body.at("temperature").get<double>(), 0.5);
}

TEST(OpenAIModel, stream_yields_deltas) {
  Fixture f;
  f.transport->push(text_script());

  std::vector<Message> msgs{message::human_message("hi")};
  std::vector<std::string> pieces;
  for (auto const& c : f.model->stream(msgs, CallOptions{}))
    for (auto const& b : c.content)
      if (auto const* t = std::get_if<message::TextBlock>(&b)) pieces.push_back(t->text);

  ASSERT_EQ(pieces.size(), 2u);
  EXPECT_EQ(pieces[0], "Hel");
  EXPECT_EQ(pieces[1], "lo");
}

TEST(OpenAIModel, tool_call_fragments_reassemble) {
  Fixture f;
  http::FakeTransport::Script s;
  s.chunks = {
      sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
          R"("function":{"name":"get_weather","arguments":""}}]}}]})"),
      sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
          R"("function":{"arguments":"{\"city\":"}}]}}]})"),
      sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
          R"("function":{"arguments":"\"SF\"}"}}]}}]})"),
      sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"),
      kDone,
  };
  f.transport->push(s);

  std::vector<Message> msgs{message::human_message("weather?")};
  auto m = f.model->invoke(msgs, CallOptions{});

  ASSERT_TRUE(m.has_value()) << m.error().message;
  ASSERT_EQ(m->tool_calls.size(), 1u);
  EXPECT_EQ(m->tool_calls[0].id, "call_1");
  EXPECT_EQ(m->tool_calls[0].name, "get_weather");
  EXPECT_EQ(m->tool_calls[0].args.at("city").get<std::string>(), "SF");
  EXPECT_EQ(m->response_metadata.at("finish_reason").get<std::string>(), "tool_calls");
}

TEST(OpenAIModel, usage_frame_accumulates) {
  Fixture f;
  http::FakeTransport::Script s;
  s.chunks = {
      sse(R"({"choices":[{"delta":{"content":"hi"},"finish_reason":null}]})"),
      sse(R"({"choices":[],"usage":{"prompt_tokens":9,"completion_tokens":2,"total_tokens":11}})"),
      kDone,
  };
  f.transport->push(s);

  std::vector<Message> msgs{message::human_message("hi")};
  auto m = f.model->invoke(msgs, CallOptions{});
  ASSERT_TRUE(m.has_value()) << m.error().message;
  ASSERT_TRUE(m->usage.has_value());
  EXPECT_EQ(m->usage->input_tokens, 9);
  EXPECT_EQ(m->usage->total_tokens, 11);
}

TEST(OpenAIModel, error_mapping_auth) {
  Fixture f;
  http::FakeTransport::Script s;
  s.status = 401;
  s.chunks = {R"({"error":{"message":"invalid api key"}})"};
  f.transport->push(s);

  std::vector<Message> msgs{message::human_message("hi")};
  auto m = f.model->invoke(msgs, CallOptions{});
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().kind, ModelError::Kind::Auth);
  EXPECT_EQ(m.error().http_status, 401);
}

TEST(OpenAIModel, error_mapping_transport_failure) {
  Fixture f;
  http::FakeTransport::Script s;
  s.error = "connection reset by peer";
  f.transport->push(s);

  std::vector<Message> msgs{message::human_message("hi")};
  auto m = f.model->invoke(msgs, CallOptions{});
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().kind, ModelError::Kind::Network);
}

TEST(OpenAIModel, error_mapping_bad_sse_payload) {
  Fixture f;
  http::FakeTransport::Script s;
  s.chunks = {"data: {oops not json}\n\n", kDone};
  f.transport->push(s);

  std::vector<Message> msgs{message::human_message("hi")};
  auto m = f.model->invoke(msgs, CallOptions{});
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().kind, ModelError::Kind::Parse);
}

TEST(OpenAIModel, cancel_mid_stream_stops_callbacks) {
  Fixture f;
  http::FakeTransport::Script s = text_script();
  s.delay_ms = 40;  // 后台线程分片投递
  f.transport->push(s);

  std::vector<Message> msgs{message::human_message("hi")};
  int deltas = 0;
  bool done_called = false;
  bool error_called = false;

  StreamHandlers h;
  h.on_delta = [&deltas](message::MessageChunk const&) { ++deltas; };
  h.on_done = [&done_called](Message) { done_called = true; };
  h.on_error = [&error_called](ModelError) { error_called = true; };

  auto token = f.model->stream_async(msgs, CallOptions{}, std::move(h));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));  // 让第一片到达
  token.cancel();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 等后台线程收尾

  EXPECT_GE(deltas, 1);
  EXPECT_LT(deltas, 4);
  EXPECT_FALSE(done_called);
  EXPECT_FALSE(error_called);  // 取消后不得再触发任何回调
}

TEST(OpenAIModel, cancelled_before_start_delivers_nothing) {
  Fixture f;
  http::FakeTransport::Script s = text_script();
  s.delay_ms = 30;
  f.transport->push(s);

  std::vector<Message> msgs{message::human_message("hi")};
  int callbacks = 0;
  StreamHandlers h;
  h.on_delta = [&callbacks](message::MessageChunk const&) { ++callbacks; };
  h.on_done = [&callbacks](Message) { ++callbacks; };
  h.on_error = [&callbacks](ModelError) { ++callbacks; };

  auto token = f.model->stream_async(msgs, CallOptions{}, std::move(h));
  token.cancel();  // 立即取消
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(callbacks, 0);
}

TEST(OpenAIModel, invoke_async_awaitable) {
  Fixture f;
  f.transport->push(text_script());

  std::vector<Message> msgs{message::human_message("hi")};
  auto result = f.model->invoke_async(msgs, CallOptions{}).wait();
  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_EQ(result->text(), "Hello");
}

// 协程组合：在外层协程里 co_await 模型的异步调用
static Task<std::string> ask(std::shared_ptr<ChatModel> m, std::vector<Message> msgs) {
  auto r = co_await m->invoke_async(msgs, CallOptions{});
  if (!r) co_return std::string("ERR:");
  co_return r->text();
}

TEST(OpenAIModel, coroutine_composition) {
  Fixture f;
  f.transport->push(text_script());
  f.transport->push(text_script());

  std::vector<Message> msgs{message::human_message("hi")};
  EXPECT_EQ(ask(f.model, msgs).wait(), "Hello");
  EXPECT_EQ(ask(f.model, msgs).wait(), "Hello");  // 第二次请求（脚本第二次）
}