// cppchain —— src/tool/file_extract.cpp
// problems/02 追加 S4/S5：结构化提取（json / yaml / markdown / xml）+ 采样跳读。
//
// 设计取向：
//   · 全部**只读**、确定性、UTF-8 安全；解析失败 fail-closed 回喂（不猜、不静默返回半截）。
//   · 返回文本一律受 max_chars 约束——定位类工具的价值就在"只取该看的那段"。
//   · 找不到 selector **不是错误**（found=false + 目录提示），错误只留给"文件读不了/格式坏了"。
#include "cppchain/tool/file_tools.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cppchain::tool {
namespace {

using Json = nlohmann::json;

// UTF-8 安全：不超过 max_bytes 的最大前缀长度
std::size_t utf8_prefix_len(std::string_view s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s.size();
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return end;
}

// 去掉尾部不完整的多字节字符（窗口按字节读，可能切在字符中间）
void trim_incomplete_utf8(std::string& s) {
  std::size_t i = s.size();
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) --i;
  if (i == 0) {  // 全是续字节（异常输入）：保守清空
    s.clear();
    return;
  }
  unsigned char const lead = static_cast<unsigned char>(s[i - 1]);
  std::size_t const need = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
  std::size_t const have = s.size() - (i - 1);
  if (have < need) s.resize(i - 1);
}

// 截断并标注（返回 truncated 标志）
std::string clip_text(std::string_view s, std::size_t max_chars, bool& truncated) {
  std::size_t const keep = utf8_prefix_len(s, max_chars);
  truncated = keep < s.size();
  std::string out(s.substr(0, keep));
  if (truncated) out += "…";
  return out;
}

std::string trim(std::string_view s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

std::string unquote(std::string v) {
  if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                        (v.front() == '\'' && v.back() == '\'')))
    return v.substr(1, v.size() - 2);
  return v;
}

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

std::expected<std::string, ToolError> read_text_file(std::filesystem::path const& p,
                                                     std::uintmax_t max_bytes) {
  std::error_code ec;
  auto const size = std::filesystem::file_size(p, ec);
  if (ec) return std::unexpected(ToolError{"无法读取文件大小：" + ec.message()});
  if (size > max_bytes)
    return std::unexpected(ToolError{"文件过大（" + std::to_string(size) + " 字节，解析类提取上限 " +
                                     std::to_string(max_bytes) + "）；请先用 file_grep 定位。"});
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::unexpected(ToolError{"无法打开文件"});
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
  if (data.find('\0') != std::string::npos)
    return std::unexpected(ToolError{"检测到二进制内容（NUL 字节），不适合结构化提取。"});
  return data;
}

// ---- 格式判定 ----
std::string detect_format(std::filesystem::path const& p, std::string const& explicit_fmt) {
  if (!explicit_fmt.empty() && explicit_fmt != "auto") return explicit_fmt;
  auto ext = lower_ascii(p.extension().string());
  if (ext == ".json") return "json";
  if (ext == ".yaml" || ext == ".yml") return "yaml";
  if (ext == ".xml" || ext == ".html" || ext == ".htm") return "xml";
  return "markdown";  // .md/.markdown/.txt/无扩展名：按文本结构处理
}

// ---- JSON：点路径 → JSON Pointer ----
std::string dot_path_to_pointer(std::string const& path) {
  if (path.empty()) return "";
  if (path.front() == '/') return path;  // 已是 RFC 6901
  std::string out;
  std::string token;
  auto flush = [&](std::string_view t) {
    // 支持 a[0] / a[0][1] 形式
    std::size_t i = 0;
    std::string base;
    while (i < t.size() && t[i] != '[') base.push_back(t[i++]);
    out += "/" + base;
    while (i < t.size() && t[i] == '[') {
      std::size_t e = t.find(']', i);
      if (e == std::string_view::npos) break;
      out += "/" + std::string(t.substr(i + 1, e - i - 1));
      i = e + 1;
    }
  };
  for (char c : path) {
    if (c == '.') { flush(token); token.clear(); }
    else token.push_back(c);
  }
  flush(token);
  return out;
}

