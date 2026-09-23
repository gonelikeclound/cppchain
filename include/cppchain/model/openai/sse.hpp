// cppchain —— model/openai/sse.hpp
// SSE 增量解析器：把字节流切成 data 负载。**必须支持跨包截断**
// （一个事件被拆在两个 TCP 包里到达是常态）。
//
// 处理约定：
//  - 事件分隔符 \n\n（输入中的 \r\n 先归一化为 \n）
//  - 只取 data: 行（多行 data 以 \n 连接）；event:/id:/retry:/注释行忽略
//  - data: [DONE] → Result::done = true，不作为事件返回
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cppchain::model::openai {

class SseParser {
 public:
  struct Result {
    std::vector<std::string> data_events;
    bool done = false;  // 收到 [DONE]
  };

  // 喂入一段字节，返回本次能完整解析出的事件（可能为空）。
  Result feed(std::string_view bytes) {
    append_normalized(bytes);
    Result r;
    std::size_t sep = 0;
    while ((sep = buffer_.find("\n\n")) != std::string::npos) {
      std::string block = buffer_.substr(0, sep);
      buffer_.erase(0, sep + 2);
      std::string payload;
      bool has_data = false;
      for (auto const& line : split_lines(block)) {
        if (line.rfind("data:", 0) != 0) continue;  // 非 data 字段忽略
        std::string value{line.substr(5)};
        if (!value.empty() && value.front() == ' ') value.erase(0, 1);
        if (has_data) payload += '\n';
        payload += value;
        has_data = true;
      }
      if (!has_data) continue;
      if (payload == "[DONE]") {
        r.done = true;
      } else {
        r.data_events.push_back(std::move(payload));
      }
    }
    return r;
  }

  [[nodiscard]] bool has_pending() const { return !buffer_.empty(); }

 private:
  void append_normalized(std::string_view bytes) {
    for (char c : bytes) {
      if (c == '\r') continue;  // 归一化：丢弃 CR
      buffer_ += c;
    }
  }

  static std::vector<std::string_view> split_lines(std::string_view block) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= block.size()) {
      std::size_t nl = block.find('\n', start);
      if (nl == std::string_view::npos) {
        lines.push_back(block.substr(start));
        break;
      }
      lines.push_back(block.substr(start, nl - start));
      start = nl + 1;
    }
    return lines;
  }

  std::string buffer_;
};

}  // namespace cppchain::model::openai