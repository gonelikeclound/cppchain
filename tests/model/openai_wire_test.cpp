// M2 测试：SSE 增量解析 + OpenAI wire 适配（请求构造/响应解析/delta 解析）
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cppchain/model/openai/sse.hpp"
#include "cppchain/model/openai/wire.hpp"

using namespace cppchain;
using namespace cppchain::model;
using cppchain::model::openai::SseParser;

// ---------- SSE ----------

TEST(Sse, single_event) {
  SseParser p;
  auto r = p.feed("data: {\"a\":1}\n\n");
  ASSERT_EQ(r.data_events.size(), 1u);
  EXPECT_EQ(r.data_events[0], "{\"a\":1}");
  EXPECT_FALSE(r.done);
}

TEST(Sse, cross_packet_split) {
  // 一个事件被拆在两个包里到达 —— 必须正确拼接
  SseParser p;
  auto r1 = p.feed("data: {\"cho");
  EXPECT_TRUE(r1.data_events.empty());
  auto r2 = p.feed("ices\":[]}\n\n");
  ASSERT_EQ(r2.data_events.size(), 1u);
  EXPECT_EQ(r2.data_events[0], "{\"choices\":[]}");
}

TEST(Sse, multiple_events_one_packet) {
  SseParser p;
  auto r = p.feed("data: a\n\ndata: b\n\ndata: c\n\n");
  ASSERT_EQ(r.data_events.size(), 3u);
  EXPECT_EQ(r.data_events[2], "c");
}

TEST(Sse, done_marker_and_crlf) {
  SseParser p;
  auto r = p.feed("data: {\"x\":1}\r\n\r\ndata: [DONE]\r\n\r\n");
  ASSERT_EQ(r.data_events.size(), 1u);
  EXPECT_TRUE(r.done);
}

TEST(Sse, ignores_non_data_fields_and_comments) {
  SseParser p;
  auto r = p.feed(": keep-alive\nevent: ping\nid: 7\ndata: payload\n\n");
  ASSERT_EQ(r.data_events.size(), 1u);
  EXPECT_EQ(r.data_events[0], "payload");
}

TEST(Sse, multiline_data_joined) {
  SseParser p;
  auto r = p.feed("data: line1\ndata: line2\n\n");
  ASSERT_EQ(r.data_events.size(), 1u);
  EXPECT_EQ(r.data_events[0], "line1\nline2");
}

// ---------- wire：出站 ----------

TEST(Wire, message_to_openai_roles_and_text) {
  auto sys = message::system_message("be nice");
  auto j = openai::message_to_openai(sys);
  EXPECT_EQ(j.at("role").get<std::string>(), "system");
  EXPECT_EQ(j.at("content").get<std::string>(), "be nice");

  auto tool = message::tool_message("sunny", "call_1");
  auto jt = openai::message_to_openai(tool);
  EXPECT_EQ(jt.at("role").get<std::string>(), "tool");
  EXPECT_EQ(jt.at("tool_call_id").get<std::string>(), "call_1");
}

TEST(Wire, message_to_openai_tool_calls) {
  auto ai = message::ai_message("checking");
  ai.tool_calls.push_back(message::ToolCall{"call_1", "get_weather",
                                            nlohmann::json{{"city", "SF"}}});
  auto j = openai::message_to_openai(ai);
  ASSERT_TRUE(j.contains("tool_calls"));
  EXPECT_EQ(j["tool_calls"][0]["id"].get<std::string>(), "call_1");
  EXPECT_EQ(j["tool_calls"][0]["function"]["name"].get<std::string>(), "get_weather");
  // arguments 必须是 JSON 字符串（OpenAI 规范）
  auto args_text = j["tool_calls"][0]["function"]["arguments"].get<std::string>();
  EXPECT_EQ(nlohmann::json::parse(args_text).at("city").get<std::string>(), "SF");
}

TEST(Wire, build_request_params_and_extra_passthrough) {
  std::vector<message::Message> msgs{message::human_message("hi")};
  CallOptions opts;
  opts.temperature = 0.2;
  opts.max_tokens = 128;
  opts.extra = nlohmann::json{{"top_p", 0.9}};  // 厂商私有参数透传
  ModelConfig cfg;
  cfg.model = "deepseek-chat";

  auto body = openai::build_request(msgs, opts, cfg, /*stream=*/true);
  EXPECT_EQ(body.at("model").get<std::string>(), "deepseek-chat");
  EXPECT_TRUE(body.at("stream").get<bool>());
  EXPECT_DOUBLE_EQ(body.at("temperature").get<double>(), 0.2);
  EXPECT_EQ(body.at("max_tokens").get<int>(), 128);
  EXPECT_DOUBLE_EQ(body.at("top_p").get<double>(), 0.9);
}

// ---------- wire：入站（非流式） ----------

