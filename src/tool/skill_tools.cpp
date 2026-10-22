// cppchain —— src/tool/skill_tools.cpp
// 见 include/cppchain/tool/skill_tools.hpp。宿主侧可信读：目录由环境变量指定，
// 工具内仍做「规范化 + 前缀包含」防逃逸（skill 名只允许相对、限 .md，越界拒绝）。
#include "cppchain/tool/skill_tools.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

// 工具通用：取字符串参数（缺失/非串 → 报错）
std::expected<std::string, ToolError> req_string(Json const& args, char const* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->is_string() || it->get<std::string>().empty())
    return std::unexpected(ToolError{"参数 '" + std::string(key) + "' 缺失或非字符串"});
  return it->get<std::string>();
}

// env 目录（存在才认）：WFR_SKILL_DIR / WFR_WIKI_DIR
std::optional<std::filesystem::path> env_dir(char const* key) {
  char const* v = std::getenv(key);
  if (v == nullptr || *v == '\0') return std::nullopt;
  std::error_code ec;
  std::filesystem::path p = std::filesystem::weakly_canonical(v, ec);
  if (ec || !std::filesystem::is_directory(p, ec)) return std::nullopt;
  return p;
}

// skill 文档可查找目录：WFR_SKILL_DIR 及其 wiki/ 子目录（skill 与 LLM Wiki 同一知识区）
std::vector<std::filesystem::path> skill_bases() {
  std::vector<std::filesystem::path> out;
  auto base = env_dir("WFR_SKILL_DIR");
  if (!base) return out;
  out.push_back(*base);
  std::error_code ec;
  std::filesystem::path sub = *base / "wiki";
  if (std::filesystem::is_directory(sub, ec)) out.push_back(sub);
  return out;
}

// 前缀包含守卫（词法 + 符号链接解析后质押在同一目录树内）
bool within(std::filesystem::path const& base, std::filesystem::path const& p) {
  auto const bs = base.generic_string();
  auto const ps = p.generic_string();
  return ps == bs || (ps.size() > bs.size() && ps.compare(0, bs.size(), bs) == 0 &&
                      ps[bs.size()] == '/');
}

// skill 名 → 相对路径（锁 .md；拒绝对路径/.. 逃逸）
std::optional<std::filesystem::path> sanitize_skill_name(std::string const& name) {
  if (name.find('\0') != std::string::npos) return std::nullopt;
  std::filesystem::path p(name);
  if (p.is_absolute()) return std::nullopt;
  std::filesystem::path norm = p.lexically_normal();
  if (norm.empty() || norm == std::filesystem::path(".") ||
      norm.string().starts_with(".."))
    return std::nullopt;
  if (norm.extension() != ".md") norm += ".md";
  return norm;
}

