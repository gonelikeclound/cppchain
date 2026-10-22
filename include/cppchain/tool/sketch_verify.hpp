// cppchain —— tool/sketch_verify.hpp
// 画板 sketch 校验工具（question 工作流用）：静态脚枪 + node --check 语法。
// 真机挂载（CDP）在 geo-server 集成层做；这里先在模型自修环里挡掉"一眼崩"的写法。
#pragma once

#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

// 纯本地、零密钥；无配置。
[[nodiscard]] ToolSpec make_sketch_verify_tool();

}  // namespace cppchain::tool