std::expected<Json, ToolError> extract_json(std::string const& text, std::string const& selector,
                                            bool& found) {
  Json doc = Json::parse(text, nullptr, false);
  if (doc.is_discarded())
    return std::unexpected(ToolError{"JSON 解析失败（文件不是合法 JSON）。"});
  if (selector.empty()) { found = true; return doc; }
  std::string const ptr = dot_path_to_pointer(selector);
  try {
    Json const& sub = doc.at(Json::json_pointer(ptr));
    found = true;
    return sub;
  } catch (Json::exception const&) {
    found = false;
    return Json();
  }
}

// ---- Markdown：标题识别（ATX + 中文「第X章/回」） ----
struct MdHeading {
  std::size_t line = 0;  // 0 起
  int level = 0;         // 0 = 不是标题（parse_md_heading 的唯一判据）
  std::string title;
};

bool is_chapter_heading(std::string_view line) {
  // 行首「第」+ ≤8 个 UTF-8 字符内出现 回/章/节/卷/部
  static constexpr std::string_view kWord = "\xE7\xAC\xAC";  // 第
  if (line.size() < kWord.size() || line.compare(0, kWord.size(), kWord) != 0) return false;
  std::size_t pos = kWord.size(), chars = 0;
  while (pos < line.size() && chars < 8) {
    unsigned char const c = static_cast<unsigned char>(line[pos]);
    std::size_t const len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    for (std::string_view const m : {"\xE5\x9B\x9E", "\xE7\xAB\xA0", "\xE8\x8A\x82",
                                     "\xE5\x8D\xB7", "\xE9\x83\xA8"})  // 回 章 节 卷 部
      if (line.size() >= pos + m.size() && line.compare(pos, m.size(), m) == 0) return true;
    pos += len;
    ++chars;
  }
  return false;
}

MdHeading parse_md_heading(std::string_view line, std::size_t line_no) {
  MdHeading h;
  h.line = line_no;
  std::size_t i = 0;
  while (i < line.size() && i < 3 && line[i] == ' ') ++i;  // 最多 3 个前导空格
  if (i < line.size() && line[i] == '#') {
    std::size_t n = 0;
    std::size_t j = i;
    while (j < line.size() && line[j] == '#') { ++j; ++n; }
    if (n >= 1 && n <= 6 && (j == line.size() || std::isspace(static_cast<unsigned char>(line[j])))) {
      h.level = static_cast<int>(n);
      h.title = trim(line.substr(j));
      return h;
    }
  }
  if (is_chapter_heading(line)) {
    h.level = 1;
    h.title = trim(line);
  }
  return h;
}

std::vector<std::string> split_lines(std::string const& text) {
  std::vector<std::string> lines;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t nl = text.find('\n', pos);
    std::size_t end = nl == std::string::npos ? text.size() : nl;
    std::string line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return lines;
}

std::expected<Json, ToolError> extract_markdown(std::string const& text,
                                                std::string const& selector, bool& found) {
  auto const lines = split_lines(text);
  std::vector<MdHeading> headings;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    MdHeading h = parse_md_heading(lines[i], i);
    if (h.level > 0) headings.push_back(std::move(h));
  }

  // 空 selector → 目录（标题 + 行号）
  if (selector.empty()) {
    found = true;
    Json out = Json::array();
    for (auto const& h : headings)
      out.push_back(Json{{"line", h.line + 1}, {"level", h.level}, {"title", h.title}});
    return out;
  }

  std::string const needle = lower_ascii(selector);
  std::size_t hit = headings.size();
  for (std::size_t i = 0; i < headings.size(); ++i)
    if (lower_ascii(headings[i].title).find(needle) != std::string::npos) { hit = i; break; }
  if (hit == headings.size()) {
    found = false;
    Json hint = Json::array();
    for (auto const& h : headings) hint.push_back(h.title);
    return hint;  // 上层把数组当"可用标题提示"
  }
  found = true;
  std::size_t end_line = lines.size();
  for (std::size_t i = hit + 1; i < headings.size(); ++i)
    if (headings[i].level <= headings[hit].level) { end_line = headings[i].line; break; }
  std::string body;
  for (std::size_t i = headings[hit].line; i < end_line; ++i) {
    body += lines[i];
    body += "\n";
  }
  return Json{{"heading", headings[hit].title},
              {"level", headings[hit].level},
              {"line", headings[hit].line + 1},
              {"body", body}};
}