// 读文件内容（字节截断到上限；不切破 UTF-8 字符）
std::string read_capped(std::filesystem::path const& p, std::size_t cap_bytes) {
  std::ifstream in(p, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (s.size() <= cap_bytes) return s;
  std::size_t end = cap_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return s.substr(0, end) + "…";
}

std::string lower_of(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::size_t count_matches(std::string const& hay, std::string const& needle) {
  if (needle.empty()) return 0;
  std::size_t count = 0, pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

// 命中位置附近的片段：找第一个命中词，回看行首/最多 240 字节，取 max_bytes 上下文。
// hay 为小写正文（与 text 逐字节等长），偏移可直接用于原文。无命中 → 取头 max_bytes。
std::string match_snippet(std::string const& text, std::string const& hay,
                          std::vector<std::string> const& terms, std::size_t max_bytes) {
  std::size_t best = std::string::npos;
  for (std::string const& t : terms) {
    std::size_t const p = hay.find(t);
    if (p != std::string::npos && (best == std::string::npos || p < best)) best = p;
  }
  if (best == std::string::npos)
    return text.size() <= max_bytes ? text : text.substr(0, max_bytes) + "…";
  std::size_t start = best > 240 ? best - 240 : 0;
  std::size_t const ls = text.rfind('\n', start);
  if (ls != std::string::npos && best - ls < 2000) start = ls + 1;
  std::string out = text.substr(start, max_bytes);
  if (start > 0) out = "…" + out;
  if (start + max_bytes < text.size()) out += "…";
  return out;
}

// 可用 skill 清单（前 20 个文件名，去 .md）
std::string list_skills(std::filesystem::path const& base) {
  std::vector<std::string> names;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
    if (ec || !std::filesystem::is_regular_file(it->path(), ec)) continue;
    std::string const f = it->path().filename().string();
    if (!f.ends_with(".md")) continue;
    names.push_back(f.substr(0, f.size() - 3));
  }
  if (names.empty()) return "(空)";
  std::sort(names.begin(), names.end());
  std::string out;
  for (std::size_t i = 0; i < names.size() && i < 20; ++i)
    out += (out.empty() ? "" : ", ") + names[i];
  if (names.size() > 20) out += ", …";
  return out;
}

// 递归收集目录下全部 *.md
void collect_md(std::filesystem::path const& dir, std::vector<std::filesystem::path>& out,
                std::size_t& budget) {
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec || budget == 0) return;
    std::error_code e2;
    if (std::filesystem::is_directory(it->path(), e2)) {
      collect_md(it->path(), out, budget);
    } else if (it->path().extension() == ".md") {
      out.push_back(it->path());
      --budget;
    }
  }
}

}  // namespace

ToolSpec make_read_skill_tool() {
  return make_tool(
               "read_skill",
               "读取一份 skill 文档（工程编辑规范/规则/经验/组件库目录，如 hmi-skill、"
               "hmi-layout、gotchas、hmi-components）。文档在 WFR_SKILL_DIR（含其 wiki/ 子目录）"
               "里按名找，可带或不带 .md；不存在会返回可用清单。撰写/修改 hmi.xml 前先读"
               "对应 skill/组件目录。只读、无副作用。",
               SchemaBuilder().prop("skill", "string",
                                    "skill 名（如 hmi-skill、hmi-components）")
                   .build(),
               [](Json const& args, ToolContext& ctx)
                   -> std::expected<Json, ToolError> {
                 (void)ctx;
                 auto bases = skill_bases();
                 if (bases.empty())
                   return std::unexpected(ToolError{
                       "未配置 skill 目录（宿主未设 WFR_SKILL_DIR）"});
                 auto name = req_string(args, "skill");
                 if (!name) return std::unexpected(name.error());
                 auto rel = sanitize_skill_name(*name);
                 if (!rel)
                   return std::unexpected(ToolError{"skill 名非法：「" + *name +
                                                    "」（只允许相对路径、限 .md）"});
                 for (auto const& base : bases) {
                   std::error_code ec;
                   auto const target = base / *rel;
                   auto const canon = std::filesystem::weakly_canonical(target, ec);
                   auto const canon_base = std::filesystem::weakly_canonical(base, ec);
                   if (!ec && within(canon_base, canon) &&
                       std::filesystem::is_regular_file(canon)) {
                     std::string const content = read_capped(canon, 64u * 1024);
                     return Json{{"skill", canon.filename().string()},
                                 {"content", content},
                                 {"truncated", content.size() >= 64u * 1024}};
                   }
                 }
                 std::string avail;
                 for (auto const& b : bases) {
                   std::string const n = list_skills(b);
                   if (!n.empty() && n != "(空)") avail += (avail.empty() ? "" : "；") + n;
                 }
                 return std::unexpected(ToolError{
                     "skill 不存在：「" + *name + "」。可用：" + (avail.empty() ? "(空)" : avail)});
               })
      .with_category("retrieval");
}

