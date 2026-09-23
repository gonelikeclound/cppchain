// cppchain —— tool/registry.hpp
// 工具注册表：注册 / 按名查找 / 导出给模型的 schema。
// 注意：find() 返回的指针在**注册完成后**稳定（先注册后使用，勿在持有指针时继续 add）。
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

class ToolRegistry {
 public:
  void add(ToolSpec spec) {
    index_[spec.name] = tools_.size();
    tools_.push_back(std::move(spec));
  }

  [[nodiscard]] ToolSpec const* find(std::string_view name) const {
    auto it = index_.find(std::string(name));
    return it == index_.end() ? nullptr : &tools_[it->second];
  }

  [[nodiscard]] std::span<ToolSpec const> all() const { return tools_; }
  [[nodiscard]] std::size_t size() const { return tools_.size(); }
  [[nodiscard]] bool empty() const { return tools_.empty(); }

  // 导出给定子集为 OpenAI tools 数组（检索后只发子集 → 省 prompt）
  [[nodiscard]] nlohmann::json to_openai_tools(std::span<ToolSpec const* const> subset) const {
    auto arr = nlohmann::json::array();
    for (ToolSpec const* spec : subset)
      if (spec) arr.push_back(spec->to_openai());
    return arr;
  }

  // 全量导出（小集合时用）
  [[nodiscard]] nlohmann::json to_openai_tools() const {
    std::vector<ToolSpec const*> ptrs;
    ptrs.reserve(tools_.size());
    for (auto const& t : tools_) ptrs.push_back(&t);
    return to_openai_tools(ptrs);
  }

 private:
  std::vector<ToolSpec> tools_;
  std::unordered_map<std::string, std::size_t> index_;
};

}  // namespace cppchain::tool