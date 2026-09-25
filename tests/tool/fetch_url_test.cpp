// M3 测试：fetch_url 网页正文提取工具（FakeTransport 离线跑）
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/tool/fetch_url.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;
using std::string;

namespace {

ToolContext make_ctx() {
  ToolContext c;
  c.tool_call_id = "call_fetch";
  return c;
}

// 一段带 script/style/注释/实体的典型 HTML
constexpr std::string_view kHtml = R"(<html>
<head><title>cppchain 首页</title>
<style>.nice{color:red}</style></head>
<body>
<h1>Hello</h1>
<p>第一段文字&amp;符号。</p>
<script>var x = "不该出现";</script>
<!-- 注释也不该出现 -->
<ul><li>要点一</li><li>要点二</li></ul>
</body></html>)";

ToolSpec make_spec(std::shared_ptr<http::FakeTransport> tp) {
  return make_fetch_url_tool(std::move(tp));
}

}  // namespace

TEST(FetchUrl, strips_script_style_comments_and_tags) {
  string const t = html_to_text(kHtml);
  EXPECT_EQ(t.find("不该出现"), string::npos);   // script 已被剥除
  EXPECT_EQ(t.find("nice"), string::npos);       // style 已被剥除
  EXPECT_EQ(t.find("<!--"), string::npos);       // 注释已被剥除
  EXPECT_NE(t.find("第一段文字&符号"), string::npos);  // &amp; 已解码为 &
  EXPECT_NE(t.find("要点一"), string::npos);
}

TEST(FetchUrl, block_tags_break_lines) {
  EXPECT_EQ(html_to_text("<p>甲</p><p>乙</p>"), "甲\n乙");
}

TEST(FetchUrl, collapses_whitespace_and_trims) {
  EXPECT_EQ(html_to_text("<p>  你好    世界  </p>  <p>  下一段</p>"), "你好 世界\n下一段");
}

TEST(FetchUrl, decodes_numeric_entities) {
  EXPECT_EQ(html_to_text("<p>&#20320;&#22909;</p>"), "你好");  // &#20320;=你 &#22909;=好
}

TEST(FetchUrl, extracts_title) {
  EXPECT_EQ(extract_title(kHtml), "cppchain 首页");
  EXPECT_EQ(extract_title("<html><title>  标题  </title></html>"), "标题");
  EXPECT_EQ(extract_title("<html><h1>无标题</h1></html>"), "");
}

TEST(FetchUrl, handler_round_trip_via_transport) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {std::string(kHtml)};
  tp->push(std::move(s));

  auto spec = make_spec(tp);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"url", "https://cppchain.example.io"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("url").get<string>(), "https://cppchain.example.io");
  EXPECT_EQ(r->at("status").get<long>(), 200);
  EXPECT_EQ(r->at("title").get<string>(), "cppchain 首页");
  string const content = r->at("content").get<string>();
  EXPECT_NE(content.find("第一段文字&符号"), string::npos);
  EXPECT_EQ(content.find("不该出现"), string::npos);

  auto const& reqs = tp->requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].url, "https://cppchain.example.io");
  EXPECT_EQ(reqs[0].method, "GET");
}

TEST(FetchUrl, reject_non_http_scheme) {
  auto spec = make_spec(std::make_shared<http::FakeTransport>());
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"url", "ftp://example.com/a"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("http"), string::npos);
}

TEST(FetchUrl, http_error_is_reported) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 404;
  tp->push(std::move(s));
  auto spec = make_spec(tp);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"url", "https://example.com/missing"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("HTTP 404"), string::npos);
}

TEST(FetchUrl, js_only_page_is_reported) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {R"(<html><head><script>window.app=1</script></head><body></body></html>)"};
  tp->push(std::move(s));
  auto spec = make_spec(tp);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"url", "https://example.com/spa"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("没有可提取的正文"), string::npos);
}

TEST(FetchUrl, truncates_utf8_safely_at_char_boundary) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {R"(<html><body><p>你好世界世界世界</p></body></html>)"};
  tp->push(std::move(s));
  FetchUrlConfig cfg;
  cfg.default_max_chars = 4;  // "你"占 3 字节、边界处应为完整字符
  auto spec = make_fetch_url_tool(tp, cfg);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"url", "https://example.com/a"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("content").get<string>(), "你…");
}