TEST(Wire, parse_response_text_usage_and_metadata) {
  auto body = nlohmann::json::parse(R"({
    "id": "resp_1", "model": "deepseek-chat",
    "choices": [{"index":0, "finish_reason":"stop",
                 "message":{"role":"assistant","content":"Hello there"}}],
    "usage": {"prompt_tokens":7,"completion_tokens":3,"total_tokens":10}
  })");

  auto m = openai::parse_response(body);
  ASSERT_TRUE(m.has_value()) << m.error().message;
  EXPECT_EQ(m->type, message::Role::Ai);
  EXPECT_EQ(m->text(), "Hello there");
  ASSERT_TRUE(m->usage.has_value());
  EXPECT_EQ(m->usage->input_tokens, 7);
  EXPECT_EQ(m->usage->total_tokens, 10);
  EXPECT_EQ(m->response_metadata.at("finish_reason").get<std::string>(), "stop");
  EXPECT_EQ(m->response_metadata.at("model").get<std::string>(), "deepseek-chat");
}

TEST(Wire, parse_response_tool_calls_and_invalid_args) {
  auto body = nlohmann::json::parse(R"({
    "choices": [{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,
      "tool_calls":[
        {"id":"call_1","type":"function","function":{"name":"get_weather","arguments":"{\"city\":\"SF\"}"}},
        {"id":"call_2","type":"function","function":{"name":"broken","arguments":"{not json"}}
      ]}}]
  })");

  auto m = openai::parse_response(body);
  ASSERT_TRUE(m.has_value()) << m.error().message;
  ASSERT_EQ(m->tool_calls.size(), 1u);
  EXPECT_EQ(m->tool_calls[0].name, "get_weather");
  EXPECT_EQ(m->tool_calls[0].args.at("city").get<std::string>(), "SF");
  // args 非法 → 进 invalid_tool_calls（不静默丢弃）
  ASSERT_EQ(m->invalid_tool_calls.size(), 1u);
  EXPECT_EQ(m->invalid_tool_calls[0].id, "call_2");
}

TEST(Wire, parse_response_reasoning_content_to_reasoning_block) {
  // DeepSeek reasoner 风格
  auto body = nlohmann::json::parse(R"({
    "choices": [{"finish_reason":"stop",
      "message":{"role":"assistant","reasoning_content":"let me think","content":"42"}}]
  })");
  auto m = openai::parse_response(body);
  ASSERT_TRUE(m.has_value()) << m.error().message;
  ASSERT_EQ(m->content.size(), 2u);
  EXPECT_EQ(std::get<message::ReasoningBlock>(m->content[0]).reasoning, "let me think");
  EXPECT_EQ(std::get<message::TextBlock>(m->content[1]).text, "42");
}

TEST(Wire, parse_response_passes_through_nonstandard_fields) {
  auto body = nlohmann::json::parse(R"({
    "choices": [{"finish_reason":"stop",
      "message":{"role":"assistant","content":"x","vendor_private":{"k":1}}}]
  })");
  auto m = openai::parse_response(body);
  ASSERT_TRUE(m.has_value());
  // 非标准字段透传（langchain 的 ChatOpenAI 会丢弃，我们保留）
  ASSERT_TRUE(m->response_metadata.contains("extra_vendor_private"));
  EXPECT_EQ(m->response_metadata["extra_vendor_private"]["k"].get<int>(), 1);
}

TEST(Wire, parse_response_error_on_missing_choices) {
  auto m = openai::parse_response(nlohmann::json::parse(R"({"error":"boom"})"));
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().kind, ModelError::Kind::Parse);
}

// ---------- wire：入站（流式 delta） ----------

TEST(Wire, parse_chunk_text_and_finish_reason) {
  auto ev = nlohmann::json::parse(
      R"({"choices":[{"delta":{"content":"Hel"},"finish_reason":null}]})");
  std::string fr;
  auto c = openai::parse_chunk(ev, &fr);
  ASSERT_TRUE(c.has_value()) << c.error().message;
  ASSERT_EQ(c->content.size(), 1u);
  EXPECT_EQ(std::get<message::TextBlock>(c->content[0]).text, "Hel");

  auto ev2 = nlohmann::json::parse(
      R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})");
  auto c2 = openai::parse_chunk(ev2, &fr);
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(fr, "tool_calls");
}

TEST(Wire, parse_chunk_tool_call_fragments) {
  auto ev = nlohmann::json::parse(
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
      R"("function":{"name":"get_weather","arguments":"{\"cit"}}]}}]})");
  auto c = openai::parse_chunk(ev);
  ASSERT_TRUE(c.has_value()) << c.error().message;
  ASSERT_EQ(c->tool_call_chunks.size(), 1u);
  EXPECT_EQ(c->tool_call_chunks[0].index, 0);
  EXPECT_EQ(c->tool_call_chunks[0].id, "call_1");
  EXPECT_EQ(c->tool_call_chunks[0].name, "get_weather");
  EXPECT_EQ(c->tool_call_chunks[0].args, "{\"cit");
}

TEST(Wire, parse_chunk_usage_only_frame) {
  auto ev = nlohmann::json::parse(
      R"({"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":2,"total_tokens":7}})");
  auto c = openai::parse_chunk(ev);
  ASSERT_TRUE(c.has_value());
  ASSERT_TRUE(c->usage.has_value());
  EXPECT_EQ(c->usage->total_tokens, 7);
}
