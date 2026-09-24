// cppchain —— tool/web_search.hpp
// 真实联网搜索工具：火山方舟 Responses API 的 web_search（豆包来源）。
// 参考（用户提供的文档示例 curl）：
//   POST {base_url}/api/v3/responses
//   Authorization: Bearer $ARK_API_KEY
//   { "model": "doubao-...", "stream": true,
//     "tools": [{"type":"web_search","sources":["doubao"]}],
//     "input": [{"role":"user","content":[{"type":"input_text","text": query}]}] }
// 实现走 stream=false（工具内需要完整 JSON 一次给到），返回模型基于豆包搜索
// 的综合回答 + 引用列表。密钥与端点由使用方注入，绝不写死在源码。
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cppchain/http/transport.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

struct WebSearchConfig {
  std::string base_url = "https://ark.cn-beijing.volces.com";  // 火山方舟端点
  std::string path = "/api/v3/responses";                      // Responses API（OpenAI 兼容）
  std::string api_key;                                         // Ark API Key（UUID 格式）
  std::string model = "doubao-seed-2-1-pro-260628";
  std::string source = "doubao";                               // web_search 的 sources
  std::string ca_info;                                         // 企业代理/自签 CA
  long timeout_ms = 60000;
  // 搜索结果时效性强（热点/价格/新闻），15 分钟后即视为可能过期
  //（由 agent::StaleStateGuard 消费，见 04-problems/06）。
  std::chrono::seconds ttl = std::chrono::minutes(15);
};

// 生成 web_search 工具。参数：query（必填）。返回：
//   {query, answer, citations:[{title,url}], source}
// answer 是模型基于豆包搜索给出的综合回答；citations 是命中的引用（若能解析到）。
// 网络/鉴权失败 → unexpected(ToolError)，模型会如实告知用户而不是编造。
[[nodiscard]] ToolSpec make_web_search_tool(std::shared_ptr<http::Transport> transport,
                                            WebSearchConfig cfg);

// ---- 纯函数，单独暴露便于离线单测（FakeTransport 注入真实形状的响应）----

// 由配置 + 查询词生成 Responses 请求体
[[nodiscard]] nlohmann::json build_web_search_request(WebSearchConfig const& cfg,
                                                      std::string const& query);

// 解析 Responses 响应 → 归一化的 {query, answer, citations[]}
// 按 Responses API 的 output[] 结构防御性提取（answer 取 message 的 output_text 级联）
[[nodiscard]] nlohmann::json parse_web_search_response(nlohmann::json const& raw,
                                                       std::string const& query);

}  // namespace cppchain::tool