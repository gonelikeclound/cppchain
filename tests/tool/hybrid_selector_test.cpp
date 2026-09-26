// M9 测试：HybridSelector（keyword ∪ vector 去重合流 / 纯关键词退化 / 向量失败降级）
#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "cppchain/model/embeddings.hpp"
#include "cppchain/tool/hybrid_selector.hpp"

using namespace cppchain;
using namespace cppchain::model;
using namespace cppchain::tool;

namespace {

bool has(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

// 确定性"语义空间"：三维 = [天气, 股票, 邮件]（复用 embedding_selector_test 的 Stub 思路）
class StubEmbeddings final : public Embeddings {
 public:
  std::expected<std::vector<Embedding>, ModelError> embed(
      std::span<std::string const> texts) override {
    std::vector<Embedding> out;
    for (auto const& t : texts) out.push_back(vector_for(t));
    return out;
  }
  [[nodiscard]] std::size_t dimension() const override { return 3; }
  [[nodiscard]] std::size_t max_batch_size() const override { return 20; }

  static Embedding vector_for(std::string_view t) {
    auto axis = [&](std::string_view a, std::string_view b) {
      return (has(t, a) || has(t, b)) ? 1.0f : 0.0f;
    };
    Embedding v{axis("天气", "weather"), axis("股价", "stock"), axis("邮件", "email")};
    if (has(t, "伞") || has(t, "下雨")) v[0] = 1.0f;  // 语义泛化：伞/下雨≈天气
    float norm = 0.0f;
    for (float x : v) norm += x * x;
    if (norm > 0.0f) {
      norm = std::sqrt(norm);
      for (float& x : v) x /= norm;
    }
    return v;
  }
};

class FailingEmbeddings final : public Embeddings {
 public:
  std::expected<std::vector<Embedding>, ModelError> embed(std::span<std::string const>) override {
    return std::unexpected(ModelError{ModelError::Kind::Network, 0, "boom", {}});
  }
  [[nodiscard]] std::size_t dimension() const override { return 3; }
  [[nodiscard]] std::size_t max_batch_size() const override { return 20; }
};

tool::ToolRegistry sample_registry() {
  tool::ToolRegistry reg;
  reg.add(tool::make_tool("天气查询", "实时查询城市天气天气状况",
                          nlohmann::json::object(),
                          [](nlohmann::json const&, ToolContext&) {
                            return std::expected<nlohmann::json, ToolError>{nlohmann::json("晴")};
                          }));
  reg.add(tool::make_tool("户外建议", "根据天气给户外出行提供穿衣与物品建议",
                          nlohmann::json::object(),
                          [](nlohmann::json const&, ToolContext&) {
                            return std::expected<nlohmann::json, ToolError>{nlohmann::json("带伞")};
                          }));
  reg.add(tool::make_tool("股价查询", "查询个股股价",
                          nlohmann::json::object(),
                          [](nlohmann::json const&, ToolContext&) {
                            return std::expected<nlohmann::json, ToolError>{nlohmann::json("88")};
                          }));
  return reg;
}

std::vector<std::string> names(std::vector<ToolSpec const*> const& picked) {
  std::vector<std::string> out;
  for (auto* s : picked) out.push_back(s->name);
  return out;
}

}  // namespace

TEST(HybridSelector, works_pure_keyword_without_embedder) {
  tool::ToolRegistry const reg = sample_registry();
  HybridSelector::Options opts;
  opts.use_vector = false;
  HybridSelector h(nullptr, opts);
  ASSERT_TRUE(h.build_index(reg));  // no-op
  EXPECT_FALSE(h.indexed());
  // 无向量时退化为关键词：能命中"天气"字面
  auto picked = h.select(reg, "实时天气查询", 5);
  auto const n = names(picked);
  ASSERT_FALSE(n.empty());
  EXPECT_EQ(n.front(), "天气查询");
}

TEST(HybridSelector, union_dedup_keyword_first) {
  tool::ToolRegistry const reg = sample_registry();
  StubEmbeddings stub;
  HybridSelector h(&stub);
  ASSERT_TRUE(h.build_index(reg));
  EXPECT_TRUE(h.indexed());

  // 关键词命中"天气查询"；向量对"带伞"还会命中"户外建议"
  auto picked = h.select(reg, "今天下雨要带伞吗，查询天气", 8);
  auto const n = names(picked);
  ASSERT_FALSE(n.empty());
  EXPECT_EQ(n.front(), "天气查询");                 // 关键词结果在前
  EXPECT_NE(std::find(n.begin(), n.end(), "户外建议"), n.end());  // 向量/关键词臂带进来的
  // 去重："天气查询"由关键词与向量各返回一次，混合结果里只能出现一次
  EXPECT_EQ(std::count(n.begin(), n.end(), "天气查询"), 1u);
}

TEST(HybridSelector, vector_hits_synonym_keyword_misses) {
  tool::ToolRegistry const reg = sample_registry();
  StubEmbeddings stub;
  HybridSelector h(&stub);
  ASSERT_TRUE(h.build_index(reg));

  // "带伞"不是任何工具名的字面 token（关键词空），但向量理解为天气语义 → 召回"户外建议"
  auto const n = names(h.select(reg, "今天出门带伞吗", 5));
  EXPECT_NE(std::find(n.begin(), n.end(), "户外建议"), n.end());
}

TEST(HybridSelector, degrades_to_keyword_on_embed_failure) {
  tool::ToolRegistry const reg = sample_registry();
  FailingEmbeddings fail;
  HybridSelector h(&fail);
  auto r = h.build_index(reg);
  ASSERT_FALSE(r.has_value());           // 建索引失败如实上报
  auto const n = names(h.select(reg, "查询今天的天气情况", 5));
  EXPECT_NE(std::find(n.begin(), n.end(), "天气查询"), n.end());  // 关键词臂仍然工作
  EXPECT_TRUE(h.last_error().has_value());
}