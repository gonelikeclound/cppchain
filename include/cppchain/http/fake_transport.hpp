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

#include "cppchain/http/transport.hpp"

namespace cppchain::http {

class FakeTransport final : public Transport {
 public:
  struct Script {
    long status = 200;
    std::vector<std::string> chunks;  // 依次投递的响应字节
    std::string error;                // 非空 → 走 on_error
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

 private:
  static void deliver(Script const& s, HttpCallbacks const& cbs, core::CancelToken const& token) {
    auto const alive = [&token] { return !token.cancelled(); };
    auto const pause = [&s] {
      if (s.delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(s.delay_ms));
    };

    pause();
    if (!alive()) return;
    if (!s.error.empty()) {
      if (cbs.on_error) cbs.on_error(s.error);
      return;
    }
    if (cbs.on_status) cbs.on_status(s.status);

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
};

}  // namespace cppchain::http