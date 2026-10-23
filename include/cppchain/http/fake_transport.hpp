// cppchain —— http/fake_transport.hpp
// 脚本化仿真传输：喂预置的字节流，用于离线确定性测试 invoke/stream/取消/错误路径。
// 与真实传输同接口，因此"先在仿真上跑通、装好 libcurl 只换实现"（02-arch-design/02 §7）。
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cppchain/http/stream.hpp"
#include "cppchain/http/transport.hpp"

namespace cppchain::http {

class FakeTransport final : public Transport, public StreamProvider {
 public:
  struct Script {
    long status = 200;
    std::vector<std::string> chunks;  // 依次投递的响应字节
    std::vector<std::string> headers; // 响应头（形如 "mcp-session-id: abc"；on_response_headers 上浮）
    std::string error;                // 非空 → 走 on_error
    bool timeout = false;             // error 是否应归类为超时
    int delay_ms = 0;                 // >0 → 后台线程分片延迟投递（用于测试取消/异步）
  };

  void push(Script s) { scripts_.push_back(std::move(s)); }

  [[nodiscard]] std::vector<HttpRequest> const& requests() const { return requests_; }

  void send(HttpRequest const& req, HttpCallbacks cbs, core::CancelToken token) override {
    requests_.push_back(req);
    Script s = (next_ < scripts_.size()) ? scripts_[next_++] : Script{};
    if (s.delay_ms <= 0) {
      deliver(s, cbs, token);
      return;
    }
    workers_.emplace_back([s = std::move(s), cbs = std::move(cbs), token = std::move(token)] {
      deliver(s, cbs, token);
    });
  }

  // ---- StreamProvider：长连接流（同步投递 —— 测试需要确定序；真实路径本就异步）----
  void open_stream(StreamRequest const& req, StreamCallbacks cbs, core::CancelToken token) override {
    stream_reqs_.push_back(req);
    stream_cbs_ = std::move(cbs);
    stream_token_ = std::move(token);
    stream_open_ = true;
  }

  // 往当前打开的流投一块正文（同步交付；流未开/已关则忽略，不作妖）。
  void stream_push(std::string chunk) {
    if (!stream_open_ || stream_token_.cancelled()) return;
    if (stream_cbs_.on_body_chunk) stream_cbs_.on_body_chunk(chunk);
  }
  // 服务端关流：on_complete → on_close（契约见 stream.hpp）。
  void stream_close() {
    if (!stream_open_ || stream_token_.cancelled()) return;
    stream_open_ = false;
    if (stream_cbs_.on_complete) stream_cbs_.on_complete();
    if (stream_cbs_.on_close) stream_cbs_.on_close();
  }

  [[nodiscard]] std::vector<StreamRequest> const& stream_requests() const { return stream_reqs_; }
  [[nodiscard]] bool stream_open() const { return stream_open_; }

 private:
  static void deliver(Script const& s, HttpCallbacks const& cbs, core::CancelToken const& token) {
    auto const alive = [&token] { return !token.cancelled(); };
    auto const pause = [&s] {
      if (s.delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(s.delay_ms));
    };

    pause();
    if (!alive()) return;
    if (!s.error.empty()) {
      if (cbs.on_error) cbs.on_error(HttpError{s.error, s.timeout});
      return;
    }
    if (cbs.on_status) cbs.on_status(s.status);
    if (cbs.on_response_headers && !s.headers.empty()) cbs.on_response_headers(s.headers);

    for (auto const& c : s.chunks) {
      if (!alive()) return;
      if (cbs.on_body_chunk) cbs.on_body_chunk(c);
      pause();
    }
    if (!alive()) return;
    if (cbs.on_complete) cbs.on_complete();
  }

  std::vector<Script> scripts_;
  std::size_t next_ = 0;
  std::vector<HttpRequest> requests_;
  std::vector<std::jthread> workers_;  // 析构自动 join

  // 流状态（单测单线程使用：open → push/close 都在测试线程，无需加锁）
  std::vector<StreamRequest> stream_reqs_;
  StreamCallbacks stream_cbs_;
  core::CancelToken stream_token_;
  bool stream_open_ = false;
};

}  // namespace cppchain::http