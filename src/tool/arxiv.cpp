// cppchain —— src/tool/arxiv.cpp
#include "cppchain/tool/arxiv.hpp"

#include <algorithm>
#include <cctype>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/tool/http_json.hpp"
#include "cppchain/tool/text.hpp"

namespace cppchain::tool {
namespace {

using nlohmann::json;

// 取元素体：`<tag …>` 与 `</tag>` 之间的原始文本（大小写不敏感；无属性假设放宽到找 '>'）
std::string elem_content(std::string_view span, std::string_view tag) {
  std::string const open = std::string("<") + std::string(tag);
  auto const b = ci_find(span, open);
  if (b == std::string_view::npos) return {};
  auto const gt = span.find('>', b);
  if (gt == std::string_view::npos) return {};
  std::string const close = std::string("</") + std::string(tag);
  auto const e = ci_find(span, close, gt + 1);
  std::size_t const end = (e == std::string_view::npos) ? span.size() : e;
  return std::string(span.substr(gt + 1, end - gt - 1));
}

// 取分类：优先 <arxiv:primary_category>，回退第一个 <category> 的 term 属性
std::string first_category(std::string_view span) {
  for (std::string_view const open : {"<arxiv:primary_category", "<category"}) {
    auto const b = ci_find(span, open);
    if (b == std::string_view::npos) continue;
    std::string_view const rest = span.substr(b);
    auto const tk = ci_find(rest, "term=\"");
    if (tk == std::string_view::npos) continue;
    std::size_t const start = tk + 6;
    std::size_t const close = rest.find('"', start);
    if (close == std::string_view::npos) continue;
    return std::string(rest.substr(start, close - start));
  }
  return {};
}

// 作者：拼接所有 <author><name>…</name></author>；>8 人时截断为 前 8 + "等 N 人"
std::string authors_string(std::string_view span) {
  std::vector<std::string> names;
  std::size_t pos = 0;
  std::string_view const kAuthorClose = "</author>";
  while ((pos = ci_find(span, "<author", pos)) != std::string_view::npos) {
    auto const be = ci_find(span, kAuthorClose, pos + 7);
    if (be == std::string_view::npos) break;
    auto const nm = elem_content(span.substr(pos, be - pos + kAuthorClose.size()), "name");
    if (!nm.empty()) names.push_back(normalize_space(decode_entities(nm)));
    pos = be + kAuthorClose.size();
  }
  if (names.empty()) return {};
  constexpr std::size_t kCap = 8;
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += ", ";
    if (i == kCap) {
      out += "等 " + std::to_string(names.size() - kCap) + " 人";
      break;
    }
    out += names[i];
  }
  return out;
}

// 规范化 abs 链接：http→https、去 /abs/ 后的版本后缀 vN。
std::string clean_abs_id(std::string_view raw_id) {
  std::string const t = std::string(trim_ascii(decode_entities(raw_id)));
  std::size_t const p = t.find("/abs/");
  if (p == std::string::npos) return t;
  std::string_view tail(&t[p + 5]);
  std::size_t const slash = tail.rfind('/');
  std::string_view core = (slash == std::string_view::npos) ? tail : tail.substr(0, slash);
  auto const v = core.rfind('v');  // 去版本后缀 v\d+
  if (v != std::string_view::npos && v > 0) {
    bool digits = core.size() > v + 1;
    for (std::size_t i = v + 1; digits && i < core.size(); ++i)
      if (core[i] < '0' || core[i] > '9') digits = false;
    if (digits) core = core.substr(0, v);
  }
  return "https://arxiv.org/abs/" + std::string(core);
}

// 命中总数：<totalResults> 或带命名空间的 <opensearch:totalResults>
long total_results(std::string_view raw) {
  for (std::string_view const open : {"<opensearch:totalResults", "<totalResults"}) {
    auto const b = ci_find(raw, open);
    if (b == std::string_view::npos) continue;
    auto const gt = raw.find('>', b);
    if (gt == std::string_view::npos) continue;
    long v = 0;
    bool any = false;
    for (std::size_t i = gt + 1; i < raw.size() && raw[i] >= '0' && raw[i] <= '9'; ++i) {
      v = v * 10 + (raw[i] - '0');
      any = true;
    }
    if (any) return v;
  }
  return 0;
}

}  // namespace

std::string build_arxiv_query(ArxivConfig const& cfg, std::string const& search_query,
                              long max_results) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string url = cfg.base_url;
  url += "?search_query=";
  for (unsigned char c : search_query) {
    if (c == ' ') {  // arXiv 约定：空格 → '+'
      url += '+';
      continue;
    }
    bool const safe = std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
                      c == ':' || c == '(' || c == ')' || c == '*' || c == '/';
    if (safe) {
      url.push_back(static_cast<char>(c));
      continue;
    }
    url.push_back('%');
    url.push_back(kHex[c >> 4]);
    url.push_back(kHex[c & 0xF]);
  }
  url += "&start=0&max_results=";
  url += std::to_string(std::clamp(max_results, 1L, cfg.max_results_cap));
  return url;
}