// ---- YAML（常用子集：缩进映射 + 块列表；锚点/多文档不支持） ----
struct YLine {
  std::size_t indent = 0;
  std::string content;
};

std::vector<YLine> yaml_lines(std::string const& text) {
  std::vector<YLine> out;
  for (std::string const& raw : split_lines(text)) {
    std::size_t i = 0, indent = 0;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) {
      indent += raw[i] == '\t' ? 2 : 1;
      ++i;
    }
    std::string content = raw.substr(i);
    if (content.empty() || content[0] == '#') continue;
    // 行内注释：仅剥「空格 + #」，且不在引号内（保守）
    bool in_s = false, in_d = false;
    for (std::size_t k = 0; k < content.size(); ++k) {
      char const c = content[k];
      if (c == '\'' && !in_d) in_s = !in_s;
      else if (c == '"' && !in_s) in_d = !in_d;
      else if (c == '#' && !in_s && !in_d && k > 0 &&
               std::isspace(static_cast<unsigned char>(content[k - 1]))) {
        content = content.substr(0, k);
        break;
      }
    }
    content = trim(content);
    if (content.empty()) continue;
    out.push_back(YLine{indent, std::move(content)});
  }
  return out;
}

struct YNode {
  std::string key;
  std::string scalar;
  std::vector<YNode> kids;
  bool is_list = false;
};

std::size_t parse_yaml_block(std::vector<YLine> const& ls, std::size_t i, std::size_t block_indent,
                             std::vector<YNode>& out);

// 把连续 "- ..." 项解析进 target（target.is_list=true）；返回下一个未消费行下标
std::size_t parse_yaml_list(std::vector<YLine> const& ls, std::size_t i, std::size_t block_indent,
                            YNode& target) {
  target.is_list = true;
  while (i < ls.size() && ls[i].indent == block_indent &&
         (ls[i].content == "-" || ls[i].content.rfind("- ", 0) == 0)) {
    std::string item = ls[i].content == "-" ? "" : trim(ls[i].content.substr(2));
    YNode node;
    if (!item.empty()) {
      auto const colon = item.find(':');
      if (colon != std::string::npos && item.find("://") == std::string::npos) {
        node.key = trim(item.substr(0, colon));
        std::string rest = trim(item.substr(colon + 1));
        if (!rest.empty()) node.scalar = unquote(rest);
      } else {
        node.scalar = unquote(item);
      }
    }
    ++i;
    if (i < ls.size() && ls[i].indent > block_indent)
      i = parse_yaml_block(ls, i, ls[i].indent, node.kids);
    target.kids.push_back(std::move(node));
  }
  return i;
}

// 在 block_indent 这一层解析键值；返回下一个未消费行下标
std::size_t parse_yaml_block(std::vector<YLine> const& ls, std::size_t i, std::size_t block_indent,
                             std::vector<YNode>& out) {
  while (i < ls.size()) {
    if (ls[i].indent < block_indent) break;
    if (ls[i].indent > block_indent) { ++i; continue; }  // 异常缩进：跳过（宽容）
    std::string const& c = ls[i].content;
    if (c == "-" || c.rfind("- ", 0) == 0) {
      YNode list;
      i = parse_yaml_list(ls, i, block_indent, list);
      out.push_back(std::move(list));
      continue;
    }
    auto const colon = c.find(':');
    if (colon == std::string::npos) { ++i; continue; }  // 非键值行：跳过（宽容）
    YNode node;
    node.key = trim(c.substr(0, colon));
    std::string rest = trim(c.substr(colon + 1));
    ++i;
    if (!rest.empty()) {
      node.scalar = unquote(rest);
    } else if (i < ls.size() && ls[i].indent > block_indent) {
      // 关键：若是块列表，列表项直接挂在这个键节点上（ports: - 80），
      // 不再包一层空 key 的 list 子节点——否则 "ports.0" 查不到。
      if (ls[i].content == "-" || ls[i].content.rfind("- ", 0) == 0)
        i = parse_yaml_list(ls, i, ls[i].indent, node);
      else
        i = parse_yaml_block(ls, i, ls[i].indent, node.kids);
    }
    out.push_back(std::move(node));
  }
  return i;
}

