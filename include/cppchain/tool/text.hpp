// cppchain —— tool/text.hpp
// 文本清洗小工具（header-only）：HTML/XML 实体解码、UTF-8 安全截断、空白压平。
// 由 fetch_url（HTML→正文）与 arxiv（Atom XML→论文摘要）等文本类工具共享。
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace cppchain::tool {

// 大小写不敏感子串查找
inline std::size_t ci_find(std::string_view s, std::string_view needle, std::size_t from = 0) {
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

// 去首尾空白
inline std::string_view trim_ascii(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

// 压平空白：行内空白→单空格；连续换行→单换行；去行首尾空白（保留段落结构）
inline std::string collapse_whitespace(std::string_view s) {
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

// 全部空白（含换行）→ 单个空格，去首尾（标题/摘要这类单段文本用）
inline std::string normalize_space(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool pending = false;
  for (unsigned char c : s) {
    if (std::isspace(c)) {
      pending = true;
      continue;
    }
    if (pending && !out.empty()) out.push_back(' ');
    pending = false;
    out.push_back(static_cast<char>(c));
  }
  return out;
}

// UTF-8 安全截断（不切断多字节字符，尾部加省略号）
inline std::string utf8_truncate(std::string_view s, std::size_t max_chars) {
  if (s.size() <= max_chars) return std::string(s);
  std::size_t end = max_chars;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;  // 回退到字符边界
  return std::string(s.substr(0, end)) + "…";
}

namespace detail {
// Unicode 码点 → UTF-8 字节（解码数字实体用）
inline std::string encode_utf8(long cp) {
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
}  // namespace detail

// 解码实体：&amp; &lt; &gt; 等命名实体与数字实体（&#NNN; / &#xHH;）。
// 未知实体原样保留（& 脱出）。HTML 与 Atom XML 通用。
struct NamedEntity {
  std::string_view name;
  std::string_view value;
};
inline NamedEntity const kEntities[] = {
    {"amp", "&"},         {"lt", "<"},       {"gt", ">"},      {"quot", "\""},
    {"apos", "'"},        {"nbsp", " "}, {"mdash", "—"}, {"ndash", "–"},
    {"hellip", "…"}, {"lsquo", "‘"}, {"rsquo", "’"}, {"ldquo", "“"},
    {"rdquo", "”"},  {"middot", "·"}, {"copy", "©"}, {"reg", "®"},
    {"deg", "°"},    {"plusmn", "±"}, {"times", "×"}, {"divide", "÷"},
    {"cent", "¢"},   {"pound", "£"}, {"euro", "€"}, {"yen", "¥"},
    {"micro", "µ"},  {"para", "¶"}, {"infin", "∞"}, {"ne", "≠"},
    {"le", "≤"},     {"ge", "≥"}, {"pi", "π"}, {"alpha", "α"},
    {"beta", "β"},   {"gamma", "γ"}, {"delta", "δ"}, {"sum", "∑"},
    {"radic", "√"}};

inline std::string decode_entities(std::string_view s) {
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
        out += detail::encode_utf8(cp);
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

}  // namespace cppchain::tool