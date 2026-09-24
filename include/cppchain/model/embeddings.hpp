// cppchain —— model/embeddings.hpp
// 向量（embedding）抽象：与 ChatModel 并列的另一类模型能力。
// 用途：语义检索（工具检索、记忆检索、RAG）——「大批量工具查找」问题的解法之一。
#pragma once

#include <cmath>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/model/error.hpp"

namespace cppchain::model {

using Embedding = std::vector<float>;

struct EmbeddingConfig {
  std::string api_key;
  std::string base_url;
  std::string model;
  long timeout_ms = 60000;
  std::size_t max_batch_size = 20;  // 服务端批量上限（DashScope 实测 20，超出报 InvalidParameter）
  std::string ca_info;              // 企业代理/自签环境的 CA bundle
};

class Embeddings {
 public:
  virtual ~Embeddings() = default;

  // 批量嵌入。**返回顺序与输入严格一致**（实现负责按服务端的 index 重排）。
  // 超出 max_batch_size 时由实现自动分批。
  virtual std::expected<std::vector<Embedding>, ModelError> embed(
      std::span<std::string const> texts) = 0;

  virtual std::size_t dimension() const = 0;
  virtual std::size_t max_batch_size() const = 0;

  // 单条查询的便捷封装
  std::expected<Embedding, ModelError> embed_query(std::string_view text) {
    std::vector<std::string> one{std::string(text)};
    auto r = embed(one);
    if (!r) return std::unexpected(r.error());
    if (r->empty()) return std::unexpected(ModelError{ModelError::Kind::Parse, 0, "empty result", {}});
    return std::move((*r)[0]);
  }
};

// 余弦相似度（两个等长向量）
inline double cosine_similarity(Embedding const& a, Embedding const& b) {
  if (a.size() != b.size() || a.empty()) return 0.0;
  double dot = 0.0;
  double na = 0.0;
  double nb = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    na += static_cast<double>(a[i]) * static_cast<double>(a[i]);
    nb += static_cast<double>(b[i]) * static_cast<double>(b[i]);
  }
  if (na <= 0.0 || nb <= 0.0) return 0.0;
  return dot / (std::sqrt(na) * std::sqrt(nb));
}

}  // namespace cppchain::model