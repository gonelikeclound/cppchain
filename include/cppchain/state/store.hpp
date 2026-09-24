// cppchain —— state/store.hpp
// 长期记忆：跨线程的命名空间 KV（M5）。参照 langchain Store。
// 检索先做**子串匹配**（零依赖）；语义检索留接口，待需要时接 embedding。
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cppchain::state {

using Json = nlohmann::json;

struct StoreItem {
  std::vector<std::string> ns;
  std::string key;
  Json value;
  std::string created_at;
  std::string updated_at;
};

using Namespace = std::vector<std::string>;

class Store {
 public:
  virtual ~Store() = default;

  virtual void put(Namespace const& ns, std::string const& key, Json value) = 0;
  virtual std::optional<StoreItem> get(Namespace const& ns, std::string const& key) = 0;
  virtual bool del(Namespace const& ns, std::string const& key) = 0;

  // 在 ns 前缀下检索：query 为空 → 全部；否则匹配 key 或 value 的**子串**
  virtual std::vector<StoreItem> search(Namespace const& ns_prefix, std::string const& query,
                                        std::size_t limit) = 0;

  // 列出 ns_prefix 下的所有命名空间
  virtual std::vector<Namespace> list_namespaces(Namespace const& ns_prefix) = 0;
};

class MemoryStore final : public Store {
 public:
  void put(Namespace const& ns, std::string const& key, Json value) override;
  std::optional<StoreItem> get(Namespace const& ns, std::string const& key) override;
  bool del(Namespace const& ns, std::string const& key) override;
  std::vector<StoreItem> search(Namespace const& ns_prefix, std::string const& query,
                                std::size_t limit) override;
  std::vector<Namespace> list_namespaces(Namespace const& ns_prefix) override;

  // 条目总数（跨命名空间累加）—— 注意不是命名空间个数
  [[nodiscard]] std::size_t size() const {
    std::size_t total = 0;
    for (auto const& entry : items_) total += entry.second.size();
    return total;
  }

  [[nodiscard]] std::size_t namespace_count() const { return items_.size(); }

 private:
  static std::string join(Namespace const& ns);
  std::unordered_map<std::string, std::unordered_map<std::string, StoreItem>> items_;  // ns → key → item
};

}  // namespace cppchain::state