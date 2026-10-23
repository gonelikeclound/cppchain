// cppchain —— mcp/listen.hpp
// MCP 三期批②：变化监听句柄（长连接收 notifications/tools/list_changed → on_change）。
// 句柄活着，长连接就挂着 —— 由使用方保有它即可（McpProviderResult 持有全部已开监听）；
// 析构 / stop() 即撤流（内部 token 取消）。
//
// 线程与寿命：
//   · SSE 组帧/识别发生在传输层 IO 线程；**on_change 在 McpListener 自有 worker 线程跑**
//     —— 回调里做同步 client 调用（refresh 重拉 tools/list）绝不能挡在 IO 线程上：
//     那次 http_json 会同步等 multi 完成，而 multi 的驱动正是 IO 线程 → 自我死锁（实测命中）。
//   · 寿命：监听的 client ⊆ manager 池、registry 归使用方 —— 宿主须保证 manager/registry
//     活得**比** McpProviderResult（含监听）长。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cppchain/core/cancel.hpp"
#include "cppchain/mcp/client.hpp"

namespace cppchain::mcp {

class McpListener {
 public:
  // 构造即尝试挂流：transport 支持长连接 + 已有会话才成功；否则 error() 非空、closed()==true。
  // 传 preset_error（非空）→ 直接携带该错误、**不尝试开流**（能力门跳过的观察位，见 provider）。
  McpListener(MCPClient& client, std::function<void()> on_change,
              std::string preset_error = {});
  ~McpListener();

  McpListener(McpListener const&) = delete;
  McpListener& operator=(McpListener const&) = delete;
  McpListener(McpListener&&) = delete;
  McpListener& operator=(McpListener&&) = delete;

  // 显式撤流（幂等；析构自动调；join worker）。
  void stop();

  [[nodiscard]] std::string const& server() const { return server_; }
  // 非空 = 监听未生效（transport 不支持 / 无会话 / 流已失败）。
  [[nodiscard]] std::string const& error() const { return error_; }
  // 已收到的 tools/list_changed 通知次数（观察/验收用）。
  [[nodiscard]] std::uint64_t changes() const { return changes_.load(); }
  // true = 流已关/未开（可能可重挂）；false = 活跃收推中。
  [[nodiscard]] bool closed() const { return closed_.load(); }

 private:
  // 通知到达（IO 线程）：只置位+唤醒，**不在此跑 on_change**。
  void notify_change();
  // worker 线程主循环：串行执行 on_change（回调可同步调 client，IO 线程不挡）。
  void worker_loop();

  std::thread worker_;                     // 先声明 → 次被析构（dtor 先 join 已保证安全）
  std::mutex mu_;
  std::condition_variable cv_;
  bool event_pending_ = false;
  bool worker_stop_ = false;

  MCPClient* client_;
  std::string server_;
  std::function<void()> on_change_;
  core::CancelToken token_;              // 撤流句柄
  std::atomic<std::uint64_t> changes_{0};
  std::atomic<bool> closed_{true};        // 开流成功后才置 false
  std::string error_;
};

}  // namespace cppchain::mcp