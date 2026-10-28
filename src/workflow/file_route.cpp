// cppchain —— src/workflow/file_route.cpp
// problems/02 追加 S6：文件取数路由的实现（规则判定 + 升级阶梯 + 预算 + 证据）。
#include "cppchain/workflow/file_route.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/message/message.hpp"

namespace cppchain::workflow {
namespace {

using Json = nlohmann::json;

bool has_any(std::string_view q, std::initializer_list<std::string_view> words) {
  for (std::string_view w : words)
    if (q.find(w) != std::string_view::npos) return true;
  return false;
}

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

std::string clip_utf8(std::string_view s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return std::string(s);
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return std::string(s.substr(0, end));
}

// grep 的 pattern 是正则：字面/选择器统一转义，避免 "." 之类的元字符误命中
std::string regex_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (std::string_view(".^$|()[]{}*+?\\").find(c) != std::string_view::npos) out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

struct FileEntry {
  std::string name;
  long long size = 0;
};

std::vector<FileEntry> list_files(tool::ToolRegistry const& reg) {
  auto const* list = reg.find("list_dir");
  if (list == nullptr) return {};
  tool::ToolContext ctx;
  auto r = list->handler(Json{{"path", "."}, {"max_entries", 500}}, ctx);
  if (!r || !r->contains("entries") || !r->at("entries").is_array()) return {};
  std::vector<FileEntry> out;
  for (Json const& e : r->at("entries"))
    if (e.value("type", "") == "file")
      out.push_back(FileEntry{e.value("name", ""), e.value("size", 0LL)});
  return out;
}

std::string ext_of(std::string_view name) {
  auto const dot = name.rfind('.');
  if (dot == std::string_view::npos) return {};
  return lower_ascii(name.substr(dot));
}

bool is_structured_ext(std::string_view ext) {
  return ext == ".json" || ext == ".yaml" || ext == ".yml" || ext == ".xml" ||
         ext == ".md" || ext == ".markdown" || ext == ".txt";
}

std::vector<std::string> mentioned_files(std::string const& q,
                                         std::vector<FileEntry> const& files) {
  std::vector<std::string> out;
  for (auto const& f : files)
    if (!f.name.empty() && q.find(f.name) != std::string::npos) out.push_back(f.name);
  return out;
}

bool is_ascii_token_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '[' ||
         c == ']' || c == '/' || c == '-' || c == ':';
}

// JSON Pointer / XML 元素路径：/a/b、/root/item[2]/name
std::string detect_pointer(std::string_view q) {
  for (std::size_t i = 0; i < q.size(); ++i) {
    if (q[i] != '/') continue;
    if (i > 0 && (std::isalnum(static_cast<unsigned char>(q[i - 1])) || q[i - 1] == '_'))
      continue;  // URL 里的 "/" 不算（前面紧邻字母）
    std::size_t j = i;
    while (j < q.size() && (is_ascii_token_char(q[j]) || q[j] == ']')) ++j;
    std::string_view sub = q.substr(i, j - i);
    if (sub.size() >= 3 && sub.find('/', 1) != std::string_view::npos) return std::string(sub);
  }
  return {};
}

// 点键路径：server.ports.1 / a.b[0]（≥2 个点，排除文件名）
std::string detect_dot_path(std::string_view q, std::vector<FileEntry> const& files) {
  std::size_t i = 0;
  while (i < q.size()) {
    while (i < q.size() && !is_ascii_token_char(q[i])) ++i;
    std::size_t const start = i;
    while (i < q.size() && is_ascii_token_char(q[i])) ++i;
    if (i == start) break;
    std::string_view tok = q.substr(start, i - start);
    if (tok.find('.') == std::string_view::npos) continue;
    std::size_t dots = 0;
    for (char c : tok)
      if (c == '.') ++dots;
    if (dots < 2) continue;
    if (is_structured_ext(ext_of(tok))) continue;  // 文件名不是路径
    bool same_as_file = false;
    for (auto const& f : files)
      if (f.name == tok) same_as_file = true;
    if (!same_as_file && tok.find_first_of("0123456789") != std::string_view::npos)
      return std::string(tok);
  }
  return {};
}

// 中文章节 / Section N
std::string detect_chapter(std::string_view q) {
  static constexpr std::string_view kWord = "\xE7\xAC\xAC";  // 第
  for (std::size_t i = 0; i + kWord.size() <= q.size(); ++i) {
    if (q.compare(i, kWord.size(), kWord) != 0) continue;
    std::size_t pos = i + kWord.size(), chars = 0;
    while (pos < q.size() && chars < 8) {
      unsigned char const c = static_cast<unsigned char>(q[pos]);
      std::size_t const len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      for (std::string_view const m : {"\xE7\xAB\xA0", "\xE5\x9B\x9E", "\xE8\x8A\x82",
                                       "\xE5\x8D\xB7", "\xE9\x83\xA8"})  // 章回节卷部
        if (pos + m.size() <= q.size() && q.compare(pos, m.size(), m) == 0)
          return std::string(q.substr(i, pos + m.size() - i));
      pos += len;
      ++chars;
    }
  }
  auto const s = lower_ascii(q).find("section");
  if (s != std::string::npos) {
    std::size_t j = s + 7;
    while (j < q.size() && std::isspace(static_cast<unsigned char>(q[j]))) ++j;
    std::size_t const start = j;
    while (j < q.size() && std::isdigit(static_cast<unsigned char>(q[j]))) ++j;
    if (j > start) return std::string(q.substr(s, j - s));
  }
  return {};
}

// 字面：引号内容优先；否则取"像代码/标识符"的 ASCII token
std::string detect_literal(std::string_view q, std::vector<FileEntry> const& files) {
  if (auto a = q.find('"'); a != std::string::npos) {
    auto b = q.find('"', a + 1);
    if (b != std::string::npos && b > a + 1) return std::string(q.substr(a + 1, b - a - 1));
  }
  if (auto a = q.find('\''); a != std::string::npos) {
    auto b = q.find('\'', a + 1);
    if (b != std::string::npos && b > a + 1) return std::string(q.substr(a + 1, b - a - 1));
  }
  // 中文引号「…」
  static constexpr std::string_view kOpen = "\xE3\x80\x8C";   // 「
  static constexpr std::string_view kClose = "\xE3\x80\x8D";  // 」
  if (auto a = q.find(kOpen); a != std::string::npos) {
    auto b = q.find(kClose, a + kOpen.size());
    if (b != std::string::npos && b > a + kOpen.size())
      return std::string(q.substr(a + kOpen.size(), b - a - kOpen.size()));
  }
  // ASCII 代码 token（0x9F3A / code=... / v2.3 / ERROR_CODE）
  std::size_t i = 0;
  while (i < q.size()) {
    while (i < q.size() && !is_ascii_token_char(q[i]) && q[i] != '=' && q[i] != '%')
      ++i;
    std::size_t const start = i;
    while (i < q.size() && (is_ascii_token_char(q[i]) || q[i] == '=' || q[i] == '%')) ++i;
    if (i == start) break;
    std::string_view tok = q.substr(start, i - start);
    if (tok.size() < 4) continue;
    bool const has_digit = tok.find_first_of("0123456789") != std::string_view::npos;
    bool const has_eq = tok.find('=') != std::string_view::npos;
    bool const hex = tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X');
    if (!(has_digit || has_eq || hex)) continue;
    if (is_structured_ext(ext_of(tok))) continue;  // 文件名不是字面
    bool same_as_file = false;
    for (auto const& f : files)
      if (f.name == tok) same_as_file = true;
    if (same_as_file) continue;
    if (tok.find('/') != std::string_view::npos) continue;  // 指针已由 detect_pointer 处理
    return std::string(tok);
  }
  return {};
}

// ---- ⑤ BM25 / ⑥ 向量：检索臂（grep/extract 未命中时的兜底）----

bool is_cjk(unsigned cp) { return cp >= 0x4E00 && cp <= 0x9FFF; }

// ASCII 词 + CJK 单字 + CJK 相邻二字（bigram）——中文无空格，bigram 提供词面重叠。
std::vector<std::string> retrieval_tokens(std::string_view s) {
  std::vector<std::string> out;
  std::string ascii, prev_cjk;
  auto flush = [&] {
    if (!ascii.empty()) {
      out.push_back(ascii);
      ascii.clear();
    }
  };
  for (std::size_t i = 0; i < s.size();) {
    unsigned char const c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      if (std::isalnum(c)) {
        ascii.push_back(static_cast<char>(std::tolower(c)));
        prev_cjk.clear();
        ++i;
        continue;
      }
      flush();
      prev_cjk.clear();
      ++i;
      continue;
    }
    std::size_t const len = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (i + len > s.size()) break;
    unsigned cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
    for (std::size_t k = 1; k < len; ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    if (is_cjk(cp)) {
      flush();
      std::string const ch(s.substr(i, len));
      out.push_back(ch);
      if (!prev_cjk.empty()) out.push_back(prev_cjk + ch);
      prev_cjk = ch;
    } else {
      flush();
      prev_cjk.clear();
    }
    i += len;
  }
  flush();
  return out;
}

// 用 file_read 分页把文件读进内存（受 max_bytes 限制；UTF-8 安全截断）。
std::string read_file_paged(tool::ToolRegistry const& reg, std::string const& path,
                            long max_bytes) {
  auto const* rd = reg.find("file_read");
  if (rd == nullptr) return {};
  std::string out;
  long long offset = 0;
  for (int guard = 0; guard < 64 && static_cast<long>(out.size()) < max_bytes; ++guard) {
    tool::ToolContext ctx;
    auto r = rd->handler(Json{{"path", path}, {"offset", offset}, {"max_chars", 8000}}, ctx);
    if (!r) break;
    std::string c = r->value("content", "");
    if (c.size() >= 3 && c.compare(c.size() - 3, 3, "…") == 0) c.resize(c.size() - 3);
    out += c;
    if (!r->contains("next_offset") || r->at("next_offset").is_null()) break;
    offset = r->at("next_offset").get<long long>();
  }
  return clip_utf8(out, static_cast<std::size_t>(max_bytes));
}

struct RetrievalChunk {
  std::string path;
  std::size_t offset = 0;
  std::string text;
  std::vector<std::string> toks;
  double bm25 = 0.0;
  double vec = 0.0;
  double score = 0.0;
};

std::string join_names(Json const& arr) {
  std::string out;
  for (Json const& n : arr) out += (out.empty() ? "" : "、") + n.get<std::string>();
  return out;
}

}  // namespace

