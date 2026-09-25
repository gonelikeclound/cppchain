// M3 测试：arxiv_search 论文检索工具（arXiv Atom API，FakeTransport 离线跑）
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/tool/arxiv.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;

namespace {

ToolContext make_ctx() {
  ToolContext c;
  c.tool_call_id = "call_arxiv";
  return c;
}

// 一段典型的 arXiv Atom feed（2 篇论文，带命名空间 totalResults 与 primary_category）
constexpr std::string_view kFeed = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <title>ArXiv Query: all:transformer</title>
  <id>http://arxiv.org/api/query?search_query=all:transformer</id>
  <updated>2024-06-01T00:00:00Z</updated>
  <opensearch:totalResults xmlns:opensearch="http://a9.com/-/spec/opensearch/1.1/">2</opensearch:totalResults>
  <entry>
    <id>http://arxiv.org/abs/1706.03762v7</id>
    <updated>2023-07-08T17:18:00Z</updated>
    <published>2017-06-12T00:00:00Z</published>
    <title>
      Attention Is All You Need
    </title>
    <summary>
      We propose a new simple network architecture, the Transformer,
      based solely on attention mechanisms.
    </summary>
    <author><name>Ashish Vaswani</name></author>
    <author><name>Noam Shazeer</name></author>
    <author><name>Niki Parmar</name></author>
    <category term="cs.LG" scheme="http://arxiv.org/schemas/atom"/>
    <arxiv:primary_category xmlns:arxiv="http://arxiv.org/schemas/atom" term="cs.CL" scheme="http://arxiv.org/schemas/atom"/>
  </entry>
  <entry>
    <id>http://arxiv.org/abs/1801.10198v5</id>
    <updated>2019-10-14T02:44:00Z</updated>
    <published>2018-01-30T00:00:00Z</published>
    <title>Universal Language Model Fine-tuning for Text Classification</title>
    <summary>We describe ULMFiT, an effective transfer learning method.</summary>
    <author><name>Jeremy Howard</name></author>
    <author><name>Sebastian Ruder</name></author>
    <category term="cs.CL" scheme="http://arxiv.org/schemas/atom"/>
  </entry>
</feed>)";

ToolSpec make_spec(std::shared_ptr<http::FakeTransport> tp) {
  return make_arxiv_search_tool(std::move(tp));
}

}  // namespace

// ---- build_arxiv_query ----

TEST(Arxiv, BuildQuerySpacesBecomePlusAndKeepsSyntax) {
  ArxivConfig cfg;
  auto url = build_arxiv_query(cfg, "attention mechanism AND cat:cs.CL", 5);
  EXPECT_NE(url.find("?search_query=attention+mechanism+AND+cat:cs.CL"), std::string::npos);
  EXPECT_NE(url.find("&start=0&max_results=5"), std::string::npos);
  EXPECT_EQ(url.find("http"), std::size_t{0});  // 以 base_url 开头
}

TEST(Arxiv, BuildQueryEncodesQuotesAndPlus) {
  ArxivConfig cfg;
  EXPECT_NE(build_arxiv_query(cfg, "ti:\"deep learning\"", 3).find("%22deep+learning%22"),
            std::string::npos);  // 引号百分号编码
  EXPECT_NE(build_arxiv_query(cfg, "C++", 3).find("C%2B%2B"), std::string::npos);  // '+' 不作空格
  EXPECT_NE(build_arxiv_query(cfg, "R&D", 3).find("R%26D"), std::string::npos);    // 非法字符编码
}

TEST(Arxiv, BuildQueryClampsMaxResults) {
  ArxivConfig cfg;
  EXPECT_NE(build_arxiv_query(cfg, "x", 99).find("&max_results=10"), std::string::npos);
  EXPECT_NE(build_arxiv_query(cfg, "x", 0).find("&max_results=1"), std::string::npos);
}

// ---- parse_arxiv_response ----

TEST(Arxiv, ParsesEntriesIntoPapers) {
  auto j = parse_arxiv_response(kFeed, "all:transformer", 500);
  EXPECT_EQ(j.at("query").get<std::string>(), "all:transformer");
  EXPECT_EQ(j.at("total").get<long>(), 2);
  EXPECT_EQ(j.at("source").get<std::string>(), "arXiv API (Atom)");
  ASSERT_EQ(j.at("papers").size(), 2u);

  auto const& p = j.at("papers")[0];
  EXPECT_EQ(p.at("id").get<std::string>(), "https://arxiv.org/abs/1706.03762");  // 去 v7 + https
  EXPECT_EQ(p.at("title").get<std::string>(), "Attention Is All You Need");
  EXPECT_EQ(p.at("authors").get<std::string>(), "Ashish Vaswani, Noam Shazeer, Niki Parmar");
  EXPECT_EQ(p.at("category").get<std::string>(), "cs.CL");  // 优先 primary_category
  EXPECT_EQ(p.at("published").get<std::string>(), "2017-06-12T00:00:00Z");
  EXPECT_EQ(p.at("summary").get<std::string>(),
            "We propose a new simple network architecture, the Transformer, based solely on "
            "attention mechanisms.");
}

