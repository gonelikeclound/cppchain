#include <expected>
#include <generator>
#include <print>
#include <ranges>
#include <vector>

// 冒烟测试：验证“gcc-16 + -std=c++26 + 新的标准库件”整条链路可用。
// C++20 coroutine 实现的 std::generator —— LLM 流式响应会用它。

std::generator<int> co_squares(int n) {
  for (int i = 0; i < n; ++i)
    co_yield i * i;
}

int main() {
  // std::expected —— 工具/模型层错误传递的主力
  std::expected<int, std::string> e = 42;
  if (e) {
    std::println("cppchain smoke OK | __cplusplus={} | expected value={}", __cplusplus, *e);
  }

  // ranges + generator —— 流式管道
  std::vector<int> v{};
  for (int x : co_squares(6)) {
    v.push_back(x);
  }
  std::println("generator sequence: {}", v);

  return 0;
}