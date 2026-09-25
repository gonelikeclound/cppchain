// cppchain —— src/tool/fetch_url.cpp
#include "cppchain/tool/fetch_url.hpp"

#include <algorithm>
#include <cctype>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "cppchain/tool/http_json.hpp"

namespace cppchain::tool {
namespace {

// ---------- HTML → 文本的一组纯函数（供离线单测） ----------

// 大小写不敏感子串查找
std::size_t ci_find(std::string_view s, std::string_view needle, std::size_t from = 0) {
  if (needle.empty() || s.size() < needle.size()) return std::string_view::npos;
  for (std::size_t i = from; i + needle.size() <= s.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(s[i + j])) !=
          std::tolower(static_cast<unsigned char>(needle[j]))) {
        match = false;
        break;
      }
    }
    if (match) return i;
  }
  return std::string_view::npos;
}

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

// Unicode 码点 → UTF-8 字节
std::string encode_utf8(long cp) {
  std::string out;
  if (cp < 0 || cp > 0x10FFFF) return out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

// 命名实体表（名 → UTF-8 文本）
struct NamedEntity {
  std::string_view name;
  std::string_view value;
};
constexpr NamedEntity kEntities[] = {
    {"amp", "&"},        {"lt", "<"},       {"gt", ">"},      {"quot", "\""},
    {"apos", "'"},       {"nbsp", " "}, {"mdash", "—"}, {"ndash", "–"},
    {"hellip", "…"}, {"lsquo", "‘"}, {"rsquo", "’"}, {"ldquo", "“"},
    {"rdquo", "”"}, {"middot", "·"}, {"copy", "©"}, {"reg", "®"},
    {"deg", "°"},   {"plusmn", "±"}, {"times", "×"}, {"divide", "÷"},
    {"cent", "¢"},  {"pound", "£"},  {"euro", "€"},  {"yen", "¥"},
    {"micro", "µ"}, {"para", "¶"},   {"infin", "∞"}, {"ne", "≠"},
    {"le", "≤"},    {"ge", "≥"},    {"pi", "π"},   {"alpha", "α"},
    {"beta", "β"},  {"gamma", "γ"},  {"delta", "δ"}, {"sum", "∑"},
    {"radic", "√"}};

// 解码实体：&amp; &lt; …、数字 &#NNN; / &#xHH;。未知实体原样保留（& 脱出）。
std::string decode_entities(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '&') {
      out.push_back(s[i]);
      ++i;
      continue;
    }
    auto const semi = s.find(';', i);
    if (semi == std::string_view::npos || semi - i > 12) {  // 不是实体：& 原样
      out.push_back('&');
      ++i;
      continue;
    }
    std::string_view const ent = s.substr(i + 1, semi - i - 1);  // 去掉 & 与 ;
    bool decoded = false;

    if (ent.size() >= 2 && ent[0] == '#') {  // 数字实体
      long cp = -1;
      if (ent.size() >= 3 && (ent[1] == 'x' || ent[1] == 'X')) {
        try {
          cp = std::stol(std::string(ent.substr(2)), nullptr, 16);
        } catch (...) {
          cp = -1;
        }
        if (cp < 0 || cp > 0x10FFFF) cp = -1;
      } else {
        try {
          cp = std::stol(std::string(ent.substr(1)));
        } catch (...) {
          cp = -1;
        }
      }
      if (cp >= 0) {
        out += encode_utf8(cp);
        decoded = true;
      }
    } else {
      for (auto const& e : kEntities) {
        if (e.name == ent) {
          out.append(e.value);
          decoded = true;
          break;
        }
      }
    }

    if (decoded) {
      i = semi + 1;
    } else {
      out.push_back('&');
      ++i;
    }
  }
  return out;
}

// 压平空白：行内空白（空格/Tab/CR）→ 单空格；连续换行 → 单换行；去行首尾空白
std::string collapse_whitespace(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool pending_space = false;  // 行内"待打"的一个空格
  bool at_line_start = true;   // 行首：吸收前导空白
  for (unsigned char c : s) {
    if (c == '\n') {
      if (!at_line_start) {
        out.push_back('\n');
        at_line_start = true;
      }
      pending_space = false;
      continue;
    }
    if (c == '\t' || c == '\r' || c == ' ') {
      if (!at_line_start) pending_space = true;
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(static_cast<char>(c));
    at_line_start = false;
  }
  while (!out.empty() && out.back() == '\n') out.pop_back();  // 去结尾空行
  return out;
}

std::string_view trim_ascii(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

// UTF-8 安全截断（不切断多字节字符，尾部加省略号）
std::string utf8_truncate(std::string_view s, std::size_t max_chars) {
  if (s.size() <= max_chars) return std::string(s);
  std::size_t end = max_chars;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;  // 回退到字符边界
  return std::string(s.substr(0, end)) + "…";
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