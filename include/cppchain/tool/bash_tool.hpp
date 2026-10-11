// cppchain —— tool/bash_tool.hpp
// bash 工具（wfr-㊸ @①）：副作用任意的"程序杠杆"，出厂契约：
//   read_only=false · category=action · approval=NeedHuman（越过静态策略，直接要求人审——
//   批② 起由 approve_tools 对该类 interrupt 悬挂）
//   timeout_ms=120s（调用方可覆盖）· executor 注入（无执行器 → handler fail-closed 拒跑）
//   command_classify = 词面分类器（L2 静态门禁）—— 只读命令免审，其余 guard 阻断/升级
// 机制见 01-discuss/06 §5 L1/L2/L4。
#pragma once

#include <chrono>
#include <optional>

#include "cppchain/tool/executor.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

// 构造 bash 工具。executor 为执行后端（批① 用 make_plain_spawn_executor；批③ SandboxManager）。
// timeout 缺省 120s；spec.timeout_ms 即该值。
[[nodiscard]] ToolSpec make_bash_tool(
    ToolExecutor const* executor = nullptr,
    std::optional<std::chrono::milliseconds> timeout = std::chrono::milliseconds(120000));

}  // namespace cppchain::tool