std::vector<YNode> parse_yaml(std::string const& text) {
  auto const ls = yaml_lines(text);
  std::vector<YNode> roots;
  if (ls.empty()) return roots;
  parse_yaml_block(ls, 0, ls.front().indent, roots);
  return roots;
}

void emit_yaml(YNode const& n, int depth, std::string& out) {
  std::string const pad(static_cast<std::size_t>(depth) * 2, ' ');
  if (n.is_list) {
    // 有 key 的列表：key 占一层，条目缩进一层；无 key（顶层列表）：条目就在当前层
    std::string const item_pad = n.key.empty() ? pad : pad + "  ";
    int const child_depth = n.key.empty() ? depth + 1 : depth + 2;
    if (!n.key.empty()) out += pad + n.key + ":\n";
    for (auto const& k : n.kids) {
      if (!k.key.empty()) {
        out += item_pad + "- " + k.key + ":" + (k.scalar.empty() ? "" : " " + k.scalar) + "\n";
        for (auto const& kk : k.kids) emit_yaml(kk, child_depth, out);
      } else {
        out += item_pad + "- " + k.scalar + "\n";
        for (auto const& kk : k.kids) emit_yaml(kk, child_depth, out);
      }
    }
    return;
  }
  out += pad + n.key + ":" + (n.scalar.empty() ? "" : " " + n.scalar) + "\n";
  for (auto const& k : n.kids) emit_yaml(k, depth + 1, out);
}

YNode const* find_yaml(YNode const& n, std::string_view seg) {
  if (n.is_list) {
    for (char c : seg)
      if (!std::isdigit(static_cast<unsigned char>(c))) return nullptr;
    std::size_t idx = 0;
    try { idx = static_cast<std::size_t>(std::stoul(std::string(seg))); } catch (...) { return nullptr; }
    return idx < n.kids.size() ? &n.kids[idx] : nullptr;
  }
  for (auto const& k : n.kids)
    if (k.key == seg) return &k;
  return nullptr;
}

std::expected<std::string, ToolError> extract_yaml(std::string const& text,
                                                   std::string const& selector, bool& found) {
  auto const roots = parse_yaml(text);
  if (selector.empty()) { found = true; return text; }

  // 根是虚拟容器：先在所有顶层键里找第一段
  std::vector<std::string> segs;
  {
    std::string cur;
    for (char c : selector) {
      if (c == '.') { segs.push_back(cur); cur.clear(); }
      else cur.push_back(c);
    }
    segs.push_back(cur);
  }
  if (segs.empty() || segs[0].empty()) { found = false; return std::string{}; }

  YNode const* cur = nullptr;
  for (auto const& r : roots)
    if (!r.is_list && r.key == segs[0]) { cur = &r; break; }
  if (cur == nullptr) { found = false; return std::string{}; }

  for (std::size_t i = 1; i < segs.size(); ++i) {
    cur = find_yaml(*cur, segs[i]);
    if (cur == nullptr) { found = false; return std::string{}; }
  }
  found = true;
  if (!cur->scalar.empty() && cur->kids.empty()) return cur->scalar;
  std::string out;
  emit_yaml(*cur, 0, out);
  return out;
}

