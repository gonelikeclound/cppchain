// cppchain —— tool/skill_tools.hpp
// Skill 文档 + LLM Wiki 检索工具（⑤ hmi-editor 上下文工程）：
//   · read_skill   —— 按名读取一份 skill 文档（宿主目录 WFR_SKILL_DIR）
//   · wiki_search  —— 按关键词检索 wiki 文章（WFR_WIKI_DIR，回落 WFR_SKILL_DIR/wiki），
//                     返回 top-k 片段。阶段 1 = 关键词/TOC 检索（确定性）；阶段 2 可换
//                     embedding（复用 model/embeddings.hpp 的零件）。
// 两者都只读（start 在 model/run_tools 的 ToolMessage 通道返回给模型）。
#pragma once

#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

// read_skill：目录 = 环境变量 WFR_SKILL_DIR（缺失 → 工具报错并提示）。
// 参数 skill 可带或不带 .md；越界/不存在返回可用清单。
[[nodiscard]] ToolSpec make_read_skill_tool();

// wiki_search：目录 = WFR_WIKI_DIR，未设置则回落 <WFR_SKILL_DIR>/wiki。
// 参数 query（必填）、max_results（1–5，默认 3）。返回 [{file, snippet}]。
[[nodiscard]] ToolSpec make_wiki_search_tool();

}  // namespace cppchain::tool