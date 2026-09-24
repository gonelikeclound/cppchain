// cppchain —— tool/embedding_selector.hpp
// 向量检索：用 embedding 做语义召回，解决关键词检索的三个已知短板
// （同义改写、跨语言、纯语义相似 —— 见 04-problems/01）。
//
// 用法：
//   EmbeddingSelector sel(embeddings);
//   if (auto r = sel.build_index(reg); !r) { ... }   // 离线建索引（批量）
//   auto picked = sel.select(reg, "明天带伞吗", 5);   // 运行时：嵌入查询 + 余弦 top-k
#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/model/embeddings.hpp"
#include "cppchain/tool/registry.hpp"
#include "cppchain/tool/selector.hpp"

namespace cppchain::tool {

class EmbeddingSelector final : public ToolSelector {
 public:
  struct Options {
    // 查询嵌入失败时降级到关键词检索（避免检索整体不可用）
    bool fallback_to_keyword = true;
    KeywordSelector::Options keyword{};
  };

  explicit EmbeddingSelector(model::Embeddings& embedder)
      : embedder_(&embedder), keyword_(opts_.keyword) {}
  EmbeddingSelector(model::Embeddings& embedder, Options opts)
      : embedder_(&embedder), opts_(opts), keyword_(opts.keyword) {}

  // 为注册表建索引：批量嵌入"工具名 + 可检索描述"，顺序与 reg.all() 一致。
  std::expected<void, model::ModelError> build_index(ToolRegistry const& reg);

  // 显式标志：不能用 index_.size()==indexed_count_ 判断（未建索引时两者都是 0 会误判为"已建"）
  [[nodiscard]] bool indexed() const { return built_; }
  [[nodiscard]] std::size_t indexed_count() const { return indexed_count_; }

  // 运行时的最近一次错误（查询嵌入失败时置位；已降级/返回空）
  [[nodiscard]] std::optional<model::ModelError> last_error() const { return last_error_; }

  [[nodiscard]] std::vector<ToolSpec const*> select(ToolRegistry const& reg, std::string_view query,
                                                    std::size_t k) const override;

  // 建索引时用的文本（也用于测试与解释）
  [[nodiscard]] static std::string index_text(ToolSpec const& spec) {
    return spec.name + " " + KeywordSelector::searchable_description(spec);
  }

 private:
  model::Embeddings* embedder_;
  Options opts_;
  KeywordSelector keyword_;

  std::vector<model::Embedding> index_;  // 与注册表 all() 同序
  std::size_t indexed_count_ = 0;
  bool built_ = false;                   // 是否成功建过索引
  mutable std::optional<model::ModelError> last_error_;
};

}  // namespace cppchain::tool