// ---- XML（良构无命名空间的常用子集） ----
struct XNode {
  std::string name;
  std::size_t inner_begin = 0;
  std::size_t inner_end = 0;
  int parent = -1;
  std::vector<int> kids;
};

std::vector<XNode> parse_xml(std::string const& text) {
  std::vector<XNode> nodes;
  nodes.push_back(XNode{"", 0, 0, -1, {}});  // 虚拟根
  std::vector<int> st{0};
  std::size_t pos = 0;
  while ((pos = text.find('<', pos)) != std::string::npos) {
    if (text.compare(pos, 4, "<!--") == 0) {
      auto e = text.find("-->", pos + 4);
      if (e == std::string::npos) break;
      pos = e + 3;
      continue;
    }
    if (text.compare(pos, 9, "<![CDATA[") == 0) {
      auto e = text.find("]]>", pos + 9);
      if (e == std::string::npos) break;
      pos = e + 3;
      continue;
    }
    if (text.compare(pos, 2, "<?") == 0) {
      auto e = text.find("?>", pos + 2);
      if (e == std::string::npos) break;
      pos = e + 2;
      continue;
    }
    if (text.compare(pos, 2, "<!") == 0) {
      auto e = text.find('>', pos + 2);
      if (e == std::string::npos) break;
      pos = e + 1;
      continue;
    }
    if (pos + 1 >= text.size()) break;  // 末尾孤立的 '<'
    bool const closing = text[pos + 1] == '/';
    std::size_t p = pos + (closing ? 2 : 1);
    std::size_t const name_start = p;
    while (p < text.size() && !std::isspace(static_cast<unsigned char>(text[p])) &&
           text[p] != '>' && text[p] != '/')
      ++p;
    std::string const name = text.substr(name_start, p - name_start);
    bool self = false;
    std::size_t q = p;
    while (q < text.size() && text[q] != '>') {
      if (text[q] == '"' || text[q] == '\'') {
        char const quote = text[q++];                 // 跳过开引号
        while (q < text.size() && text[q] != quote) ++q;
        if (q < text.size()) ++q;                     // 跳过闭引号（否则下一轮把闭引号当开引号）
      } else {
        if (text[q] == '/') self = true;
        ++q;
      }
    }
    if (q >= text.size() || name.empty()) break;
    std::size_t const after = q + 1;
    if (closing) {
      if (st.size() > 1 && nodes[static_cast<std::size_t>(st.back())].name == name) {
        nodes[static_cast<std::size_t>(st.back())].inner_end = pos;
        st.pop_back();
      } else {
        while (st.size() > 1 && nodes[static_cast<std::size_t>(st.back())].name != name) {
          nodes[static_cast<std::size_t>(st.back())].inner_end = pos;
          st.pop_back();
        }
        if (st.size() > 1) {
          nodes[static_cast<std::size_t>(st.back())].inner_end = pos;
          st.pop_back();
        }
      }
    } else {
      int const idx = static_cast<int>(nodes.size());
      nodes.push_back(XNode{name, after, after, st.back(), {}});
      nodes[static_cast<std::size_t>(st.back())].kids.push_back(idx);
      if (!self)
        st.push_back(idx);
      else
        nodes[static_cast<std::size_t>(idx)].inner_end = after;
    }
    pos = after;
  }
  while (st.size() > 1) {
    nodes[static_cast<std::size_t>(st.back())].inner_end = text.size();
    st.pop_back();
  }
  return nodes;
}

std::string xml_path_of(std::vector<XNode> const& nodes, int idx) {
  std::vector<std::string> parts;
  while (idx > 0) {
    XNode const& n = nodes[static_cast<std::size_t>(idx)];
    int const parent = n.parent;
    int same = 0, nth = 0;
    if (parent >= 0)
      for (int k : nodes[static_cast<std::size_t>(parent)].kids)
        if (nodes[static_cast<std::size_t>(k)].name == n.name) {
          ++same;
          if (k == idx) nth = same;
        }
    parts.push_back(same > 1 ? n.name + "[" + std::to_string(nth) + "]" : n.name);
    idx = parent;
  }
  std::string out;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it)
    out += (out.empty() ? "" : "/") + *it;
  return out;
}

