// cppchain —— http/stream.hpp
// 长连接流能力（可选接口）：Transport 实现方**自愿**附带提供（CurlMultiTransport / 测试的
// FakeTransport 都有；纯接口消费方拿不到也照样过——`dynamic_cast` 失败 = 不支持流）。
//
// 用途：MCP 的 notifications 推送（SSE 长连接，P3 批②）等需要"打开一条流、持续投递直到
// 取消/对端关流/出错"的场景。与 Transport::send（一发一收即忘）的本质差别：**流的生命期以
// 显式结束为界**，期间任何时刻都有 `on_body_chunk` 到达。
//
// 线程契约（与 transport.hpp 相同）：回调可能在传输层 IO 线程触发；token 一旦取消，
// **不得再触发任何回调**（含 on_close）。回调返回后即视为该次投递结束。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cppchain/http/transport.hpp"

namespace cppchain::http {

// 打开一条长连接流的请求描述。
struct StreamRequest {
  std::string url;
  std::string method = "GET";
  std::vector<std::string> headers;  // 形如 "Accept: text/event-stream"、"mcp-session-id: x"
};

// 流级回调（生命周期见头注释的线程契约）：
//   正常：  on_status →（任意次 on_body_chunk）→ on_complete → on_close
//   传输错：on_status →（…）→ on_error → on_close
//   取消：  无任何回调。
// on_close 一律收尾 —— 对端关流/出错都要靠它把"流已死"交给持有者。
struct StreamCallbacks {
  std::function<void(long status)> on_status;            // 状态行（200 等）
  std::function<void(std::string_view)> on_body_chunk;   // 增量字节（到达即交付）
  std::function<void(HttpError)> on_error;               // 传输层错误；随后必有 on_close
  std::function<void()> on_complete;                      // 对端关流（正常）；随后必有 on_close
  std::function<void()> on_close;                         // 一律收尾（错误/正常关）
};

// 可选的"长连接流"能力。使用方通过 dynamic_cast 探测：
//   auto* sp = dynamic_cast<http::StreamProvider*>(&transport);
// 拿不到即"该传输不支持长连接"（MCP 监听需在调用侧降级/报错，不影响单发调用）。
class StreamProvider {
 public:
  virtual ~StreamProvider() = default;

  // 打开一条长连接流，立即返回；投递按 StreamCallbacks 的生命周期进行。
  // req/token 由实现方在流存活期间持有；token 取消后立刻停止投递（无回调）。
  virtual void open_stream(StreamRequest const& req, StreamCallbacks cbs,
                           core::CancelToken token) = 0;
};

}  // namespace cppchain::http