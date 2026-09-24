// M4(向量) 测试：DashScope embedding 适配器（离线 FakeTransport）
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cppchain/http/fake_transport.hpp"
#include "cppchain/model/dashscope/embeddings.hpp"

using namespace cppchain;
using namespace cppchain::model;

namespace {

// 造一个符合 DashScope 格式的响应；order 用于模拟"返回乱序"
std::string response_json(std::size_t n, int dim = 4, bool reversed = false,
                          long tokens = 7) {
  nlohmann::json embs = nlohmann::json::array();
  for (std::size_t i = 0; i < n; ++i) {
    nlohmann::json vec = nlohmann::json::array();
    for (int d = 0; d < dim; ++d) vec.push_back(static_cast<float>(i) + 0.5f * d);
    embs.push_back({{"text_index", static_cast<int>(i)}, {"embedding", vec}});
  }
  if (reversed) std::reverse(embs.begin(), embs.end());
  nlohmann::json body = {{"output", {{"embeddings", embs}}},
                         {"usage", {{"total_tokens", tokens}}},
                         {"request_id", "req-1"}};
  return body.dump();
}

struct Fixture {
  std::shared_ptr<http::FakeTransport> transport = std::make_shared<http::FakeTransport>();
  std::shared_ptr<dashscope::DashScopeEmbeddings> emb;

  Fixture() {
    EmbeddingConfig cfg;
    cfg.api_key = "test-emb-key";
    cfg.base_url = "https://emb.example.test";
    cfg.model = "qwen3.7-text-embedding";
    emb = std::make_shared<dashscope::DashScopeEmbeddings>(cfg, transport);
  }
};

http::FakeTransport::Script script(std::string body, long status = 200) {
  http::FakeTransport::Script s;
  s.status = status;
  s.chunks = {std::move(body)};
  return s;
}

}  // namespace

TEST(DashScopeEmbeddings, request_shape_matches_native_protocol) {
  Fixture f;
  f.transport->push(script(response_json(2)));

  std::vector<std::string> texts{"天气查询", "股票价格"};
  auto r = f.emb->embed(texts);
  ASSERT_TRUE(r.has_value()) << r.error().message;

  ASSERT_EQ(f.transport->requests().size(), 1u);
  auto const& req = f.transport->requests()[0];
  EXPECT_NE(req.url.find("/api/v1/services/embeddings/text-embedding/text-embedding"),
            std::string::npos);
  bool has_auth = false;
  for (auto const& h : req.headers)
    if (h == "Authorization: Bearer test-emb-key") has_auth = true;
  EXPECT_TRUE(has_auth);

  auto body = nlohmann::json::parse(req.body);
  EXPECT_EQ(body.at("model").get<std::string>(), "qwen3.7-text-embedding");
  // 关键：DashScope 用 input.texts（不是 OpenAI 的 input 数组）
  ASSERT_TRUE(body.at("input").contains("texts"));
  EXPECT_EQ(body.at("input").at("texts").size(), 2u);
  EXPECT_EQ(body.at("input").at("texts")[0].get<std::string>(), "天气查询");
}

TEST(DashScopeEmbeddings, reorders_by_text_index) {
  Fixture f;
  f.transport->push(script(response_json(3, 4, /*reversed=*/true)));

  std::vector<std::string> texts{"a", "b", "c"};
  auto r = f.emb->embed(texts);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  ASSERT_EQ(r->size(), 3u);
  // 服务端乱序返回，结果必须按输入顺序还原（第 i 条向量的首个分量 == i）
  for (std::size_t i = 0; i < 3; ++i) {
    ASSERT_EQ((*r)[i].size(), 4u);
    EXPECT_FLOAT_EQ((*r)[i][0], static_cast<float>(i));
  }
}

TEST(DashScopeEmbeddings, batches_automatically_over_limit) {
  Fixture f;
  // 30 条 → 上限 20 → 两批（20 + 10）
  f.transport->push(script(response_json(20)));
  f.transport->push(script(response_json(10)));

  std::vector<std::string> texts;
  for (int i = 0; i < 30; ++i) texts.push_back("text " + std::to_string(i));
  auto r = f.emb->embed(texts);

  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->size(), 30u);
  ASSERT_EQ(f.transport->requests().size(), 2u);
  auto b1 = nlohmann::json::parse(f.transport->requests()[0].body);
  auto b2 = nlohmann::json::parse(f.transport->requests()[1].body);
  EXPECT_EQ(b1.at("input").at("texts").size(), 20u);
  EXPECT_EQ(b2.at("input").at("texts").size(), 10u);
}

TEST(DashScopeEmbeddings, maps_auth_error) {
  Fixture f;
  f.transport->push(script(
      R"({"code":"InvalidApiKey","message":"Invalid API-key provided.","request_id":"r"})", 401));

  auto r = f.emb->embed(std::vector<std::string>{"x"});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ModelError::Kind::Auth);
  EXPECT_EQ(r.error().http_status, 401);
  EXPECT_NE(r.error().message.find("Invalid API-key"), std::string::npos);
}

TEST(DashScopeEmbeddings, maps_parameter_error_with_message) {
  Fixture f;
  f.transport->push(script(
      R"({"code":"InvalidParameter","message":"batch size … not be larger than 20","request_id":"r"})",
      400));

  auto r = f.emb->embed(std::vector<std::string>{"x"});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().http_status, 400);
  EXPECT_NE(r.error().message.find("larger than 20"), std::string::npos);
}

TEST(DashScopeEmbeddings, detects_count_mismatch) {
  Fixture f;
  f.transport->push(script(response_json(1)));  // 请求 3 条，只回 1 条

  auto r = f.emb->embed(std::vector<std::string>{"a", "b", "c"});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ModelError::Kind::Parse);
}

TEST(DashScopeEmbeddings, accumulates_usage_and_dimension) {
  Fixture f;
  f.transport->push(script(response_json(20, 8, false, 11)));
  f.transport->push(script(response_json(5, 8, false, 4)));

  std::vector<std::string> texts;
  for (int i = 0; i < 25; ++i) texts.push_back("t" + std::to_string(i));
  auto r = f.emb->embed(texts);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(f.emb->total_tokens(), 15);
  EXPECT_EQ(f.emb->dimension(), 8u);
}

TEST(Embedding, cosine_similarity_basics) {
  Embedding const a{1.0f, 0.0f};
  Embedding const b{1.0f, 0.0f};
  Embedding const c{0.0f, 1.0f};
  EXPECT_DOUBLE_EQ(cosine_similarity(a, b), 1.0);
  EXPECT_DOUBLE_EQ(cosine_similarity(a, c), 0.0);
  EXPECT_DOUBLE_EQ(cosine_similarity(a, Embedding{1.0f}), 0.0);  // 维度不同 → 0
}