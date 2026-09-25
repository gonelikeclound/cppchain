// cppchain —— tool/web_search.hpp
// 真实联网搜索工具：feedcoop global search（参考项目 simple-agent 的 handle_web_search）。
// 协议（Bearer key，POST JSON，返回文档列表、无模型合成摘要）：
//   POST {base_url}
//   Authorization: Bearer $WEB_SEARCH_API_KEY
//   {"query": q, "doc_count": 5, "max_snippet_length": 200, "max_image_count_per_doc": 0}
//   → Result.Documents[]（Title / Url / HostInfo.Hostname / DocumentInfo.PublishTime /
//     Snippet[].Text）
// 拿回的是**原始文档列表**而非成品答案 —— 由消费本工具的 LLM 基于文档组织回答。
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cppchain/http/transport.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

struct WebSearchConfig {
  std::string base_url = "https://open.feedcoopapi.com/search_api/global_search";  // feedcoop 端点
  std::string api_key;     // feedcoop 搜索 API Key（Bearer）
  long doc_count = 5;      // 请求的文档数（1–10）
  long max_snippet_length = 200;  // 每条摘要片段的最大字符数
  std::string ca_info;     // 企业代理/自签 CA
  long timeout_ms = 30000;
  // 搜索结果时效性强（热点/价格/新闻），15 分钟后即视为可能过期
  //（由 agent::StaleStateGuard 消费，见 04-problems/06）。
  std::chrono::seconds ttl = std::chrono::minutes(15);
};

// 生成 web_search 工具。参数：query（必填）。返回：
//   {query, total, documents:[{title, url, hostname, publish_time, snippet}], source}
// documents 是命中的原始网页文档；无命中文档 → unexpected(ToolError)，
// 让模型换词重试或如实告知用户（避免把“空结果”误当成已联网取到资料）。
[[nodiscard]] ToolSpec make_web_search_tool(std::shared_ptr<http::Transport> transport,
                                            WebSearchConfig cfg);

// ---- 纯函数，单独暴露便于离线单测（FakeTransport 注入真实形状的响应）----

// 由配置 + 查询词生成 feedcoop 请求体
[[nodiscard]] nlohmann::json build_web_search_request(WebSearchConfig const& cfg,
                                                      std::string const& query);

// 解析 feedcoop 响应 → 归一化的 {query, total, documents[]}
// 兼容两种嵌套：Result.Documents（实际 API）与 Result.GlobalSearchResp.Documents（旧 spec）
[[nodiscard]] nlohmann::json parse_web_search_response(nlohmann::json const& raw,
                                                       std::string const& query);

}  // namespace cppchain::tool