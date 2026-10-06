// cppchain —— workflow/import.hpp
// workflow 可 import 其他 workflow（复用节点/子链）—— **展开式**设计：
//   读被引 IR → 前缀重写其全部节点 id 与指针/`$<id>` 引用 → 拼入本层扁平 nodes。
//   引擎（validator VG/VN/VR、compiler label 续接与 resolve）零改动——展开后是单 IR。
//
// 顶层键（避开现 `imports` 动态库数组）：
//   "import": { "workflow": "<name-or-path>", "as": "<可选前缀，默认被引 workflow 的 id>" }
//   "import": [ {...}, {...} ]          # 或数组；顺序即拼接顺序
//
// 约定：
//   · 被引 workflow 的 trigger 丢弃（子链无调度语义）；其自身 import（嵌套）递归展开；
//   · 其顶层 import 动态库（"imports"）v1 拒绝（fail-closed，需宿主先 --allow-import 顶层）；
//   · 父层引用被引 workflow 的节点：用**其文件里写的原始 id**（自动前缀映射）；
//   · 循环 import（路径链）→ 报错；找不到 → 报 searched 路径；
//   · 节点 id 全图唯一（VN2 沿用）。
#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <functional>
#include <string>

namespace cppchain::workflow {

using Json = nlohmann::json;

// 读入器的可注入点（测试用临时目录 → 内存 JSON 不走文件）。
// 缺省实现：spec 含 '/' 或 ".json" 且相对 base_dir 存在 → 直接读文件；否则按名
// → `<base_dir>/.wfr/workflows/<name>.json`（见 wfr.hpp）。
// 成功时返回 true 并把读到的 JSON 放进 out。
struct WorkflowImportLoader {
  std::function<bool(std::string const& base_dir, std::string const& spec, Json& out,
                     std::string& err)>
      read;
};

// 原地展开顶层 ir 的 "import"（含递归），改写 ir["nodes"]；成功后删除 "import" 键，
// ir 变扁平（只剩 id/nodes，可交 WorkflowIR::parse）。
// base_dir：本 workflow 文件所在目录（相对 import 名解析的基准）。
[[nodiscard]] std::expected<void, std::string>
expand_workflow_imports(Json& ir, std::string const& base_dir,
                        WorkflowImportLoader const& loader = {});

}  // namespace cppchain::workflow