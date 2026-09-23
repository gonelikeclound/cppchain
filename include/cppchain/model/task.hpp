// cppchain —— model/task.hpp
// 自研最小可等待任务（C++20 协程）。理由见 02-arch-design/02 §3：
// std::execution(P2300) 在 GCC 16 上尚未实现，async 需自己搭。
//
// 契约：
//  - 惰性启动：首次 co_await / wait() 才开始跑。
//  - Task 析构销毁协程帧；**不得在挂起期间销毁**（调用方保证先等到完成或取消）。
//  - 仅支持非 void 返回类型。
#pragma once

#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <semaphore>
#include <utility>

namespace cppchain::model {

template <typename T>
class Task {
 public:
  struct promise_type {
    std::optional<T> value;
    std::exception_ptr error;
    std::coroutine_handle<> continuation{};
    // 用可计数信号量（而非 binary_semaphore）：release 可能先于任何 acquire 发生。
    std::counting_semaphore<> done{0};

    Task get_return_object() noexcept {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
      bool await_ready() const noexcept { return false; }
      std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        h.promise().done.release();
        return h.promise().continuation ? h.promise().continuation : std::noop_coroutine();
      }
      void await_resume() const noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }

    template <typename U>
    void return_value(U&& v) {
      value.emplace(std::forward<U>(v));
    }
    void unhandled_exception() noexcept { error = std::current_exception(); }
  };

  Task() noexcept = default;
  explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}
  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  Task(Task const&) = delete;
  Task& operator=(Task const&) = delete;
  ~Task() { reset(); }

  // ---- co_await 支持 ----
  bool await_ready() const noexcept { return !handle_ || handle_.done(); }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> cont) {
    handle_.promise().continuation = cont;
    return handle_;  // 启动被等待的任务
  }
  T await_resume() {
    if (handle_.promise().error) std::rethrow_exception(handle_.promise().error);
    return std::move(*handle_.promise().value);
  }

  // ---- 同步驱动（门面与测试用）----
  T wait() {
    if (!handle_) throw std::logic_error("Task: empty task awaited");
    if (!handle_.done()) handle_.resume();
    handle_.promise().done.acquire();
    if (handle_.promise().error) std::rethrow_exception(handle_.promise().error);
    return std::move(*handle_.promise().value);
  }

 private:
  void reset() noexcept {
    if (handle_) {
      handle_.destroy();
      handle_ = {};
    }
  }
  std::coroutine_handle<promise_type> handle_{};
};

// 回调 →协程 桥接：把"以回调完成"的异步操作变成可 co_await 的表达式。
// 契约：start 必须且只能调用一次 resume。
template <typename T>
struct CallbackAwaiter {
  std::function<void(std::function<void(T)>)> start;
  std::optional<T> result = std::nullopt;  // NSDMI：避免 aggregate-init 漏字段告警

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    start([this, h](T v) {
      result.emplace(std::move(v));
      h.resume();  // 由完成回调所在线程恢复
    });
  }
  T await_resume() { return std::move(*result); }
};

}  // namespace cppchain::model