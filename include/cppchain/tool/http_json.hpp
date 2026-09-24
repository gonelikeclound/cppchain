// cppchain —— tool/http_json.hpp
// 通用"一次同步 HTTP + JSON"助手：把异步 Transport 桥接成同步调用，
// 供 get_weather / web_search 这类真网络工具复用。
//
// 分层约束：工具层只依赖 http::Transport **接口**（header-only），不链接 libcurl；
// 真传输（CurlMultiTransport）由使用方注入，测试时可注入 FakeTransport 离线跑。
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "cppchain/core/cancel.hpp"
#include "cppchain/http/transport.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

// 一次请求的完整响应。任何 HTTP 状态码都算"到达"（含 4xx/5xx、响应体非法 JSON），
// 只有**传输层失败**（网络断开 / 超时 / 取消）才走 unexpected。
struct JsonResponse {
  long status = 0;     // HTTP 状态码（0 = 未收到响应头）
  nlohmann::json body; // 解析后的 JSON（解析失败或空响应 → null）
  std::string raw;     // 原始响应体（截断保存，仅用于错误诊断）
};

// 发起请求并阻塞到完成（信号量桥接，模式同 DashScopeEmbeddings）。
//   - 网络错误 / 超时 → unexpected(ToolError{…})；
//   - 收到响应 → JsonResponse{status, body, raw}，由调用方按 status 判断成败。
// 若 token 中途取消，尽快返回 unexpected（不悬挂）。
std::expected<JsonResponse, ToolError> http_json(http::Transport& transport,
                                                 http::HttpRequest const& req,
                                                 core::CancelToken const& cancel);

// HTTP 状态码 → ToolError。2xx 返回 nullopt；非 2xx 给出含原始响应片段的提示。
std::optional<ToolError> http_status_error(long status, std::string_view what,
                                           std::string_view raw_hint = {});

// URL 查询串组件的百分号编码（城市名等中文/空格必须转义；值之外的结构由调用方拼）。
std::string url_percent_encode(std::string_view s);

}  // namespace cppchain::tool