std::expected<std::string, ToolError> extract_xml(std::string const& text,
                                                  std::string const& selector, bool& found) {
  auto const nodes = parse_xml(text);
  if (nodes.size() <= 1) return std::unexpected(ToolError{"XML 解析失败（未找到元素）。"});

  // 空 selector → 元素目录（深度 ≤3，带行号）
  if (selector.empty()) {
    found = true;
    // 预计算行号（行首下标 + 二分），避免逐节点扫描全文
    std::vector<std::size_t> line_starts{0};
    for (std::size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n') line_starts.push_back(i + 1);
    auto line_of = [&](std::size_t off) {
      return static_cast<std::size_t>(
                 std::upper_bound(line_starts.begin(), line_starts.end(), off) -
                 line_starts.begin());
    };
    std::string out;
    std::vector<int> stack{0};
    while (!stack.empty()) {
      int const idx = stack.back();
      stack.pop_back();
      if (idx > 0) {
        out += "L" + std::to_string(line_of(nodes[static_cast<std::size_t>(idx)].inner_begin)) +
               " " + xml_path_of(nodes, idx) + "\n";
      }
      int depth = 0;
      for (int p = idx; p > 0; p = nodes[static_cast<std::size_t>(p)].parent) ++depth;
      // 子节点按文档序输出：先逆序压栈（栈后进先出）
      if (depth < 3)
        for (auto it = nodes[static_cast<std::size_t>(idx)].kids.rbegin();
             it != nodes[static_cast<std::size_t>(idx)].kids.rend(); ++it)
          stack.push_back(*it);
    }
    return out;
  }

  std::vector<std::string> segs;
  {
    std::string cur;
    for (char c : selector) {
      if (c == '/') {
        if (!cur.empty()) segs.push_back(cur);
        cur.clear();
      } else cur.push_back(c);
    }
    if (!cur.empty()) segs.push_back(cur);
  }

  int cur = 0;
  for (std::string const& seg : segs) {
    std::string name = seg;
    long want = 1;  // 1 起
    if (auto const lb = seg.find('['); lb != std::string::npos && seg.back() == ']') {
      name = seg.substr(0, lb);
      try { want = std::stol(seg.substr(lb + 1, seg.size() - lb - 2)); } catch (...) { want = 1; }
    }
    int next = -1;
    long seen = 0;
    for (int k : nodes[static_cast<std::size_t>(cur)].kids)
      if (nodes[static_cast<std::size_t>(k)].name == name && ++seen == want) { next = k; break; }
    if (next < 0) { found = false; return std::string{}; }
    cur = next;
  }
  found = true;
  XNode const& n = nodes[static_cast<std::size_t>(cur)];
  return text.substr(n.inner_begin, n.inner_end > n.inner_begin ? n.inner_end - n.inner_begin : 0);
}

}  // namespace