FileRouteOutcome route_file_question(tool::ToolRegistry const& reg, std::string const& question,
                                     FileRouteOptions opts) {
  FileRouteOutcome out;
  auto const files = list_files(reg);
  for (auto const& f : files) out.candidates.push_back(f.name);
  if (opts.max_files > 0 && out.candidates.size() > static_cast<std::size_t>(opts.max_files))
    out.candidates.erase(out.candidates.begin() + opts.max_files, out.candidates.end());

  auto const mentioned = mentioned_files(question, files);
  std::string selector = detect_pointer(question);
  if (selector.empty()) selector = detect_dot_path(question, files);
  if (selector.empty()) selector = detect_chapter(question);
  std::string const literal = detect_literal(question, files);
  bool const global =
      has_any(question, {"总结", "概括", "讲了什么", "主要内容", "整体", "风格", "结局",
                         "大概", "主题", "讲的是什么", "最后", "结尾"});
  bool const complete =
      has_any(question, {"全部", "一共", "有没有", "是否提到", "每一条", "逐一", "所有",
                         "核对", "列出来", "列举"});

  long remaining = std::max<long>(200, opts.budget_chars);
  auto add_text = [&](std::string text) {
    if (remaining <= 0) {
      out.budget_hit = true;
      return;
    }
    if (static_cast<long>(text.size()) > remaining) {
      text = clip_utf8(text, static_cast<std::size_t>(remaining)) + "…[路由预算已满]";
      out.budget_hit = true;
      remaining = 0;
    } else {
      remaining -= static_cast<long>(text.size());
    }
    out.context.push_back(message::system_message(std::move(text)));
  };
  auto record = [&](std::string tool, std::string detail, bool hit, std::size_t bytes) {
    out.attempts.push_back(Json{{"tool", std::move(tool)},
                                {"detail", std::move(detail)},
                                {"hit", hit},
                                {"bytes", bytes}});
  };

  auto do_grep = [&](std::string const& pattern) -> bool {
    auto const* g = reg.find("file_grep");
    if (g == nullptr) return false;
    tool::ToolContext ctx;
    auto r = g->handler(
        Json{{"path", "."}, {"pattern", regex_escape(pattern)}, {"max_matches", 20}}, ctx);
    if (!r) {
      record("file_grep", pattern, false, 0);
      return false;
    }
    std::string text;
    for (Json const& m : r->at("matches"))
      text += m.value("path", "") + ":" + std::to_string(m.value("line", 0LL)) + ": " +
              m.value("text", "") + "\n";
    bool const hit = !text.empty();
    record("file_grep", pattern, hit, text.size());
    if (hit) {
      out.route = "grep";
      add_text("【grep pattern=" + pattern + "】\n" + text);
    }
    return hit;
  };

  auto do_extract = [&](std::string const& path, std::string const& sel) -> bool {
    auto const* e = reg.find("file_extract");
    if (e == nullptr) return false;
    tool::ToolContext ctx;
    auto r = e->handler(Json{{"path", path}, {"selector", sel}}, ctx);
    if (!r) {
      record("file_extract", path + " · " + sel, false, 0);
      return false;
    }
    bool const found = r->value("found", false);
    std::string const text = r->value("text", "");
    record("file_extract", path + " · " + sel, found, text.size());
    if (!found) return false;
    out.route = "extract";
    // 显式标「提取结果」：真机暴露过模型看到裸值（如 "443"）反而怀疑"素材没给值"
    add_text("【" + path + " · selector=" + sel + " · format=" + r->value("format", "") +
             "】\n提取结果：\n" + text);
    return true;
  };

  auto do_sample = [&](std::string const& path) -> bool {
    auto const* s = reg.find("file_sample");
    if (s == nullptr || path.empty()) return false;
    tool::ToolContext ctx;
    auto r = s->handler(Json{{"path", path}, {"chars_per_window", 600}}, ctx);
    if (!r) {
      record("file_sample", path, false, 0);
      return false;
    }
    std::string text;
    for (Json const& w : r->at("windows"))
      text += "[" + w.value("where", "") + "@" + std::to_string(w.value("offset", 0LL)) +
              "]\n" + w.value("text", "") + "\n";
    bool const hit = !text.empty();
    record("file_sample", path, hit, text.size());
    if (hit) {
      out.route = "sample";
      add_text("【" + path + " · 采样 head/middle/tail（不保证覆盖全文）】\n" + text);
    }
    return hit;
  };

  // ⑤/⑥：字面/structure 未命中时的检索升级（BM25；配置 embedder 时混合向量）。
  auto do_retrieve = [&]() -> bool {
    std::vector<std::string> targets = mentioned;
    if (targets.empty())
      for (auto const& f : files) {
        auto const e = ext_of(f.name);
        if (is_structured_ext(e) || e == ".log" || e == ".csv") targets.push_back(f.name);
      }
    if (opts.max_files > 0 && targets.size() > static_cast<std::size_t>(opts.max_files))
      targets.resize(static_cast<std::size_t>(opts.max_files));
    if (targets.empty()) {
      record("retrieve", "无候选文件", false, 0);
      return false;
    }

    std::vector<RetrievalChunk> chunks;
    for (auto const& path : targets) {
      std::string const text = read_file_paged(reg, path, opts.retrieve_max_bytes);
      if (text.empty()) continue;
      std::size_t const win = 800, step = 600;
      for (std::size_t pos = 0; pos < text.size(); pos += step) {
        std::size_t end = std::min(pos + win, text.size());
        while (end > pos && end < text.size() &&
               (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
          --end;
        std::size_t start = pos;
        while (start < text.size() &&
               (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
          ++start;
        if (end <= start) continue;
        RetrievalChunk ch;
        ch.path = path;
        ch.offset = start;
        ch.text = text.substr(start, end - start);
        ch.toks = retrieval_tokens(ch.text);
        chunks.push_back(std::move(ch));
        if (end == text.size()) break;
      }
    }
    auto const qt = retrieval_tokens(question);
    if (chunks.empty() || qt.empty()) {
      record("retrieve", "无候选片段/查询词", false, 0);
      return false;
    }

    // BM25（k1=1.2, b=0.75）；df 只统计 query 词
    std::unordered_map<std::string, std::size_t> df;
    for (auto const& t : qt) df[t] = 0;
    std::size_t total_toks = 0;
    for (auto const& ch : chunks) {
      std::unordered_map<std::string, std::size_t> tf;
      for (auto const& t : ch.toks) ++tf[t];
      for (auto& [t, c] : df)
        if (tf.count(t) > 0) ++c;
      total_toks += ch.toks.size();
    }
    double const avgdl = static_cast<double>(total_toks) / static_cast<double>(chunks.size());
    constexpr double k1 = 1.2, b = 0.75;
    double max_bm25 = 0.0;
    for (auto& ch : chunks) {
      std::unordered_map<std::string, std::size_t> tf;
      for (auto const& t : ch.toks) ++tf[t];
      double s = 0.0;
      for (auto const& [t, d] : df) {
        auto const it = tf.find(t);
        if (it == tf.end()) continue;
        double const idf =
            std::log(1.0 + (static_cast<double>(chunks.size()) - d + 0.5) / (d + 0.5));
        double const dl = static_cast<double>(ch.toks.size());
        s += idf * (it->second * (k1 + 1.0)) /
             (it->second + k1 * (1.0 - b + b * dl / (avgdl > 0 ? avgdl : 1.0)));
      }
      ch.bm25 = s;
      max_bm25 = std::max(max_bm25, s);
    }

    // 可选向量臂：query 与 chunk 余弦，与归一化 BM25 混合
    bool vec_ok = false;
    if (opts.embedder != nullptr) {
      auto const qe = opts.embedder->embed_query(question);
      if (qe) {
        std::vector<std::string> texts;
        texts.reserve(chunks.size());
        for (auto const& c : chunks)
          texts.push_back(c.text.substr(0, std::min<std::size_t>(c.text.size(), 800)));
        auto const em = opts.embedder->embed(texts);
        if (em && em->size() == chunks.size()) {
          for (std::size_t i = 0; i < chunks.size(); ++i)
            chunks[i].vec =
                std::max(0.0, model::cosine_similarity(*qe, (*em)[i]));
          vec_ok = true;
        }
      }
    }
    for (auto& ch : chunks) {
      double const kb = max_bm25 > 0 ? ch.bm25 / max_bm25 : 0.0;
      ch.score = vec_ok ? opts.vector_weight * ch.vec + (1.0 - opts.vector_weight) * kb : kb;
    }
    std::sort(chunks.begin(), chunks.end(),
              [](RetrievalChunk const& a, RetrievalChunk const& b) { return a.score > b.score; });

    std::string text;
    std::size_t bytes = 0;
    bool any = false;
    std::size_t const take = std::min(opts.retrieve_top_k, chunks.size());
    for (std::size_t i = 0; i < take; ++i) {
      if (chunks[i].score <= 1e-9) break;
      any = true;
      text += "【" + chunks[i].path + " · offset=" + std::to_string(chunks[i].offset) + " · " +
              (vec_ok ? "bm25+vector" : "bm25") + "】\n" + chunks[i].text + "\n";
      bytes += chunks[i].text.size();
    }
    record("retrieve", vec_ok ? "bm25+vector" : "bm25", any, bytes);
    if (!any) return false;
    out.route = "retrieve";
    add_text("【检索结果（字面/structure 未命中时升级）】\n" + text);
    return true;
  };

  auto sample_target = [&]() -> std::string {
    if (!mentioned.empty()) return mentioned.front();
    std::string best;
    long long best_size = -1;
    for (auto const& f : files)
      if (f.size > best_size) {
        best_size = f.size;
        best = f.name;
      }
    return best;
  };

  if (!selector.empty()) {
    // 结构信号：优先在候选文件上试 extract；全未命中 → grep（选择器原文）→ sample
    std::vector<std::string> targets = mentioned;
    if (targets.empty())
      for (auto const& f : files)
        if (is_structured_ext(ext_of(f.name))) targets.push_back(f.name);
    if (targets.size() > 3) targets.resize(3);
    bool ok = false;
    for (auto const& t : targets)
      if (do_extract(t, selector)) {
        ok = true;
        break;
      }
    if (!ok) {
      bool const g = do_grep(selector);
      if (!g && !do_retrieve()) do_sample(sample_target());
    }
  } else if (!literal.empty()) {
    if (!do_grep(literal) && !do_retrieve()) do_sample(sample_target());
  } else if (global) {
    do_sample(sample_target());
  } else if (complete) {
    record("planner", "完整性问题：没有可确定性提取的字面/结构；采样不能作兜底，交模型规划",
           false, 0);
  }

  if (out.route != "none") {
    std::string head = "【文件取数路由 route=" + out.route + "】候选文件：" +
                       join_names(out.candidates) +
                       "；以下是 harness 定位到的素材（回答请引用 path:line 或 selector；"
                       "**没命中不代表不存在**）。";
    out.context.insert(out.context.begin(), message::system_message(std::move(head)));
  }
  return out;
}

}  // namespace cppchain::workflow
