// cppchain —— model/error.hpp
// 模型层错误：统一以 std::expected<T, ModelError> 传递（错误是常态）。
#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace cppchain::model {

struct ModelError {
  enum class Kind {
    Network,    // 连接/DNS/传输层失败
    Timeout,    // 超时
    Auth,       // 401/403
    RateLimit,  // 429
    Server,     // 5xx
    Parse,      // JSON / SSE 解析失败
    Cancelled,  // 主动取消
    Unknown,
  };

  Kind kind = Kind::Unknown;
  int http_status = 0;
  std::string message;
  nlohmann::json raw = nlohmann::json::object();  // 原始错误负载（可诊断）

  bool operator==(ModelError const&) const = default;
};

inline std::string_view kind_str(ModelError::Kind k) {
  switch (k) {
    case ModelError::Kind::Network: return "network";
    case ModelError::Kind::Timeout: return "timeout";
    case ModelError::Kind::Auth: return "auth";
    case ModelError::Kind::RateLimit: return "rate_limit";
    case ModelError::Kind::Server: return "server";
    case ModelError::Kind::Parse: return "parse";
    case ModelError::Kind::Cancelled: return "cancelled";
    case ModelError::Kind::Unknown: return "unknown";
  }
  return "unknown";
}

// 供 std::generator 场景抛出（生成器无法返回 expected）
struct ModelException : std::runtime_error {
  ModelError err;
  explicit ModelException(ModelError e)
      : std::runtime_error(std::string(kind_str(e.kind)) + ": " + e.message), err(std::move(e)) {}
};

}  // namespace cppchain::model