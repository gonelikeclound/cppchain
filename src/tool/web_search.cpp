// cppchain —— src/tool/web_search.cpp
#include "cppchain/tool/web_search.hpp"

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>

#include "cppchain/tool/http_json.hpp"

namespace cppchain::tool {

// ---- 纯函数：请求构造 / 响应解析（供离线单测）----

nlohmann::json build_web_search_request(WebSearchConfig const& cfg, std::string const& query) {
  return nlohmann::json{{"query", query},
                        {"doc_count", std::clamp(cfg.doc_count, 1L, 10L)},
                        {"max_snippet_length", std::max(cfg.max_snippet_length, 0L)},
                        {"max_image_count_per_doc", 0}};
}

namespace {

// 取文档里的文本摘要：Snippet[] 中 Type=="text" 的 Text 串联，UTF-8 安全截断
std::string doc_snippet(nlohmann::json const& doc, long max_chars) {
  std::string out;
  auto const* snips = doc.contains("Snippet") ? &doc["Snippet"]
                     : doc.contains("snippet") ? &doc["snippet"]
                                               : nullptr;
  if (snips && snips->is_array()) {
    for (auto const& s : *snips) {
      if (!s.is_object()) continue;
      if (s.value("Type", s.value("type", "")) != "text") continue;
      auto const t = s.value("Text", s.value("text", ""));
      if (!t.empty()) {
        if (!out.empty()) out += " ";
        out += t;
      }
    }
  } else if (!out.empty()) {
    return out;
  }
  if (out.size() > static_cast<std::size_t>(max_chars)) {
    std::size_t end = max_chars;
    while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xC0) == 0x80) --end;
    out = out.substr(0, end) + "…";
  }
  return out;
}

// 兼容 key 大小写的取值助手
std::string val_of(nlohmann::json const& obj, std::string const& camel, std::string const& lower,
                   std::string fallback = "") {
  if (obj.is_object()) {
    if (auto it = obj.find(camel); it != obj.end() && !it->is_null()) return it->get<std::string>();
    if (auto it = obj.find(lower); it != obj.end() && !it->is_null()) return it->get<std::string>();
  }
  return fallback;
}

}  // namespace

nlohmann::json parse_web_search_response(nlohmann::json const& raw, std::string const& query) {
  nlohmann::json const* result = nullptr;
  if (auto it = raw.find("Result"); it != raw.end() && it->is_object())
    result = &*it;
  else if (auto it = raw.find("result"); it != raw.end() && it->is_object())
    result = &*it;

  long total = 0;
  nlohmann::json docs;
  if (result) {
    if (auto it = result->find("TotalDocCount"); it != result->end() && it->is_number())
      total = it->get<long>();
    else if (auto it = result->find("total"); it != result->end() && it->is_number())
      total = it->get<long>();
    if (auto it = result->find("Documents"); it != result->end() && it->is_array())
      docs = *it;
    else if (auto it = result->find("GlobalSearchResp"); it != result->end() && it->is_object())
      docs = (*it).value("Documents", nlohmann::json::array());
  }

  auto doc_arr = nlohmann::json::array();
  if (docs.is_array()) {
    for (auto const& d : docs) {
      if (!d.is_object()) continue;
      nlohmann::json row{
          {"title", val_of(d, "Title", "title", "(无标题)")},
          {"url", val_of(d, "Url", "url")},
          {"hostname",
           d.contains("HostInfo") && d["HostInfo"].is_object() ? val_of(d["HostInfo"], "Hostname", "hostname")
                                                               : std::string{}},
          {"publish_time",
           d.contains("DocumentInfo") && d["DocumentInfo"].is_object()
               ? val_of(d["DocumentInfo"], "PublishTime", "publishTime")
               : std::string{}},
          {"snippet", doc_snippet(d, 200)},
      };
      doc_arr.push_back(std::move(row));
    }
  }

  return nlohmann::json{{"query", query},
                        {"total", total},
                        {"documents", std::move(doc_arr)},
                        {"source", "feedcoop global search"}};
}

// ---- 工具 ----

ToolSpec make_web_search_tool(std::shared_ptr<http::Transport> transport, WebSearchConfig cfg) {
  ToolSpec spec = make_tool(
      "web_search",
      "联网搜索最新网页资料（新闻、事件进展、时效性事实、数据、产品信息等）。"
      "返回命中的文档列表（标题、链接、来源站点、发布时间、摘要片段）；"
      "你需要基于文档内容组织回答并尽量引用来源。返回结构化 JSON："
      "{query, total, documents:[{title,url,hostname,publish_time,snippet}]}。",
      SchemaBuilder().prop("query", "string", "搜索查询词，越具体越好").build(),
      [transport, cfg](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        auto query = arg<std::string>(args, "query");
        if (!query) return std::unexpected(query.error());

        if (cfg.api_key.empty())
          return std::unexpected(ToolError{
              "web_search 未配置 API Key：请在 .env 设置 WEB_SEARCH_API_KEY（feedcoop "
              "search_api，Bearer 认证）。"});

        http::HttpRequest req;
        req.url = cfg.base_url;
        req.body = build_web_search_request(cfg, *query).dump();
        req.timeout_ms = cfg.timeout_ms;
        req.ca_info = cfg.ca_info;
        req.headers = {"Content-Type: application/json",
                       "Authorization: Bearer " + cfg.api_key};

        auto r = http_json(*transport, req, ctx.cancel);
        if (!r) return std::unexpected(r.error());
        if (auto e = http_status_error(r->status, "搜索服务", r->raw))
          return std::unexpected(*e);

        auto parsed = parse_web_search_response(r->body, *query);
        if (parsed.value("documents", nlohmann::json::array()).empty())
          return std::unexpected(ToolError{
              "搜索未返回有效内容（无命中文档），请换一个更具体的查询词重试。"});
        return parsed;
      },
      /*read_only=*/true);
  spec.category = "retrieval";  // 供历史精简按类别过滤（wfr-㊵）
  spec.ttl = cfg.ttl;  // 搜索快照 15 分钟后视为可能过期（见 WebSearchConfig）
  return spec;
}

}  // namespace cppchain::tool