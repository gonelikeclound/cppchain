// cppchain —— tool/hybrid_selector.hpp
// M9：混合召回（问题 1 收官）—— keyword ∪ vector → 按指针去重 → top-k。
// 关键词结果在前（可解释、可复现），向量独家命中补在后面。
// 向量缺失/未建索引 → 退化为纯关键词（对照基线即为 use_vector=false 的同一对象）。
#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

#include "cppchain/model/embeddings.hpp"
#include "cppchain/model/error.hpp"
#include "cppchain/tool/embedding_selector.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/tool/selector.hpp"

namespace cppchain::tool {

class HybridSelector final : public ToolSelector {
 public:
  struct Options {
    std::size_t keyword_k = 8;
    std::size_t vector_k = 8;
    bool use_vector = true;  // 关掉即退化纯关键词（对照）
    KeywordSelector::Options keyword{};
  };

  // embedder 可为空：`embedder==nullptr || !use_vector` → 纯关键词模式。
  // 便捷重载：缺省 Options（GCC 不接受"带 DMI 的嵌套聚合"作默认参数 → 委托构造 + Options{}）
  explicit HybridSelector(model::Embeddings* embedder) : HybridSelector(embedder, Options{}) {}
  HybridSelector(model::Embeddings* embedder, Options opts);

  // 为注册表建向量索引（转发 EmbeddingSelector；纯关键词模式为 no-op）
  std::expected<void, model::ModelError> build_index(ToolRegistry const& reg);

  [[nodiscard]] std::vector<ToolSpec const*> select(ToolRegistry const& reg,
                                                    std::string_view query,
                                                    std::size_t k) const override;

  [[nodiscard]] bool indexed() const;
  [[nodiscard]] std::optional<model::ModelError> last_error() const;

 private:
  model::Embeddings* embedder_;
  Options opts_;
  KeywordSelector keyword_;
  std::optional<EmbeddingSelector> vector_;  // embedder==nullptr → 不持有
};

}  // namespace cppchain::tool