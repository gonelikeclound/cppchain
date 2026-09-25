// cppchain —— tool/arxiv.hpp
// arXiv 论文检索工具（arxiv_search）：免费、无需 API Key。
// 官方 Atom API（export.arxiv.org/api/query）返回**XML 而非 JSON**——
// 本工具内置轻量 Atom 解析（不引入第三方 XML 库；格式受控、看得见每一步）。
//
// 检索语法（arXiv API）：all: 全文 / ti: 标题 / au: 作者 / cat: 分类（如 cat:cs.AI），
// 支持 AND / OR / NOT 与引号短语，如 ti:"deep learning" AND cat:cs.CL。
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "cppchain/http/transport.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

struct ArxivConfig {
  std::string base_url = "https://export.arxiv.org/api/query";
  long default_max_results = 3;  // 默认返回条数（够综述/找相关工作）
  long max_results_cap = 10;
  long default_summary_chars = 800;  // 摘要截断（UTF-8 安全；控制上下文占用）
  std::string ca_info;               // 企业代理/自签 CA
  long timeout_ms = 30000;
  // 论文元数据更新慢，快照时效宽松：60 分钟后视为可能过期。
  std::chrono::seconds ttl = std::chrono::minutes(60);
};

// 纯函数：构造查询 URL——空格 → '+'（arXiv 约定），保留检索语法字符，其余百分号编码。
[[nodiscard]] std::string build_arxiv_query(ArxivConfig const& cfg,
                                            std::string const& search_query, long max_results);

// 纯函数：解析 arXiv Atom XML → 结构化结果（供离线单测与错误诊断）。
// 返回：{query, total, papers:[{id,title,authors,category,published,updated,summary}]}，
// summary 按 summary_chars 截断（<0 不截）。
[[nodiscard]] nlohmann::json parse_arxiv_response(std::string_view raw,
                                                  std::string const& query, long summary_chars);

// 生成 arxiv_search 工具。参数：search_query（必填）、max_results（可选 1–10）。返回如上。
// 网络失败 / 非 2xx / 无匹配论文（total=0）→ unexpected(ToolError)，让模型换词再试。
[[nodiscard]] ToolSpec make_arxiv_search_tool(std::shared_ptr<http::Transport> transport,
                                              ArxivConfig cfg = {});

}  // namespace cppchain::tool