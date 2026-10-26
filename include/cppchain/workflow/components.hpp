// cppchain —— workflow/components.hpp
// 组件注册表（M12，见 02-arch-design/11 §5.4）。
//
// 依赖倒置：IR 只说"用名为 main 的模型 + 名为 research_toolset 的工具集"，
// 具体绑哪个 C++ 对象由**宿主**在启动时注册。这样 IR 可以被存储、被生成、被审计，
// 而不携带任何进程内指针；也能在**校验期**就据注册表拒绝未知组件名（对标 VN6 对工具的做法）。
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/embeddings.hpp"
#include "cppchain/tool/registry.hpp"

namespace cppchain::workflow {

// 名字 → 组件。全部**非拥有**：宿主保证其生命周期长于编译/运行期。
struct ComponentRegistry {
  std::unordered_map<std::string, model::ChatModel*> models;           // model 组件
  std::unordered_map<std::string, model::Embeddings*> embeddings;      // embedding 组件（语义检索）
  std::unordered_map<std::string, std::vector<std::string>> toolsets;  // 工具集：名字 → 工具名列表

  [[nodiscard]] model::ChatModel* model(std::string const& name) const {
    auto const it = models.find(name);
    return it == models.end() ? nullptr : it->second;
  }

  [[nodiscard]] model::Embeddings* embedding(std::string const& name) const {
    auto const it = embeddings.find(name);
    return it == embeddings.end() ? nullptr : it->second;
  }

  [[nodiscard]] std::vector<std::string> const* toolset(std::string const& name) const {
    auto const it = toolsets.find(name);
    return it == toolsets.end() ? nullptr : &it->second;
  }
};

}  // namespace cppchain::workflow
