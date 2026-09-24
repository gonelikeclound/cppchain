// M4 测试：结构化输出（两种策略 / 最小 schema 校验 / 失败回喂重试）
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/model/openai/openai_chat_model.hpp"
#include "cppchain/model/structured.hpp"

using namespace cppchain;
using namespace cppchain::model;
using cppchain::message::Message;

namespace {

std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }
constexpr char kDone[] = "data: [DONE]\n\n";

// 模型"调用伪工具"并给出 args
http::FakeTransport::Script tool_call_script(std::string const& args_json,
                                             std::string const& tool = "structured_output") {
  http::FakeTransport::Script s;
  nlohmann::json delta = {{"tool_calls",
                           nlohmann::json::array({{{"index", 0},
                                                   {"id", "call_1"},
                                                   {"function",
                                                    {{"name", tool}, {"arguments", args_json}}}}})}};
  s.chunks = {
      sse(nlohmann::json{{"choices", nlohmann::json::array({{{"delta", delta}}})}}.dump()),
      sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"),
      kDone,
  };
  return s;
}

// 模型直接输出文本（json_object 策略用）
http::FakeTransport::Script text_script(std::string const& text) {
  http::FakeTransport::Script s;
  s.chunks = {
      sse(nlohmann::json{{"choices",
                          nlohmann::json::array({{{"delta", {{"content", text}}}}})}}
              .dump()),
      sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"),
      kDone,
  };
  return s;
}

nlohmann::json weather_schema() {
  return nlohmann::json{{"type", "object"},
                        {"properties",
                         {{"city", {{"type", "string"}}}, {"temp_c", {{"type", "number"}}}}},
                        {"required", nlohmann::json::array({"city", "temp_c"})}};
}

struct Fixture {
  std::shared_ptr<http::FakeTransport> transport = std::make_shared<http::FakeTransport>();
  std::shared_ptr<openai::OpenAIChatModel> model;

  Fixture() {
    ModelConfig cfg;
    cfg.api_key = "k";
    cfg.base_url = "https://api.example.test/v1";
    cfg.model = "m";
    model = std::make_shared<openai::OpenAIChatModel>(cfg, transport);
  }

  std::vector<Message> ask() { return {message::human_message("旧金山天气？")}; }
};

}  // namespace

// ---------- 最小 schema 校验 ----------

TEST(ValidateJson, type_and_required) {
  auto const schema = weather_schema();

  EXPECT_FALSE(validate_json(schema, {{"city", "SF"}, {"temp_c", 18}}).has_value());

  auto missing = validate_json(schema, {{"city", "SF"}});
  ASSERT_TRUE(missing.has_value());
  EXPECT_NE(missing->find("temp_c"), std::string::npos);

  auto wrong_type = validate_json(schema, {{"city", "SF"}, {"temp_c", "warm"}});
  ASSERT_TRUE(wrong_type.has_value());
  EXPECT_NE(wrong_type->find("number"), std::string::npos);
}

