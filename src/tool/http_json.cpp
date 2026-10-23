// cppchain —— src/tool/http_json.cpp
#include "cppchain/tool/http_json.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <semaphore>
#include <thread>

namespace cppchain::tool {

namespace {
// 响应体上限（防御性，防止异常巨响应吃内存；正常响应远小于此）。
// 必须远大于工具返回体（如 feedcoop web_search ~16KB）—— 此前硬编码 4KB 会把
// 大响应截断成非法 JSON，解析成"空结果"（只有小响应体 + 401 路径测不出来）。
constexpr std::size_t kBodyLimit = 16 * 1024 * 1024;
}  // namespace

std::expected<JsonResponse, ToolError> http_json(http::Transport& transport,
                                                 http::HttpRequest const& req,
                                                 core::CancelToken const& cancel) {
  // 共享完成状态（回调可能在 IO 线程触发，见 http/transport.hpp 线程契约）
  struct Sync {
    long status = 0;
    std::string body;
    std::vector<std::string> headers;
    std::optional<ToolError> err;
    std::counting_semaphore<> done{0};
  };
  auto st = std::make_shared<Sync>();

  http::HttpCallbacks cbs;
  cbs.on_status = [st](long s) { st->status = s; };
  cbs.on_response_headers = [st](std::vector<std::string> const& hs) { st->headers = hs; };
  cbs.on_body_chunk = [st](std::string_view b) {
    if (st->body.size() < kBodyLimit) {
      auto n = std::min(kBodyLimit - st->body.size(), b.size());
      st->body.append(b.substr(0, n));
    }
  };
  cbs.on_error = [st](http::HttpError e) {
    st->err = ToolError{
        std::string(e.timeout ? "请求超时：" : "网络错误：") + e.message,
    };
    st->done.release();
  };
  cbs.on_complete = [st] { st->done.release(); };

  transport.send(req, std::move(cbs), cancel);

  // 等待完成；顺带轮询取消，避免"取消后传输层不再回调"导致悬挂
  //（传输层契约：取消后不得再触发任何回调）。
  while (!st->done.try_acquire()) {
    if (cancel.cancelled()) return std::unexpected(ToolError{"已取消"});
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (st->err) return std::unexpected(*st->err);

  JsonResponse out;
  out.status = st->status;
  out.raw = st->body;
  out.headers = std::move(st->headers);
  out.body = nlohmann::json::parse(st->body, nullptr, /*allow_exceptions=*/false);
  return out;
}

std::optional<ToolError> http_status_error(long status, std::string_view what,
                                           std::string_view raw_hint) {
  if (status >= 200 && status < 300) return std::nullopt;
  std::string msg = std::string(what) + " 返回 HTTP " + std::to_string(status);
  if (!raw_hint.empty()) {
    std::string hint(raw_hint);
    if (hint.size() > 160) hint.resize(160);
    msg += "：" + hint;
  }
  return ToolError{std::move(msg)};
}

std::string url_percent_encode(std::string_view s) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

}  // namespace cppchain::tool