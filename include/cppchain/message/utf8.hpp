// cppchain —— message/utf8.hpp
// 工具/外部数据的 UTF-8 容错：nlohmann 存得下非法字节、dump 时才抛 type_error.316。
// 对外部文本/JSON 进会话层之前做净化，保证日志/调试台/摘要可序列化。
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace cppchain::message {

// 把非法 UTF-8 字节替换为 U+FFFD（�）；合法多字节原样保留。
inline void sanitize_utf8(std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  auto replacement = [&out]() { out += "\xEF\xBF\xBD"; };
  while (i < s.size()) {
    unsigned char const c = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    if (c < 0x80) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (len == 0 || i + len > s.size()) {  // 非法头字节或跨接越界 → 替换
      replacement();
      ++i;
      continue;
    }
    bool ok = true;
    for (std::size_t k = 1; k < len; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { ok = false; break; }
    if (!ok) { replacement(); ++i; continue; }
    out.append(s, i, len);
    i += len;
  }
  s = std::move(out);
}

// 递归复制，把所有字符串叶净化（供 dump 前用）。
inline nlohmann::json utf8_sanitized_copy(nlohmann::json const& j) {
  if (j.is_string()) {
    std::string t = j.get<std::string>();
    sanitize_utf8(t);
    return nlohmann::json(std::move(t));
  }
  if (j.is_array()) {
    nlohmann::json out = nlohmann::json::array();
    for (auto const& e : j) out.push_back(utf8_sanitized_copy(e));
    return out;
  }
  if (j.is_object()) {
    nlohmann::json out = nlohmann::json::object();
    for (auto it = j.begin(); it != j.end(); ++it) out[it.key()] = utf8_sanitized_copy(it.value());
    return out;
  }
  return j;  // 数字/布尔/null 无字节问题
}

// 安全 dump：非法 UTF-8 不会抛，替换为 U+FFFD。
inline std::string utf8_safe_dump(nlohmann::json const& j) {
  return utf8_sanitized_copy(j).dump();
}

}  // namespace cppchain::message