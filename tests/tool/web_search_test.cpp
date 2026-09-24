// M3 测试：真实联网搜索工具（火山方舟 Responses · web_search，FakeTransport 离线跑）
// 验证：请求体形状（对照用户提供的文档示例 curl）/ 响应解析 / 错误路径。
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/tool/web_search.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;

namespace {

ToolContext make_ctx() {
  ToolContext ctx;
  ctx.tool_call_id = "call_test";
  return ctx;
}

ToolSpec make_spec(std::string key = "ark-aaaaaaaa-bbbb-cccc-dddd-eeeeffff0000") {
  WebSearchConfig cfg;
  cfg.api_key = std::move(key);
  return make_web_search_tool(std::make_shared<http::FakeTransport>(), std::move(cfg));
}

}  // namespace

TEST(WebSearch, request_shape_matches_documented_curl) {
  WebSearchConfig cfg;
  cfg.api_key = "k";
  cfg.model = "doubao-seed-2-1-pro-260628";
  auto body = build_web_search_request(cfg, "今天有什么 AI 热点新闻？");

  EXPECT_EQ(body.at("model").get<std::string>(), "doubao-seed-2-1-pro-260628");
  EXPECT_FALSE(body.at("stream").get<bool>());  // 工具取 stream=false（一次拿完整 JSON）
  ASSERT_EQ(body.at("input").size(), 1u);
  auto const& user = body.at("input")[0];
  EXPECT_EQ(user.at("role").get<std::string>(), "user");
  EXPECT_EQ(user.at("content")[0].at("type").get<std::string>(), "input_text");
  EXPECT_EQ(user.at("content")[0].at("text").get<std::string>(), "今天有什么 AI 热点新闻？");

  // tools: [{type:web_search, sources:[doubao]}] —— 与用户给的示例 curl 一致
  auto const& tools = body.at("tools");
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].at("type").get<std::string>(), "web_search");
  ASSERT_EQ(tools[0].at("sources").size(), 1u);
  EXPECT_EQ(tools[0].at("sources")[0].get<std::string>(), "doubao");
}

TEST(WebSearch, parses_responses_output_to_answer_and_citations) {
  json raw = {{"id", "resp_1"},
              {"object", "response"},
              {"output",
               json::array({
                   json{{"type", "web_search_call"}, {"id", "ws_1"}, {"status", "completed"}},
                   json{{"type", "message"},
                        {"role", "assistant"},
                        {"content",
                         json::array({
                             json{{"type", "output_text"},
                                  {"text", "今天 AI 领域有两则热点："},
                                  {"annotations",
                                   json::array({
                                       json{{"type", "url_citation"},
                                            {"url", "https://example.com/a"},
                                            {"title", "示例新闻 A"}},
                                   })}},
                             json{{"type", "output_text"},
                                  {"text", "一是模型发布，二是开源社区动态。"}},
                         })}},
               })}};

  auto out = parse_web_search_response(raw, "AI 热点");
  EXPECT_EQ(out.at("query").get<std::string>(), "AI 热点");
  EXPECT_EQ(out.at("answer").get<std::string>(),
            "今天 AI 领域有两则热点：一是模型发布，二是开源社区动态。");
  ASSERT_EQ(out.at("citations").size(), 1u);
  EXPECT_EQ(out.at("citations")[0].at("url").get<std::string>(), "https://example.com/a");
  EXPECT_EQ(out.at("citations")[0].at("title").get<std::string>(), "示例新闻 A");
}

TEST(WebSearch, handler_round_trip_via_transport) {
  auto tp = std::make_shared<http::FakeTransport>();
  json body = {{"output",
                json::array({
                    json{{"type", "message"},
                         {"role", "assistant"},
                         {"content",
                          json::array({
                              json{{"type", "output_text"},
                                   {"text", "回答摘要：豆包搜索确认了该事件于今日发布。"}},
                          })}},
                })}};
  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {body.dump()};
  tp->push(std::move(s));

  WebSearchConfig cfg;
  cfg.api_key = "ark-aaaaaaaa-bbbb-cccc-dddd-eeeeffff0000";
  auto spec = make_web_search_tool(tp, cfg);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"query", "langchain 最新动态"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("answer").get<std::string>(),
            "回答摘要：豆包搜索确认了该事件于今日发布。");
  EXPECT_EQ(r->at("query").get<std::string>(), "langchain 最新动态");

  auto const& reqs = tp->requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_NE(reqs[0].url.find("/api/v3/responses"), std::string::npos);
  bool has_bearer = false;
  for (auto const& h : reqs[0].headers)
    if (h.find(cfg.api_key) != std::string::npos) has_bearer = true;
  EXPECT_TRUE(has_bearer) << "请求必须携带配置的 API Key";
  EXPECT_NE(reqs[0].body.find("\"web_search\""), std::string::npos);
}

TEST(WebSearch, missing_api_key_is_actionable) {
  auto spec = make_spec(/*key=*/"");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"query", "什么新闻"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("WEB_SEARCH_API_KEY"), std::string::npos);
}

TEST(WebSearch, http_auth_error_is_reported) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 401;
  tp->push(std::move(s));
  WebSearchConfig cfg;
  cfg.api_key = "bad-format-key";
  auto spec = make_web_search_tool(tp, cfg);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"query", "新闻"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("HTTP 401"), std::string::npos);
}

TEST(WebSearch, empty_answer_is_reported) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {json{{"output", json::array()}}.dump()};  // 无任何输出
  tp->push(std::move(s));
  WebSearchConfig cfg;
  cfg.api_key = "k";
  auto spec = make_web_search_tool(tp, cfg);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"query", "无结果查询"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("未返回有效内容"), std::string::npos);
}