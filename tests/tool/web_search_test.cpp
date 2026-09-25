// M3 测试：真实联网搜索工具（feedcoop global search，FakeTransport 离线跑）
// 参考 simple-agent 的 handle_web_search。验证：请求体形状 / 响应解析 / 错误路径。
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

ToolSpec make_spec(std::string key = "feedcoop-aaaaaaaa") {
  WebSearchConfig cfg;
  cfg.api_key = std::move(key);
  return make_web_search_tool(std::make_shared<http::FakeTransport>(), std::move(cfg));
}

}  // namespace

TEST(WebSearch, request_shape_matches_feedcoop_protocol) {
  WebSearchConfig cfg;
  cfg.api_key = "k";
  cfg.doc_count = 8;
  auto body = build_web_search_request(cfg, "今天有什么 AI 热点新闻？");

  EXPECT_EQ(body.at("query").get<std::string>(), "今天有什么 AI 热点新闻？");
  EXPECT_EQ(body.at("doc_count").get<long>(), 8);                // 发送配置的文档数
  EXPECT_EQ(body.at("max_snippet_length").get<long>(), 200);     // 片段截断
  EXPECT_EQ(body.at("max_image_count_per_doc").get<long>(), 0);  // 不带图片
}

TEST(WebSearch, parses_documents_to_normalized_output) {
  json raw;
  raw["Result"]["TotalDocCount"] = 1;
  json doc;
  doc["Title"] = "示例新闻 A";
  doc["Url"] = "https://example.com/a";
  doc["HostInfo"]["Hostname"] = "示例站";
  doc["DocumentInfo"]["PublishTime"] = "2025-01-01T00:00:00+08:00";
  doc["Snippet"] = json::array();
  doc["Snippet"].push_back({{"Type", "text"}, {"Text", "这是正文第一段。"}});
  doc["Snippet"].push_back({{"Type", "image"}, {"Text", "这段是图片说明，应被忽略"}});
  raw["Result"]["Documents"].push_back(doc);

  auto out = parse_web_search_response(raw, "AI 热点");
  EXPECT_EQ(out.at("query").get<std::string>(), "AI 热点");
  EXPECT_EQ(out.at("total").get<long>(), 1);
  ASSERT_EQ(out.at("documents").size(), 1u);
  auto const& d = out.at("documents")[0];
  EXPECT_EQ(d.at("title").get<std::string>(), "示例新闻 A");
  EXPECT_EQ(d.at("url").get<std::string>(), "https://example.com/a");
  EXPECT_EQ(d.at("hostname").get<std::string>(), "示例站");
  EXPECT_EQ(d.at("publish_time").get<std::string>(), "2025-01-01T00:00:00+08:00");
  EXPECT_EQ(d.at("snippet").get<std::string>(), "这是正文第一段。");  // 只取 text 型片段
}

TEST(WebSearch, parses_wrapped_GlobalSearchResp_variant) {
  json raw;
  raw["Result"]["TotalDocCount"] = 1;
  json doc;
  doc["Title"] = "旧 spec 包装";
  doc["Snippet"] = json::array();
  doc["Snippet"].push_back({{"Type", "text"}, {"Text", "包装层也能解析"}});
  raw["Result"]["GlobalSearchResp"]["Documents"].push_back(doc);

  auto out = parse_web_search_response(raw, "q");
  ASSERT_EQ(out.at("documents").size(), 1u);
  EXPECT_EQ(out.at("documents")[0].at("title").get<std::string>(), "旧 spec 包装");
}

TEST(WebSearch, handler_round_trip_via_transport) {
  auto tp = std::make_shared<http::FakeTransport>();
  json body;
  body["Result"]["TotalDocCount"] = 1;
  json doc;
  doc["Title"] = "langchain 社区动态";
  doc["Url"] = "https://example.com/langchain";
  doc["Snippet"] = json::array();
  doc["Snippet"].push_back({{"Type", "text"}, {"Text", "本月发布新版本。"}});
  body["Result"]["Documents"].push_back(doc);

  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {body.dump()};
  tp->push(std::move(s));

  WebSearchConfig cfg;
  cfg.api_key = "********-********-***";
  auto spec = make_web_search_tool(tp, cfg);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"query", "langchain 最新动态"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("query").get<std::string>(), "langchain 最新动态");
  ASSERT_EQ(r->at("documents").size(), 1u);
  EXPECT_EQ(r->at("documents")[0].at("snippet").get<std::string>(), "本月发布新版本。");

  auto const& reqs = tp->requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_NE(reqs[0].url.find("search_api/global_search"), std::string::npos);
  bool has_bearer = false;
  for (auto const& h : reqs[0].headers)
    if (h.find(cfg.api_key) != std::string::npos) has_bearer = true;
  EXPECT_TRUE(has_bearer) << "请求必须携带配置的 API Key（Bearer）";
  EXPECT_NE(reqs[0].body.find("\"doc_count\""), std::string::npos);
  EXPECT_NE(reqs[0].body.find("\"query\""), std::string::npos);
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

TEST(WebSearch, empty_documents_is_reported) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 200;
  json body;
  body["Result"]["TotalDocCount"] = 0;
  body["Result"]["Documents"] = json::array();
  s.chunks = {body.dump()};
  tp->push(std::move(s));
  WebSearchConfig cfg;
  cfg.api_key = "k";
  auto spec = make_web_search_tool(tp, cfg);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"query", "无结果查询"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("未返回有效内容"), std::string::npos);
}