nlohmann::json parse_arxiv_response(std::string_view raw, std::string const& query,
                                    long summary_chars) {
  auto entries = nlohmann::json::array();
  std::size_t pos = 0;
  std::string_view const kEntryClose = "</entry>";
  for (;;) {
    auto const b = ci_find(raw, "<entry", pos);
    if (b == std::string_view::npos) break;
    auto const e = ci_find(raw, kEntryClose, b + 6);
    if (e == std::string_view::npos) break;
    std::string_view const span = raw.substr(b, e + kEntryClose.size() - b);

    auto summary = normalize_space(decode_entities(elem_content(span, "summary")));
    if (summary_chars >= 0) summary = utf8_truncate(summary, static_cast<std::size_t>(summary_chars));

    std::string const title = normalize_space(decode_entities(elem_content(span, "title")));
    entries.push_back(nlohmann::json{
        {"id", clean_abs_id(elem_content(span, "id"))},
        {"title", title.empty() ? std::string("(无标题)") : title},
        {"authors", authors_string(span)},
        {"category", first_category(span)},
        {"published", normalize_space(decode_entities(elem_content(span, "published")))},
        {"updated", normalize_space(decode_entities(elem_content(span, "updated")))},
        {"summary", std::move(summary)},
    });

    pos = e + kEntryClose.size();
    if (pos <= b) break;  // 防死循环
  }
  return nlohmann::json{{"query", query},
                        {"total", total_results(raw)},
                        {"papers", std::move(entries)},
                        {"source", "arXiv API (Atom)"}};
}

ToolSpec make_arxiv_search_tool(std::shared_ptr<http::Transport> transport, ArxivConfig cfg) {
  ToolSpec spec = make_tool(
      "arxiv_search",
      "检索 arXiv 预印本论文（数学/物理/计算机/AI 等，免费、无需密钥）。"
      "支持 arXiv 检索语法：all: 全文、ti: 标题、au: 作者、cat: 分类（如 cat:cs.AI），"
      "AND/OR/NOT 与引号短语，如 ti:\"deep learning\" AND cat:cs.CL。"
      "返回 {query, total, papers:[{id,title,authors,category,published,updated,summary}]}，"
      "摘要已截断。文献调研、找相关工作、确认某方向是否已有论文时使用。"
      "无匹配会报错并建议换词。",
      SchemaBuilder()
          .prop("search_query", "string",
                "arXiv 检索式，如 Transformer、ti:\"graph attention\" AND cat:cs.LG")
          .prop("max_results", "integer", "最多返回论文数（1–10，默认 3）", /*required=*/false)
          .build(),
      [transport, cfg](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        auto q = arg<std::string>(args, "search_query");
        if (!q) return std::unexpected(q.error());
        if (normalize_space(*q).empty())
          return std::unexpected(ToolError{"搜索词不能为空。"});

        long max_results = cfg.default_max_results;
        if (auto it = args.find("max_results"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<long>() < 1 ||
              it->get<long>() > cfg.max_results_cap)
            return std::unexpected(ToolError{"argument 'max_results' invalid: 需为 1–" +
                                             std::to_string(cfg.max_results_cap) + " 的整数"});
          max_results = it->get<long>();
        }

        http::HttpRequest req;
        req.url = build_arxiv_query(cfg, *q, max_results);
        req.method = "GET";
        req.timeout_ms = cfg.timeout_ms;
        req.ca_info = cfg.ca_info;
        req.headers = {"Accept: application/atom+xml, application/xml, text/xml"};

        auto r = http_json(*transport, req, ctx.cancel);
        if (!r) return std::unexpected(r.error());
        if (r->status == 406) {
          // arXiv 官方 API 文档：406 表示请求被拒（常见于偶发限流/过滤），建议重试一次
          r = http_json(*transport, req, ctx.cancel);
          if (!r) return std::unexpected(r.error());
        }
        if (auto e = http_status_error(r->status, "arXiv 检索服务", r->raw)) {
          if (r->status == 406)
            return std::unexpected(ToolError{
                "arXiv 拒绝了该查询（HTTP 406，可能是临时过滤或限流）。"
                "请换更常见的检索词，或改用 cat: 分类前缀（如 cat:cs.AI）重试。"});
          return std::unexpected(*e);
        }

        auto parsed = parse_arxiv_response(r->raw, *q, cfg.default_summary_chars);
        if (parsed.value("papers", nlohmann::json::array()).empty()) {
          if (parsed.value("total", 0L) == 0)
            return std::unexpected(ToolError{
                "未找到匹配 arXiv 论文（total=0）。请换更宽的关键词，"
                "或用 ti:/au:/cat: 前缀缩小范围。"});
          return std::unexpected(ToolError{
              "arXiv 返回了 total>0 却没有论文条目（可能限流或响应异常），请稍后重试。"});
        }
        return parsed;
      },
      /*read_only=*/true);
  spec.ttl = cfg.ttl;  // 论文快照 60 分钟后视为可能过期
  return spec;
}

}  // namespace cppchain::tool