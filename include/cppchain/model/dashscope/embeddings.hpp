// cppchain —— model/dashscope/embeddings.hpp
// 阿里云 MaaS / DashScope 原生 embedding 适配器（非 OpenAI 格式）。
//
// 实测协议（2026-09）：
//   POST {base}/api/v1/services/embeddings/text-embedding/text-embedding
//   → {"model":"…","input":{"texts":[…]}}
//   ← {"output":{"embeddings":[{"text_index":0,"embedding":[1024 维]}]},
//      "usage":{"total_tokens":N},"request_id":"…"}
//   错误 ← {"code":"InvalidApiKey|InvalidParameter|…","message":"…","request_id":"…"}
//   · 返回**不保证有序** → 必须按 text_index 重排
//   · 批量上限 20（超出报 "batch size … should not be larger than 20"）
#pragma once

#include <memory>

#include "cppchain/http/transport.hpp"
#include "cppchain/model/embeddings.hpp"

namespace cppchain::model::dashscope {

class DashScopeEmbeddings final : public Embeddings {
 public:
  DashScopeEmbeddings(EmbeddingConfig cfg, std::shared_ptr<http::Transport> transport);

  std::expected<std::vector<Embedding>, ModelError> embed(
      std::span<std::string const> texts) override;

  [[nodiscard]] std::size_t dimension() const override { return dimension_; }
  [[nodiscard]] std::size_t max_batch_size() const override { return cfg_.max_batch_size; }

  // 累计消耗的 token（多批次累加），便于观察成本
  [[nodiscard]] long total_tokens() const { return total_tokens_; }

 private:
  std::expected<std::vector<Embedding>, ModelError> embed_batch(
      std::span<std::string const> texts);

  EmbeddingConfig cfg_;
  std::shared_ptr<http::Transport> transport_;
  std::size_t dimension_ = 0;  // 首次响应后确定
  long total_tokens_ = 0;
};

}  // namespace cppchain::model::dashscope