ToolSpec make_file_extract_tool(FileToolsConfig cfg) {
  auto const root = std::filesystem::absolute(cfg.root_dir);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  ToolSpec spec = make_tool(
      "file_extract",
      "按文档结构提取子内容（只读，无副作用）。适用于**已知结构**的定位："
      "json 用 JSON Pointer 或点路径（a.b[0]）；yaml 用点键路径（server.ports.1）；"
      "markdown 用标题子串，空 selector 先返回目录（标题+行号）；xml 用元素路径"
      "（root/item[2]/name，[n] 从 1 起），空 selector 返回元素目录。"
      "返回 {path, format, selector, found, size, text, truncated}；找不到 selector 时 "
      "found=false（不是错误），可先用空 selector 看目录。拿到片段后无需再整文件读取。",
      SchemaBuilder()
          .prop("path", "string", "相对沙箱根目录的文件路径，如 config/app.json")
          .prop("format", "string", "auto（默认，按扩展名）/ json / yaml / markdown / xml",
                /*required=*/false)
          .prop("selector", "string",
                "结构路径：json 指针或点路径；yaml 点键路径；markdown 标题子串；"
                "xml 元素路径。空 = 整份文档 / 目录",
                /*required=*/false)
          .prop("max_chars", "integer", "返回文本上限（1–8000，默认 4000）", /*required=*/false)
          .build(),
      [cfg, root](Json const& args, ToolContext& ctx) -> std::expected<Json, ToolError> {
        (void)ctx;
        auto path = arg<std::string>(args, "path");
        if (!path) return std::unexpected(path.error());
        auto const target = resolve_path_in_root(root, *path);
        if (!target) return std::unexpected(ToolError{"路径越界或非法：" + *path});

        std::string format = "auto";
        if (auto f = arg<std::string>(args, "format", /*required=*/false); f && !f->empty())
          format = lower_ascii(*f);
        if (format != "auto" && format != "json" && format != "yaml" &&
            format != "markdown" && format != "xml")
          return std::unexpected(
              ToolError{"argument 'format' invalid: 需为 auto/json/yaml/markdown/xml"});

        std::string selector;
        if (auto s = arg<std::string>(args, "selector", /*required=*/false); s) selector = *s;

        long max_chars = cfg.default_extract_chars;
        if (auto it = args.find("max_chars"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<long>() < 1 ||
              it->get<long>() > cfg.max_extract_chars)
            return std::unexpected(ToolError{"argument 'max_chars' invalid: 需为 1–" +
                                             std::to_string(cfg.max_extract_chars) + " 的整数"});
          max_chars = it->get<long>();
        }

        std::error_code e2;
        if (!std::filesystem::exists(*target, e2))
          return std::unexpected(ToolError{"文件不存在：「" + *path + "」"});
        if (std::filesystem::is_directory(*target, e2))
          return std::unexpected(ToolError{"「" + *path + "」是目录；请给文件路径。"});

        auto const fmt = detect_format(*target, format);
        auto data = read_text_file(*target, cfg.max_extract_file_bytes);
        if (!data) return std::unexpected(data.error());

        bool found = false;
        std::string text;
        std::string hint;
        if (fmt == "json") {
          auto r = extract_json(*data, selector, found);
          if (!r) return std::unexpected(r.error());
          if (found) text = r->is_string() ? r->get<std::string>() : r->dump(2);
        } else if (fmt == "yaml") {
          auto r = extract_yaml(*data, selector, found);
          if (!r) return std::unexpected(r.error());
          text = *r;
        } else if (fmt == "markdown") {
          auto r = extract_markdown(*data, selector, found);
          if (!r) return std::unexpected(r.error());
          if (!found && r->is_array()) {
            std::string titles;
            for (Json const& h : *r) titles += (titles.empty() ? "" : "、") + h.get<std::string>();
            hint = "没有匹配的标题。可用标题：" + titles;
          } else if (found && r->is_array()) {
            for (Json const& h : *r)
              text += "L" + std::to_string(h.value("line", 0)) + " [" +
                      std::to_string(h.value("level", 1)) + "] " + h.value("title", "") + "\n";
          } else if (found) {
            text = r->value("body", "");
          }
        } else {  // xml
          auto r = extract_xml(*data, selector, found);
          if (!r) return std::unexpected(r.error());
          if (!found) hint = "没有匹配的元素。可先用空 selector 看元素目录。";
          text = *r;
        }

        bool truncated = false;
        text = clip_text(text, static_cast<std::size_t>(max_chars), truncated);
        Json out{{"path", *path},
                 {"format", fmt},
                 {"selector", selector},
                 {"found", found},
                 {"size", static_cast<long long>(data->size())},
                 {"text", text},
                 {"truncated", truncated}};
        if (!hint.empty()) out["hint"] = hint;
        return out;
      },
      /*read_only=*/true);
  spec.ttl = cfg.ttl;
  return spec;
}