TEST(ValidateJson, enum_and_nested_and_items) {
  nlohmann::json const schema = {
      {"type", "object"},
      {"properties",
       {{"route", {{"type", "string"}, {"enum", nlohmann::json::array({"weather", "stock"})}}},
        {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
      {"required", nlohmann::json::array({"route"})}};

  EXPECT_FALSE(validate_json(schema, {{"route", "weather"}, {"tags", {"a", "b"}}}).has_value());

  auto bad_enum = validate_json(schema, {{"route", "sports"}});
  ASSERT_TRUE(bad_enum.has_value());
  EXPECT_NE(bad_enum->find("enum"), std::string::npos);

  auto bad_item = validate_json(schema, {{"route", "weather"}, {"tags", {"a", 42}}});
  ASSERT_TRUE(bad_item.has_value());
  EXPECT_NE(bad_item->find("item[1]"), std::string::npos);
}

TEST(ValidateJson, root_type_mismatch) {
  nlohmann::json const schema = {{"type", "object"}};
  auto v = validate_json(schema, nlohmann::json::array({1, 2}));
  ASSERT_TRUE(v.has_value());
  EXPECT_NE(v->find("expected type 'object'"), std::string::npos);
}

// ---------- 策略①：强制工具调用 ----------

TEST(StructuredOutput, tool_call_mode_request_shape_and_result) {
  Fixture f;
  f.transport->push(tool_call_script(R"({"city":"San Francisco","temp_c":18})"));

  StructuredOutput so(*f.model, weather_schema());
  auto r = so.invoke(f.ask(), CallOptions{});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("city").get<std::string>(), "San Francisco");
  EXPECT_DOUBLE_EQ(r->at("temp_c").get<double>(), 18);

  auto body = nlohmann::json::parse(f.transport->requests()[0].body);
  ASSERT_TRUE(body.contains("tools"));
  EXPECT_EQ(body["tools"][0]["function"]["name"].get<std::string>(), "structured_output");
  EXPECT_EQ(body["tools"][0]["function"]["parameters"], weather_schema());
  // 强制选择该函数（对象形式）
  EXPECT_EQ(body["tool_choice"]["type"].get<std::string>(), "function");
  EXPECT_EQ(body["tool_choice"]["function"]["name"].get<std::string>(), "structured_output");
}

// ---------- 策略②：json_object ----------

TEST(StructuredOutput, json_object_mode_request_shape_and_result) {
  Fixture f;
  f.transport->push(text_script(R"({"city": "SF", "temp_c": 19})"));

  StructuredOptions opts;
  opts.mode = StructuredMode::JsonObject;
  StructuredOutput so(*f.model, weather_schema(), opts);
  auto r = so.invoke(f.ask(), CallOptions{});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("city").get<std::string>(), "SF");

  auto body = nlohmann::json::parse(f.transport->requests()[0].body);
  EXPECT_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
  EXPECT_FALSE(body.contains("tools"));
  // schema 通过 prompt 传达：最后一条 system 里应包含 schema
  auto const& msgs = body["messages"];
  EXPECT_NE(msgs.dump().find("JSON Schema"), std::string::npos);
}

// ---------- 校验失败 → 回喂重试 ----------

TEST(StructuredOutput, retries_after_schema_violation_and_feeds_back_error) {
  Fixture f;
  f.transport->push(tool_call_script(R"({"city":"SF","temp_c":"warm"})"));  // 类型错
  f.transport->push(tool_call_script(R"({"city":"SF","temp_c":18})"));     // 修正

  StructuredOutput so(*f.model, weather_schema());
  auto r = so.invoke(f.ask(), CallOptions{});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_DOUBLE_EQ(r->at("temp_c").get<double>(), 18);

  ASSERT_EQ(f.transport->requests().size(), 2u);
  // 第二次请求里必须带上"上一次不合规"的纠正指令（回喂）
  auto second = nlohmann::json::parse(f.transport->requests()[1].body);
  EXPECT_NE(second["messages"].dump().find("不符合要求"), std::string::npos);
  EXPECT_NE(second["messages"].dump().find("temp_c"), std::string::npos);
}

TEST(StructuredOutput, retries_after_missing_tool_call) {
  Fixture f;
  f.transport->push(text_script("抱歉，我无法调用工具"));  // 没调工具
  f.transport->push(tool_call_script(R"({"city":"SF","temp_c":18})"));

  StructuredOutput so(*f.model, weather_schema());
  auto r = so.invoke(f.ask(), CallOptions{});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  ASSERT_EQ(f.transport->requests().size(), 2u);
  auto second = nlohmann::json::parse(f.transport->requests()[1].body);
  EXPECT_NE(second["messages"].dump().find("did not call"), std::string::npos);
}

TEST(StructuredOutput, exhausts_retries_and_reports_parse_error) {
  Fixture f;
  StructuredOptions opts;
  opts.max_retries = 1;
  for (int i = 0; i < 2; ++i)
    f.transport->push(tool_call_script(R"({"city":42,"temp_c":18})"));  // city 类型错，始终不合规

  StructuredOutput so(*f.model, weather_schema(), opts);
  auto r = so.invoke(f.ask(), CallOptions{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ModelError::Kind::Parse);
  // 校验器按 field 顺序递归，先命中 city 的类型错误
  EXPECT_NE(r.error().message.find("city"), std::string::npos);
  EXPECT_EQ(f.transport->requests().size(), 2u);  // 1 次 + 1 次重试
}

TEST(StructuredOutput, json_object_mode_retries_on_invalid_json) {
  Fixture f;
  f.transport->push(text_script("这不是 JSON"));
  f.transport->push(text_script(R"({"city":"SF","temp_c":20})"));

  StructuredOptions opts;
  opts.mode = StructuredMode::JsonObject;
  StructuredOutput so(*f.model, weather_schema(), opts);
  auto r = so.invoke(f.ask(), CallOptions{});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(f.transport->requests().size(), 2u);
}

// ---------- route/judge 便捷件 ----------

TEST(StructuredOutput, classify_returns_label_and_enforces_enum) {
  Fixture f;
  f.transport->push(tool_call_script(R"({"label":"weather"})"));

  StructuredOutput so(*f.model, nlohmann::json::object());
  auto r = so.classify(f.ask(), {"weather", "stock", "email"}, CallOptions{});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(*r, "weather");

  // 请求里的 enum 必须来自 labels
  auto body = nlohmann::json::parse(f.transport->requests()[0].body);
  auto const& params = body["tools"][0]["function"]["parameters"];
  EXPECT_EQ(params["properties"]["label"]["enum"].size(), 3u);
  EXPECT_EQ(params["required"][0].get<std::string>(), "label");
}

TEST(StructuredOutput, classify_rejects_out_of_enum_label) {
  Fixture f;
  f.transport->push(tool_call_script(R"({"label":"sports"})"));  // 越界
  f.transport->push(tool_call_script(R"({"label":"stock"})"));

  StructuredOutput so(*f.model, nlohmann::json::object());
  auto r = so.classify(f.ask(), {"weather", "stock"}, CallOptions{});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(*r, "stock");
  EXPECT_EQ(f.transport->requests().size(), 2u);
}