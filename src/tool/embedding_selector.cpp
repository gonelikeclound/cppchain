// cppchain —— src/tool/embedding_selector.cpp
#include "cppchain/tool/embedding_selector.hpp"

#include <algorithm>
#include <utility>

namespace cppchain::tool {

std::expected<void, model::ModelError> EmbeddingSelector::build_index(ToolRegistry const& reg) {
  index_.clear();
  indexed_count_ = 0;
  built_ = false;
  last_error_.reset();

  if (reg.empty()) {
    built_ = true;  // 空注册表：索引平凡完成
    return {};
  }

  std::vector<std::string> texts;
  texts.reserve(reg.size());
  for (auto const& spec : reg.all()) texts.push_back(index_text(spec));

  auto vectors = embedder_->embed(texts);  // 适配器内部按 max_batch_size 分批
  if (!vectors) return std::unexpected(vectors.error());
  if (vectors->size() != texts.size())
    return std::unexpected(model::ModelError{model::ModelError::Kind::Parse, 0,
                                             "embedding count mismatch on index build", {}});

  index_ = std::move(*vectors);
  indexed_count_ = index_.size();
  built_ = true;
  return {};
}

std::vector<ToolSpec const*> EmbeddingSelector::select(ToolRegistry const& reg,
                                                       std::string_view query,
                                                       std::size_t k) const {
  last_error_.reset();

  auto const fallback = [&]() -> std::vector<ToolSpec const*> {
    if (!opts_.fallback_to_keyword) return {};
    return keyword_.select(reg, query, k);
  };

  if (!indexed()) {
    last_error_ = model::ModelError{model::ModelError::Kind::Unknown, 0,
                                    "index not built (call build_index first)", {}};
    return fallback();
  }

  auto qv = embedder_->embed_query(query);
  if (!qv) {
    last_error_ = qv.error();
    return fallback();
  }

  // 余弦相似度排序（同分保持注册顺序 → 稳定）
  std::vector<std::pair<double, std::size_t>> scored;
  scored.reserve(index_.size());
  for (std::size_t i = 0; i < index_.size(); ++i)
    scored.emplace_back(model::cosine_similarity(*qv, index_[i]), i);

  std::stable_sort(scored.begin(), scored.end(),
                   [](auto const& a, auto const& b) { return a.first > b.first; });
  if (k > 0 && scored.size() > k) scored.resize(k);

  std::vector<ToolSpec const*> out;
  out.reserve(scored.size());
  for (auto const& [score, idx] : scored) {
    (void)score;
    if (idx < reg.size()) out.push_back(&reg.all()[idx]);
  }
  return out;
}

}  // namespace cppchain::tool