ToolSpec make_file_sample_tool(FileToolsConfig cfg) {
  auto const root = std::filesystem::absolute(cfg.root_dir);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  ToolSpec spec = make_tool(
      "file_sample",
      "采样跳读（只读）：返回文件的 head / middle / tail 三段（UTF-8 安全），用于"
      "快速建立全局印象、看风格或找线索。返回 {path, size, windows:[{where,offset,text}], note}。"
      "**不保证覆盖全文**——「有没有/一共几条/是否提到 X」这类完整性问题请改用 file_grep 或分页读全，"
      "不要凭采样下否定结论。",
      SchemaBuilder()
          .prop("path", "string", "相对沙箱根目录的文件路径")
          .prop("chars_per_window", "integer",
                "每段字节数（1–2000，默认 400）；文件不大时会整份返回",
                /*required=*/false)
          .build(),
      [cfg, root](Json const& args, ToolContext& ctx) -> std::expected<Json, ToolError> {
        (void)ctx;
        auto path = arg<std::string>(args, "path");
        if (!path) return std::unexpected(path.error());
        auto const target = resolve_path_in_root(root, *path);
        if (!target) return std::unexpected(ToolError{"路径越界或非法：" + *path});

        long win = cfg.default_sample_chars;
        if (auto it = args.find("chars_per_window"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<long>() < 1 ||
              it->get<long>() > cfg.max_sample_chars)
            return std::unexpected(ToolError{"argument 'chars_per_window' invalid: 需为 1–" +
                                             std::to_string(cfg.max_sample_chars) + " 的整数"});
          win = it->get<long>();
        }

        std::error_code e2;
        if (!std::filesystem::exists(*target, e2))
          return std::unexpected(ToolError{"文件不存在：「" + *path + "」"});
        if (std::filesystem::is_directory(*target, e2))
          return std::unexpected(ToolError{"「" + *path + "」是目录；请给文件路径。"});
        auto const size = std::filesystem::file_size(*target, e2);
        std::ifstream in(*target, std::ios::binary);
        if (!in) return std::unexpected(ToolError{"无法打开文件：" + *path});

        auto const window_bytes = static_cast<std::uintmax_t>(win);
        auto read_at = [&](std::uintmax_t off) {
          off = std::min(off, size);
          in.clear();
          in.seekg(static_cast<std::streamoff>(off));
          std::string buf(static_cast<std::size_t>(std::min(window_bytes, size - off)), '\0');
          in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
          buf.resize(static_cast<std::size_t>(in.gcount()));
          trim_incomplete_utf8(buf);  // 窗口尾部可能截断在多字节字符中间
          return buf;
        };

        Json windows = Json::array();
        if (size <= window_bytes * 3) {
          windows.push_back(Json{{"where", "whole"}, {"offset", 0}, {"text", read_at(0)}});
        } else {
          std::uintmax_t const mid = (size - window_bytes) / 2;
          std::uintmax_t const tail = size - window_bytes;
          // 对齐到 UTF-8 字符起点（跳过续字节）
          auto align = [&](std::uintmax_t off) {
            if (off == 0) return std::uintmax_t{0};
            in.clear();
            in.seekg(static_cast<std::streamoff>(off));
            int c = in.get();
            while (c != EOF && (c & 0xC0) == 0x80) {
              ++off;
              c = in.get();
            }
            return off;
          };
          for (auto const& [where, off0] : std::vector<std::pair<char const*, std::uintmax_t>>{
                   {"head", 0}, {"middle", align(mid)}, {"tail", align(tail)}}) {
            windows.push_back(Json{{"where", where},
                                   {"offset", static_cast<long long>(off0)},
                                   {"text", read_at(off0)}});
          }
        }

        return Json{{"path", *path},
                    {"size", static_cast<long long>(size)},
                    {"windows", std::move(windows)},
                    {"note", "采样仅为空间分布（head/middle/tail），不保证覆盖全文；"
                             "完整性问题请用 file_grep 或 file_read 分页读全。"}};
      },
      /*read_only=*/true);
  spec.ttl = cfg.ttl;
  return spec;
}

}  // namespace cppchain::tool