ToolSpec make_wiki_search_tool() {
  return make_tool(
               "wiki_search",
               "在 LLM Wiki（沉淀好的 HMI 编辑方法/规则/经验文档库）里按关键词检索，"
               "返回最相关的文章标题与片段。查找 规则/做法/经验/样例 时用（如组件写法、"
               "debug 手法、报错码）。先检索再回答，避免凭记忆编造。只读、无副作用。",
               SchemaBuilder()
                   .prop("query", "string", "检索关键词（中文/英文均可），描述你要找的主题")
                   .prop("max_results", "integer", "返回文章数（1–5，默认 3）", false)
                   .build(),
               [](Json const& args, ToolContext& ctx) -> std::expected<Json, ToolError> {
                 (void)ctx;
                 auto query = req_string(args, "query");
                 if (!query) return std::unexpected(query.error());
                 long max_results = 3;
                 if (auto it = args.find("max_results"); it != args.end() && !it->is_null()) {
                   if (!it->is_number_integer() || it->get<long>() < 1 || it->get<long>() > 5)
                     return std::unexpected(
                         ToolError{"参数 'max_results' invalid：需为 1–5 的整数"});
                   max_results = it->get<long>();
                 }
                 std::optional<std::filesystem::path> wiki = env_dir("WFR_WIKI_DIR");
                 if (!wiki) {
                   auto sk = env_dir("WFR_SKILL_DIR");
                   if (sk) {
                     std::error_code ec;
                     auto sub = *sk / "wiki";
                     if (std::filesystem::is_directory(sub, ec)) wiki = sub;
                   }
                 }
                 if (!wiki)
                   return std::unexpected(ToolError{
                       "未配置 wiki 目录（WFR_WIKI_DIR 或 WFR_SKILL_DIR/wiki）"});
                 // 分词：按空白/标点切，小写
                 std::vector<std::string> terms;
                 {
                   std::string cur;
                   for (char c : lower_of(*query)) {
                     if (c == ' ' || c == '\t' || c == ',' || c == '，' || c == '、' ||
                         c == '。' || c == ';' || c == '；' || c == ':' || c == '：') {
                       if (!cur.empty()) terms.push_back(cur), cur.clear();
                     } else if (std::isalnum(static_cast<unsigned char>(c)) ||
                                static_cast<unsigned char>(c) >= 0x80)  // 保留中文
                       cur += c;
                   }
                   if (!cur.empty()) terms.push_back(cur);
                 }
                 std::vector<std::filesystem::path> files;
                 std::size_t budget = 400;
                 collect_md(*wiki, files, budget);
                 struct Hit {
                   std::filesystem::path path;
                   std::string title;
                   std::string text;  // 原文
                   std::string lc;    // 小写全文（与 text 逐字节等长，做索引/定位）
                   int score = 0;
                 };
                 std::vector<Hit> hits;
                 for (auto const& f : files) {
                   std::string const raw = read_capped(f, 48u * 1024);
                   std::string const lc = lower_of(raw);
                   int score = 0;
                   for (std::string const& t : terms)
                     score += static_cast<int>(count_matches(lc, t));
                   if (score == 0 && terms.empty()) score = 1;
                   if (score > 0) hits.push_back({f, f.stem().string(), raw, lc, score});
                 }
                 std::stable_sort(hits.begin(), hits.end(),
                                  [](Hit const& a, Hit const& b) { return a.score > b.score; });
                 // 空查询（没词也没文件）→ 给姿势提示
                 if (hits.empty())
                   return Json{{"articles", Json::array()},
                               {"hint", "没有命中；可换关键词或在 wiki 目录建文章"}};
                 Json out = Json::array();
                 std::size_t const n = std::min<std::size_t>(
                     static_cast<std::size_t>(max_results), hits.size());
                 for (std::size_t i = 0; i < n; ++i) {
                   std::string const snippet =
                       match_snippet(hits[i].text, hits[i].lc, terms, 700);
                   out.push_back(Json{{"file", hits[i].title + ".md"},
                                      {"score", hits[i].score},
                                      {"snippet", snippet}});
                 }
                 return Json{{"articles", std::move(out)},
                             {"total_hits", static_cast<long>(hits.size())}};
               })
      .with_category("retrieval");
}

}  // namespace cppchain::tool