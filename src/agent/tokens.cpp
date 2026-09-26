// cppchain —— src/agent/tokens.cpp
#include "cppchain/agent/tokens.hpp"

namespace cppchain::agent::tokens {

namespace {

// UTF-8 字节扫描：ASCII(<0x80) 计入 ascii；多字节字符的首字节(>=0xC0)计一个"宽字符"，
// 连续字节(0x80..0xBF)跳过——这样"一字≈1 token"对中文成立，对西文 fallback 到 /4。
struct Counts {
  std::size_t ascii = 0;
  std::size_t wide = 0;
};

Counts count(std::string_view s) {
  Counts c;
  for (std::size_t i = 0; i < s.size();) {
    unsigned char const b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
      ++c.ascii;
      ++i;
    } else if (b < 0xC0) {
      ++i;  // 游离的连续字节：忽略（不可能是合法 UTF-8 起始）
    } else {
      ++c.wide;
      i += 1;
      while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    }
  }
  return c;
}

}  // namespace

std::size_t estimate(std::string_view text) {
  Counts const c = count(text);
  return c.wide + (c.ascii + 3u) / 4u;  // 上行取整，保证对前缀非降（+3u 而非 +4u 也无碍：非降即可）
}

std::size_t estimate_msgs(std::span<message::Message const> msgs) {
  std::size_t total = 0;
  for (auto const& m : msgs) {
    total += 4;  // role 开销近似
    // 有实测 usage → 该消息直接采用实测值（不再叠加文本启发式，避免双计）；
    // 否则按内容文本估算。
    if (m.usage) {
      total += static_cast<std::size_t>(m.usage->total_tokens);
      continue;
    }
    if (m.status != "success") total += 2;  // 差别的状态说明
    for (auto const& b : m.content) {
      if (auto const* t = std::get_if<message::TextBlock>(&b)) total += estimate(t->text);
      else if (auto const* r = std::get_if<message::ReasoningBlock>(&b)) total += estimate(r->reasoning);
    }
  }
  return total;
}

}  // namespace cppchain::agent::tokens