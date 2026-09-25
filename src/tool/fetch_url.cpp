// cppchain —— src/tool/fetch_url.cpp
#include "cppchain/tool/fetch_url.hpp"

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "cppchain/tool/http_json.hpp"
#include "cppchain/tool/text.hpp"

namespace cppchain::tool {
namespace {

// 剥除 script/style/注释 块（大小写不敏感、可嵌套扫描；不见结束标签则删到尾部）
void erase_ci_blocks(std::string& s, std::string_view open, std::string_view close) {
  std::size_t pos = 0;
  for (;;) {
    auto const b = ci_find(s, open, pos);
    if (b == std::string::npos) return;
    auto const e = ci_find(s, close, b + open.size());
    if (e == std::string::npos) {
      s.erase(b);
      return;
    }
    s.erase(b, e + close.size() - b);
    pos = b;  // 重叠/嵌套块：原地继续扫描
  }
}

// 块级标签：这些标签前补换行，让段落/列表/表行在纯文本中保持可读分隔
constexpr std::string_view kBlockTags[] = {
    "p",      "div",    "br",      "li",      "h1",   "h2",   "h3",   "h4",  "h5",  "h6",
    "table",  "tr",     "td",      "th",      "ul",   "ol",   "section", "article",
    "header", "footer", "blockquote", "pre",  "figcaption"};

// 逐字符重建：块级开始/结束标签前补 '\n'；标签本体原样保留（下一步统一剥除）
void insert_block_newlines(std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '<') {
      std::size_t j = i + 1;
      if (j < s.size() && s[j] == '/') ++j;  // 闭合标签允许
      std::size_t k = j;
      while (k < s.size() && std::isalpha(static_cast<unsigned char>(s[k]))) ++k;
      std::string_view const name(&s[j], k - j);
      bool const is_block =
          !name.empty() && (k == s.size() || s[k] == '>' || s[k] == '/' ||
                            std::isspace(static_cast<unsigned char>(s[k]))) &&
          std::find(std::begin(kBlockTags), std::end(kBlockTags), name) != std::end(kBlockTags);
      if (is_block && !out.empty() && out.back() != '\n') out.push_back('\n');
    }
    out.push_back(s[i]);
    ++i;
  }
  s = std::move(out);
}

// 剥除全部 <...> 标签（含未配对 '<' 的宽容处理：找不到 '>' 则原样保留剩余）
void strip_tags(std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '<') {
      auto const gt = s.find('>', i + 1);
      if (gt == std::string::npos) {  // 不完整的标签：保留剩余原样
        out.append(s, i, s.size() - i);
        break;
      }
      i = gt + 1;
      continue;
    }
    out.push_back(s[i]);
    ++i;
  }
  s = std::move(out);
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && ci_find(s, prefix, 0) == 0;
}

}  // namespace

std::string html_to_text(std::string_view html) {
  std::string s(html);
  // ① 先剥脚本/样式/注释块（避免其上残留的标签/伪文本混入正文）
  erase_ci_blocks(s, "<script", "</script>");
  erase_ci_blocks(s, "<style", "</style>");
  erase_ci_blocks(s, "<!--", "-->");
  // ② 块级标签前补换行（段落/列表/表格在纯文本里保持分隔）
  insert_block_newlines(s);
  // ③ 剥除全部标签
  strip_tags(s);
  // ④ 解码实体（剥标签之后再解，避免 &lt; 生成假标签）
  s = decode_entities(s);
  // ⑤ 压平空白
  return collapse_whitespace(s);
}

std::string extract_title(std::string_view html) {
  auto const b = ci_find(html, "<title");
  if (b == std::string_view::npos) return {};
  auto const gt = html.find('>', b);
  if (gt == std::string_view::npos) return {};
  auto const e = ci_find(html, "</title", gt + 1);
  std::string_view const raw(&html[gt + 1], (e == std::string_view::npos ? html.size() : e) - gt - 1);
  return std::string(trim_ascii(decode_entities(raw)));
}

ToolSpec make_fetch_url_tool(std::shared_ptr<http::Transport> transport, FetchUrlConfig cfg) {
  ToolSpec spec = make_tool(
      "fetch_url",
      "抓取指定网址的网页正文。联网搜索（web_search）只能拿到标题/摘要片段，"
      "需要看完整正文时用本工具打开详情页。仅支持 http/https。"
      "返回结构化 JSON：{url, status, title, content, source}；"
      "content 是去标签后的纯文本，已按 max_chars 截断。",
      SchemaBuilder()
          .prop("url", "string", "要抓取的网页 URL（http/https 开头）")
          .prop("max_chars", "integer",
                "返回正文的最大字符数（1–20000，默认 4000）；用于控制上下文占用",
                /*required=*/false)
          .build(),
      [transport, cfg](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        auto url = arg<std::string>(args, "url");
        if (!url) return std::unexpected(url.error());
        if (!starts_with_ci(*url, "http://") && !starts_with_ci(*url, "https://"))
          return std::unexpected(ToolError{
              "仅支持 http/https 链接：'" + *url +
              "'。请确认 URL 格式（应形如 https://example.com/page）。"});

        long max_chars = cfg.default_max_chars;
        if (auto it = args.find("max_chars"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<long>() < 1 ||
              it->get<long>() > cfg.max_chars_cap)
            return std::unexpected(ToolError{"argument 'max_chars' invalid: 需为 1–" +
                                             std::to_string(cfg.max_chars_cap) + " 的整数"});
          max_chars = it->get<long>();
        }

        http::HttpRequest req;
        req.url = *url;
        req.method = "GET";
        req.timeout_ms = cfg.timeout_ms;
        req.ca_info = cfg.ca_info;
        req.headers = {"Accept: text/html,application/xhtml+xml",
                       "User-Agent: cppchain-fetch-url/0.1"};

        auto r = http_json(*transport, req, ctx.cancel);
        if (!r) return std::unexpected(r.error());
        if (auto e = http_status_error(r->status, "网页", r->raw))
          return std::unexpected(*e);

        std::string text = html_to_text(r->raw);
        std::string const title = extract_title(r->raw);
        if (collapse_whitespace(trim_ascii(text)).empty())
          return std::unexpected(ToolError{
              "该页面没有可提取的正文（可能是 JS 渲染、登录墙或纯图片页）。"
              "请换一个 URL，或改用 web_search 从摘要片段组织信息。"});

        text = utf8_truncate(text, static_cast<std::size_t>(max_chars));
        return nlohmann::json{{"url", *url},
                              {"status", r->status},
                              {"title", title},
                              {"content", std::move(text)},
                              {"source", "html_to_text"}};
      },
      /*read_only=*/true);
  spec.ttl = cfg.ttl;  // 页面快照 30 分钟后视为可能过期
  return spec;
}

}  // namespace cppchain::tool