// cppchain —— src/tool/hybrid_selector.cpp
#include "cppchain/tool/hybrid_selector.hpp"

#include <unordered_set>
#include <utility>

namespace cppchain::tool {

HybridSelector::HybridSelector(model::Embeddings* embedder, Options opts)
    : embedder_(embedder),
      opts_(std::move(opts)),
      keyword_(opts_.keyword),
      vector_(embedder_ ? std::optional<EmbeddingSelector>(std::in_place, *embedder_,
                                                           EmbeddingSelector::Options{
                                                               .fallback_to_keyword = false,  // hybrid 自带关键词臂，向量不再降级
                                                               .keyword = opts_.keyword,
                                                           })
                        : std::nullopt) {}

std::expected<void, model::ModelError> HybridSelector::build_index(ToolRegistry const& reg) {
  if (!vector_ || !opts_.use_vector) return {};
  return vector_->build_index(reg);  // vector_ 内部记 indexed/last_error
}

bool HybridSelector::indexed() const { return vector_ && vector_->indexed(); }

std::optional<model::ModelError> HybridSelector::last_error() const {
  return vector_ ? vector_->last_error() : std::nullopt;
}

std::vector<ToolSpec const*> HybridSelector::select(ToolRegistry const& reg, std::string_view query,
                                                    std::size_t k) const {
  if (!vector_ || !opts_.use_vector) return keyword_.select(reg, query, k);

  auto kw = keyword_.select(reg, query, opts_.keyword_k);
  auto vec = vector_->select(reg, query, opts_.vector_k);

  // union · 按指针去重：关键词在前，向量独家命中在后
  std::vector<ToolSpec const*> out = std::move(kw);
  std::unordered_set<ToolSpec const*> seen(out.begin(), out.end());
  for (auto* s : vec)
    if (seen.insert(s).second) out.push_back(s);

  if (k > 0 && out.size() > k) out.resize(k);
  return out;
}

}  // namespace cppchain::tool