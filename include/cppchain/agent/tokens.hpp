// cppchain —— agent/tokens.hpp
// 无 tokenizer 的启发式 token 估算（M9，供压缩触发与截断预算共同使用）。
// 口径：多字节字符（CJK 等）一字≈1 token；ASCII ≈4 字符/token；角色消息另加恒定开销。
// 保证：estimate(text) 对前缀长度**非降** → 截断的二分回退合法（见 06-m9收官详细设计 §3.1）。
// 不是精确 tokenizer：真 token 以端点的 usage 元数据为权威（口径统一，只做内部比较）。
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "cppchain/message/message.hpp"

namespace cppchain::agent::tokens {

// 单段文本的估算 token 数。
[[nodiscard]] std::size_t estimate(std::string_view text);

// 消息序列 = 每条 role 开销 + 各 ContentBlock 文本之和。
[[nodiscard]] std::size_t estimate_msgs(std::span<message::Message const> msgs);

}  // namespace cppchain::agent::tokens