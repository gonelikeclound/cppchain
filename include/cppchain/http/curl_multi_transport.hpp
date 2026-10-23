// cppchain —— http/curl_multi_transport.hpp
// 基于 libcurl multi 的真实传输：单 IO 线程 + 事件循环，非阻塞、socket 级驱动。
// 一个线程可并发驱动多路请求 —— 这是 M3 并行工具调用、M8 多 subagent 并行的底座。
//
// 取消语义：token 取消后立刻把该 easy handle 从 multi 摘除并清理，**不再触发任何回调**。
#pragma once

#include <memory>

#include "cppchain/http/stream.hpp"
#include "cppchain/http/transport.hpp"

namespace cppchain::http {

class CurlMultiTransport final : public Transport, public StreamProvider {
 public:
  CurlMultiTransport();
  ~CurlMultiTransport() override;

  CurlMultiTransport(CurlMultiTransport const&) = delete;
  CurlMultiTransport& operator=(CurlMultiTransport const&) = delete;

  // 线程安全：可从任意线程调用；回调在 IO 线程触发。
  void send(HttpRequest const& req, HttpCallbacks cbs, core::CancelToken token) override;

  // 长连接流（StreamProvider）：把流 handle 挂进现有单 IO 线程的事件循环 —— 0 新增线程。
  void open_stream(StreamRequest const& req, StreamCallbacks cbs,
                   core::CancelToken token) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cppchain::http