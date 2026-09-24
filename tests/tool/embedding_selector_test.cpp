// M4(向量) 测试：EmbeddingSelector（语义排序 / 降级 / 索引）
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cppchain/tool/embedding_selector.hpp"

using namespace cppchain;
using namespace cppchain::model;
using namespace cppchain::tool;

namespace {

bool has(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

// 确定性"语义空间"：三维 = [天气, 股票, 邮件]，中英文都认。
// 用它来验证"关键词检索会漏、向量检索能召"的场景，且完全离线。
class StubEmbeddings final : public Embeddings {
 public:
  std::expected<std::vector<Embedding>, ModelError> embed(
      std::span<std::string const> texts) override {
    ++calls_;
    std::vector<Embedding> out;
    out.reserve(texts.size());
    for (auto const& t : texts) out.push_back(vector_for(t));
    return out;
  }
  [[nodiscard]] std::size_t dimension() const override { return 3; }
  [[nodiscard]] std::size_t max_batch_size() const override { return 20; }
  [[nodiscard]] int calls() const { return calls_; }

  static Embedding vector_for(std::string_view t) {
    auto axis = [&](std::string_view a, std::string_view b) {
      return (has(t, a) || has(t, b)) ? 1.0f : 0.0f;
    };
    Embedding v{axis("weather", "天气"), axis("stock", "股价"), axis("email", "邮件")};
    // 查询里"带伞/下雨"也算天气语义（模拟 embedding 的语义泛化）
    if (has(t, "伞") || has(t, "下雨")) v[0] = 1.0f;
    float norm = 0.0f;
    for (float x : v) norm += x * x;
    if (norm > 0.0f) {
      norm = std::sqrt(norm);
      for (float& x : v) x /= norm;
    }
    return v;
  }

 private:
  int calls_ = 0;
};

class FailingEmbeddings final : public Embeddings {
 public:
  std::expected<std::vector<Embedding>, ModelError> embed(std::span<std::string const>) override {
    return std::unexpected(ModelError{ModelError::Kind::Network, 0, "boom", {}});
  }
  [[nodiscard]] std::size_t dimension() const override { return 3; }
  [[nodiscard]] std::size_t max_batch_size() const override { return 20; }
};

ToolSpec tool(std::string name, std::string desc) {
  return make_tool(std::move(name), std::move(desc), nlohmann::json::object(),
                   [](nlohmann::json const&, ToolContext&) {
                     return std::expected<nlohmann::json, ToolError>{nlohmann::json("ok")};
                   });
}

void fill(ToolRegistry& reg) {
  // 故意用**纯英文**工具名/描述 —— 中文查询时关键词检索会完全失效
  reg.add(tool("get_weather_forecast", "Look up current weather conditions for a city"));
  reg.add(tool("query_stock_quote", "Get the latest stock price"));
  reg.add(tool("send_email_message", "Send an email to a recipient"));
}

}  // namespace

TEST(EmbeddingSelector, semantic_hit_where_keyword_fails) {
  ToolRegistry reg;
  fill(reg);

  // 先确认前提：中文查询下关键词检索确实召回为空（这就是向量检索要解决的问题）
  KeywordSelector keyword;
  EXPECT_TRUE(keyword.select(reg, "明天出门要带伞吗", 3).empty());

  StubEmbeddings embedder;
  EmbeddingSelector sel(embedder);
  ASSERT_TRUE(sel.build_index(reg).has_value());
  EXPECT_TRUE(sel.indexed());
  EXPECT_EQ(sel.indexed_count(), 3u);
  EXPECT_EQ(embedder.calls(), 1);  // 索引一次批量完成

  auto picked = sel.select(reg, "明天出门要带伞吗", 3);
  ASSERT_FALSE(picked.empty());
  EXPECT_EQ(picked[0]->name, "get_weather_forecast");  // 语义命中天气

  auto stock = sel.select(reg, "股价", 3);
  ASSERT_FALSE(stock.empty());
  EXPECT_EQ(stock[0]->name, "query_stock_quote");

  auto mail = sel.select(reg, "我要发邮件", 3);
  ASSERT_FALSE(mail.empty());
  EXPECT_EQ(mail[0]->name, "send_email_message");
}

TEST(EmbeddingSelector, index_text_includes_name_description_and_params) {
  SchemaBuilder sb;
  sb.prop("city", "string", "城市名称");
  auto spec = make_tool("weather_api", "获取气象数据", sb.build(),
                        [](nlohmann::json const&, ToolContext&) {
                          return std::expected<nlohmann::json, ToolError>{nlohmann::json("ok")};
                        });
  auto const text = EmbeddingSelector::index_text(spec);
  EXPECT_NE(text.find("weather_api"), std::string::npos);
  EXPECT_NE(text.find("获取气象数据"), std::string::npos);
  EXPECT_NE(text.find("城市名称"), std::string::npos);
}

TEST(EmbeddingSelector, falls_back_to_keyword_on_query_failure) {
  ToolRegistry reg;
  fill(reg);
  FailingEmbeddings broken;

  EmbeddingSelector::Options opts;
  opts.fallback_to_keyword = true;
  EmbeddingSelector sel(broken, opts);

  // 索引失败（可预期）
  EXPECT_FALSE(sel.build_index(reg).has_value());
  EXPECT_FALSE(sel.indexed());

  // 未建索引 → 降级到关键词（英文查询能命中）
  auto picked = sel.select(reg, "weather", 3);
  ASSERT_FALSE(picked.empty());
  EXPECT_EQ(picked[0]->name, "get_weather_forecast");
  ASSERT_TRUE(sel.last_error().has_value());
  EXPECT_EQ(sel.last_error()->kind, ModelError::Kind::Unknown);
}

TEST(EmbeddingSelector, no_fallback_returns_empty_on_failure) {
  ToolRegistry reg;
  fill(reg);
  FailingEmbeddings broken;

  EmbeddingSelector::Options opts;
  opts.fallback_to_keyword = false;
  EmbeddingSelector sel(broken, opts);

  EXPECT_FALSE(sel.build_index(reg).has_value());
  EXPECT_TRUE(sel.select(reg, "weather", 3).empty());
}

TEST(EmbeddingSelector, topk_limit_respected) {
  ToolRegistry reg;
  fill(reg);
  StubEmbeddings embedder;
  EmbeddingSelector sel(embedder);
  ASSERT_TRUE(sel.build_index(reg).has_value());

  EXPECT_EQ(sel.select(reg, "天气", 1).size(), 1u);
  EXPECT_EQ(sel.select(reg, "天气", 2).size(), 2u);
  EXPECT_EQ(sel.select(reg, "天气", 0).size(), 3u);  // 0 = 不限
}