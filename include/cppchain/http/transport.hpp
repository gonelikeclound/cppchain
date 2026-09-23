// cppchain —— http/transport.hpp
// 传输抽象：屏蔽"字节怎么上网线"。仿真(FakeTransport)与真实(curl_multi)同接口，
// 上层（OpenAI 适配）零改动即可切换（见 02-arch-design/02 §7）。
//
// 线程契约：
//  - 回调可能在 IO 线程触发，**实现方不得假设回调在调用线程执行**。
//  - token 一旦取消，不得再触发任何回调。
//  - 回调返回后即视为该次投递结束；request/callbacks 的生命周期由实现方在 send 期间持有。
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/core/cancel.hpp"

namespace cppchain::http {

struct HttpRequest {
  std::string url;
  std::string body;                     // 已序列化的请求体（JSON）
  std::vector<std::string> headers;     // 形如 "Content-Type: application/json"
  long timeout_ms = 60000;
};

struct HttpCallbacks {
  std::function<void(long status)> on_status;              // 收到响应头（HTTP 状态码）
  std::function<void(std::string_view)> on_body_chunk;     // 增量响应字节
  std::function<void(std::string)> on_error;               // 传输层错误（网络/超时/取消）
  std::function<void()> on_complete;                       // 响应结束（含非 2xx）
};

class Transport {
 public:
  virtual ~Transport() = default;

  // 发起请求。token 由调用方持有并可取消；实现须在取消后停止一切回调。
  virtual void send(HttpRequest const& req, HttpCallbacks cbs, core::CancelToken token) = 0;
};

}  // namespace cppchain::http