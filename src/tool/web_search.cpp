// cppchain —— src/tool/web_search.cpp
#include "cppchain/tool/web_search.hpp"

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/tool/http_json.hpp"

namespace cppchain::tool {

namespace {

std::string join_url(std::string base, std::string_view path) {
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + std::string(path);
}

}  // namespace

// ---- 纯函数：请求构造 / 响应解析（供离线单测）----

nlohmann::json build_web_search_request(WebSearchConfig const& cfg, std::string const& query) {
  // 用显式 json 构造，避免 json::array({{...},{...}}) 被拆成两个数组元素（而非单对象两键）
  nlohmann::json text = {{"type", "input_text"}, {"text", query}};
  nlohmann::json user_msg = {{"role", "user"}, {"content", nlohmann::json::array({text})}};
  nlohmann::json web_search_tool = {{"type", "web_search"},
                                    {"sources", nlohmann::json::array({cfg.source})}};
  return nlohmann::json{{"model", cfg.model},
                        {"stream", false},  // 工具需要完整 JSON；流式留给对话层
                        {"tools", nlohmann::json::array({web_search_tool})},
                        {"input", nlohmann::json::array({user_msg})}};
}

namespace {
// content 块里提取文本：类型 output_text（OpenAI 系）/ text（老式）都可
std::string block_text(nlohmann::json const& block) {
  if (!block.is_object()) return {};
  auto t = block.value("type", "");
  if (t != "output_text" && t != "text" && t != "input_text") return {};
  return block.value("text", "");
}

// 从一条 message output 里取引用：优先级
//   ① 顶层 url_citations（部分实现直接放在 output 项上）；
//   ② content 块内 annotations[]（OpenAI Responses 形状）。
// annotations 项可能是扁平 url_citation（{type,url,title}）或嵌套（{type,url_citation:{...}}）。
void collect_citations(nlohmann::json const& item, std::vector<nlohmann::json>& out) {
  if (auto cit = item.find("url_citations"); cit != item.end() && cit->is_array())
    for (auto const& c : *cit)
      if (c.is_object()) out.push_back(c);
  auto content = item.find("content");
  if (content == item.end() || !content->is_array()) return;
  for (auto const& b : *content) {
    if (!b.is_object()) continue;
    auto ann = b.find("annotations");
    if (ann == b.end() || !ann->is_array()) continue;
    for (auto const& a : *ann) {
      if (!a.is_object()) continue;
      nlohmann::json const* cit = &a;
      if (auto inner = a.find("url_citation"); inner != a.end() && inner->is_object())
        cit = &*inner;  // 嵌套形状 → 取内层
      out.push_back(*cit);
    }
  }
}
}  // namespace

nlohmann::json parse_web_search_response(nlohmann::json const& raw, std::string const& query) {
  std::string answer;
  std::vector<nlohmann::json> citations;

  auto out = raw.find("output");
  if (out != raw.end() && out->is_array()) {
    for (auto const& item : *out) {
      if (!item.is_object()) continue;
      collect_citations(item, citations);
      if (item.value("type", "") != "message") continue;
      auto content = item.find("content");
      if (content == item.end() || !content->is_array()) continue;
      for (auto const& b : *content) {
        std::string const t = block_text(b);
        if (!t.empty()) answer += t;
      }
    }
  }
  // 兜底：顶层 output_text（部分实现直接暴露聚合文本）
  if (answer.empty()) answer = raw.value("output_text", "");

  auto cit_arr = nlohmann::json::array();
  for (auto const& c : citations) {
    if (!c.is_object()) continue;
    nlohmann::json row{
        {"url", c.value("url", "")},
        {"title", c.value("title", "")},
    };
    cit_arr.push_back(std::move(row));
  }

  return nlohmann::json{{"query", query},
                        {"answer", answer},
                        {"citations", std::move(cit_arr)},
                        {"source", "火山方舟 Responses · web_search(doubao)"}};
}

// ---- 工具 ----

ToolSpec make_web_search_tool(std::shared_ptr<http::Transport> transport, WebSearchConfig cfg) {
  ToolSpec spec = make_tool(
      "web_search",
      "联网搜索并返回基于搜索结果的综合回答。用于查询「当前/最新」的网络信息：热点新闻、"
      "时效性事实（谁/何时/何地）、某产品最新报价、某事件最新进展等。"
      "回答由豆包模型结合豆包搜索整理，同时给出引用链接。返回结构化 JSON："
      "{query, answer, citations:[{title,url}]}。",
      SchemaBuilder().prop("query", "string", "搜索查询词，越具体越好").build(),
      [transport, cfg](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        auto query = arg<std::string>(args, "query");
        if (!query) return std::unexpected(query.error());

        if (cfg.api_key.empty())
          return std::unexpected(ToolError{
              "web_search 未配置 API Key：请在 .env 设置 WEB_SEARCH_API_KEY 为有效的火山方舟 "
              "API Key（UUID 格式），并确认 base_url/model。"});

        http::HttpRequest req;
        req.url = join_url(cfg.base_url, cfg.path);
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
        if (parsed.value("answer", "").empty())
          return std::unexpected(ToolError{
              "搜索未返回有效内容：请在工具描述约束的范围内换一种查询词重试"
              "（详见服务端响应截断诊断）。"});
        return parsed;
      },
      /*read_only=*/true);
  spec.ttl = cfg.ttl;  // 搜索快照 15 分钟后视为可能过期（见 WebSearchConfig）
  return spec;
}

}  // namespace cppchain::tool