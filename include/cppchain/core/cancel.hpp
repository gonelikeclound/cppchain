// cppchain —— core/cancel.hpp
// 取消令牌：包装 std::stop_source。拷贝共享同一 stop-state，
// 因此可把 token 交给传输层、同时在调用方持有并 cancel。
#pragma once

#include <stop_token>

namespace cppchain::core {

class CancelToken {
 public:
  CancelToken() = default;

  // 请求取消（幂等）；拷贝出的 token 共享同一状态。
  // const 语义：token 是"句柄"，取消改的是共享 stop-state，而非这个对象本身。
  void cancel() const { src_.request_stop(); }

  [[nodiscard]] bool cancelled() const { return src_.stop_requested(); }
  [[nodiscard]] std::stop_token stop_token() const { return src_.get_token(); }

 private:
  mutable std::stop_source src_{};
};

}  // namespace cppchain::core