TEST(Arxiv, EmptyResultParsesToTotalZero) {
  std::string const feed =
      R"(<feed xmlns="http://www.w3.org/2005/Atom"><opensearch:totalResults>0</opensearch:totalResults></feed>)";
  auto j = parse_arxiv_response(feed, "q", 500);
  EXPECT_EQ(j.at("total").get<long>(), 0);
  EXPECT_EQ(j.at("papers").size(), 0u);
}

TEST(Arxiv, TruncatesSummaryUtf8Safe) {
  std::string const feed =
      "<feed><entry><id>http://arxiv.org/abs/1</id><title>T</title>"
      "<summary>abcdefghijklmnopqrstuvwxyz</summary></entry></feed>";
  auto j = parse_arxiv_response(feed, "q", 10);
  EXPECT_EQ(j.at("papers")[0].at("summary").get<std::string>(), "abcdefghij…");  // 10 字节截断
}

TEST(Arxiv, AuthorsCappedWhenMany) {
  std::string authors;
  for (int i = 1; i <= 10; ++i)
    authors += "<author><name>Author" + std::to_string(i) + "</name></author>";
  std::string const feed =
      "<feed><entry><id>http://arxiv.org/abs/1</id><title>T</title><summary>S</summary>" +
      authors + "</entry></feed>";
  auto j = parse_arxiv_response(feed, "q", 500);
  std::string const got = j.at("papers")[0].at("authors").get<std::string>();
  EXPECT_NE(got.find("Author1"), std::string::npos);
  EXPECT_NE(got.find("等 2 人"), std::string::npos);  // 8 个后截断，还剩 2 个
}

// ---- handler（FakeTransport）----

TEST(Arxiv, HandlerRoundTripViaTransport) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {std::string(kFeed)};
  tp->push(std::move(s));

  auto spec = make_spec(tp);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"search_query", "all:transformer"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  ASSERT_EQ(r->at("papers").size(), 2u);
  EXPECT_EQ(r->at("papers")[0].at("title").get<std::string>(), "Attention Is All You Need");

  auto const& reqs = tp->requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].method, "GET");
  EXPECT_NE(reqs[0].url.find("search_query=all:transformer"), std::string::npos);
  EXPECT_NE(reqs[0].url.find("export.arxiv.org"), std::string::npos);  // 走配置的 base_url
}

TEST(Arxiv, HandlerReportsEmptyQueryFeddback) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 200;
  s.chunks = {
      R"(<feed xmlns="http://www.w3.org/2005/Atom"><opensearch:totalResults>0</opensearch:totalResults></feed>)"};
  tp->push(std::move(s));
  auto spec = make_spec(tp);

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"search_query", "zzz_no_such"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("未找到"), std::string::npos);
}

TEST(Arxiv, HandlerReportsHttpError) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.status = 422;
  tp->push(std::move(s));
  auto spec = make_spec(tp);

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"search_query", "x"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("HTTP 422"), std::string::npos);
}

TEST(Arxiv, HandlerTransportFailureIsFedBack) {
  auto tp = std::make_shared<http::FakeTransport>();
  http::FakeTransport::Script s;
  s.error = "connection refused";
  tp->push(std::move(s));
  auto spec = make_spec(tp);

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"search_query", "x"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("网络错误"), std::string::npos);
}

TEST(Arxiv, MissingQueryIsArgError) {
  auto spec = make_spec(std::make_shared<http::FakeTransport>());
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("search_query"), std::string::npos);
}

TEST(Arxiv, MaxResultsValidated) {
  auto spec = make_spec(std::make_shared<http::FakeTransport>());
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"search_query", "x"}, {"max_results", 99}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("max_results"), std::string::npos);
}

TEST(Arxiv, ReadOnlySemantics) {
  EXPECT_TRUE(make_spec(std::make_shared<http::FakeTransport>()).read_only);
  EXPECT_TRUE(make_spec(std::make_shared<http::FakeTransport>()).ttl.has_value());
}