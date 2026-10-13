// cppchain —— tool/trip_db.hpp
// B2-B（DeepSeek-V3.2 三日出行规划任务）的业务 DB 工具面：只读城市/酒店/餐厅/景点/交通/天气
// 查询 + submit_result（收最终 JSON + 自动验证器落 .out/trip_verdict.json）。
// 数据为确定性种子集（约束指派的搜索空间小但可组合，验证规则照论文：不重复/城市一致/预算档）。
#pragma once

#include <string>
#include <vector>

#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

// 一次性注册 13 个工具（12 只读 + submit_result）
[[nodiscard]] std::vector<ToolSpec> make_trip_tools();

}  // namespace cppchain::tool