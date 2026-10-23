// cppchain —— curl_multi_transport.cpp
// 单 IO 线程事件循环：
//   1) 把新提交的请求加入 multi（send() 里 curl_multi_wakeup 唤醒循环）
//   2) 扫描并摘除已取消的请求（不回调）
//   3) curl_multi_perform 驱动
//   4) 读 CURLMSG_DONE → on_complete / on_error
//   5) curl_multi_poll 等待事件
#include "cppchain/http/curl_multi_transport.hpp"

#include <curl/curl.h>

#include <atomic>
#include <charconv>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cppchain::http {

namespace {

// 全局初始化一次（curl_global_init 非线程安全）
void ensure_global_init() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Request {
  CURL* easy = nullptr;
  curl_slist* headers = nullptr;
  HttpCallbacks cbs;
  core::CancelToken token;
  std::string body;   // 拥有请求体（curl 只持有指针）
  std::string url;
  long status = 0;
  bool status_sent = false;
  std::vector<std::string> resp_headers;  // 当前响应块的头行（不含状态行），空行处上浮
};

void release_request(Request& r) {
  if (r.headers) curl_slist_free_all(r.headers);
  if (r.easy) curl_easy_cleanup(r.easy);
  r.headers = nullptr;
  r.easy = nullptr;
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* req = static_cast<Request*>(userdata);
  if (req->token.cancelled()) return 0;  // 中止传输
  std::size_t n = size * nmemb;
  if (req->cbs.on_body_chunk) req->cbs.on_body_chunk(std::string_view(ptr, n));
  return n;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* req = static_cast<Request*>(userdata);
  std::size_t n = size * nitems;
  std::string_view line(buffer, n);
  // 去掉行尾 CRLF，入列前统一成方便比对的行文本
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
  if (line.rfind("HTTP/", 0) == 0) {
    // 新响应块：上一条块已由空行 flush；复位（重定向/分段时防止块内容混串）
    req->resp_headers.clear();
    if (!req->status_sent) {
      auto sp = line.find(' ');
      if (sp != std::string_view::npos) {
        long code = 0;
        auto first = line.data() + sp + 1;
        auto last = line.data() + line.size();
        if (std::from_chars(first, last, code).ec == std::errc{}) {
          if (code >= 200) {  // 跳过 1xx 中间响应
            req->status = code;
            req->status_sent = true;
            if (req->cbs.on_status) req->cbs.on_status(code);
          }
        }
      }
    }
  } else if (line.empty()) {
    // 空行 = 当前响应块头结束 → 整组上浮
    if (!req->resp_headers.empty() && req->cbs.on_response_headers)
      req->cbs.on_response_headers(req->resp_headers);
    req->resp_headers.clear();
  } else if (req->status_sent) {
    req->resp_headers.emplace_back(std::string(line));
  }
  return n;
}

int xferinfo_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto* req = static_cast<Request*>(userdata);
  return req->token.cancelled() ? 1 : 0;  // 非 0 → 中止
}

}  // namespace

struct CurlMultiTransport::Impl {
  CURLM* multi = nullptr;
  std::thread io;
  std::atomic<bool> running{true};

  std::mutex mutex;
  std::vector<std::unique_ptr<Request>> incoming;
  std::unordered_map<CURL*, std::unique_ptr<Request>> active;

  Impl() {
    ensure_global_init();
    multi = curl_multi_init();
    io = std::thread([this] { loop(); });
  }

  ~Impl() {
    running.store(false);
    if (multi) curl_multi_wakeup(multi);
    if (io.joinable()) io.join();
    // 尚未被事件循环取走的请求（send 之后、取出之前就析构了）：同样要释放，
    // 否则它们的 easy/slist 句柄会泄漏。
    for (auto& req : incoming) release_request(*req);
    incoming.clear();
    for (auto& [handle, req] : active) {
      curl_multi_remove_handle(multi, handle);
      release_request(*req);
    }
    active.clear();
    if (multi) curl_multi_cleanup(multi);
  }

  // easy 句柄由调用方（send）创建并传入：初始化失败时 cbs 还握在 send 手里，能按契约报错。
  Request* make_request(HttpRequest const& spec, CURL* easy, HttpCallbacks cbs,
                        core::CancelToken token) {
    auto req = std::make_unique<Request>();
    req->cbs = std::move(cbs);
    req->token = std::move(token);
    req->url = spec.url;
    req->body = spec.body;
    req->easy = easy;

    for (auto const& h : spec.headers) req->headers = curl_slist_append(req->headers, h.c_str());

    curl_easy_setopt(req->easy, CURLOPT_URL, req->url.c_str());
    // 方法：默认 POST（模型/embedding 用）；GET 用于读类接口（如 Open-Meteo 查询）
    if (spec.method == "GET") {
      curl_easy_setopt(req->easy, CURLOPT_HTTPGET, 1L);
    } else {
      curl_easy_setopt(req->easy, CURLOPT_POST, 1L);
      curl_easy_setopt(req->easy, CURLOPT_COPYPOSTFIELDS, req->body.c_str());
    }
    curl_easy_setopt(req->easy, CURLOPT_HTTPHEADER, req->headers);
    curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, req.get());
    curl_easy_setopt(req->easy, CURLOPT_HEADERFUNCTION, &header_cb);
    curl_easy_setopt(req->easy, CURLOPT_HEADERDATA, req.get());
    curl_easy_setopt(req->easy, CURLOPT_XFERINFOFUNCTION, &xferinfo_cb);
    curl_easy_setopt(req->easy, CURLOPT_XFERINFODATA, req.get());
    curl_easy_setopt(req->easy, CURLOPT_NOPROGRESS, 0L);  // 启用 xferinfo 回调
    curl_easy_setopt(req->easy, CURLOPT_NOSIGNAL, 1L);    // 多线程环境必须
    curl_easy_setopt(req->easy, CURLOPT_TCP_NODELAY, 1L);  // 流式低延迟
    curl_easy_setopt(req->easy, CURLOPT_TIMEOUT_MS, spec.timeout_ms);
    // 企业代理/自签环境：显式指定 CA bundle（留空则用系统默认 CA）
    if (!spec.ca_info.empty()) curl_easy_setopt(req->easy, CURLOPT_CAINFO, spec.ca_info.c_str());
    return req.release();
  }

  void loop() {
    while (running.load()) {
      // 1) 新请求入 multi
      std::vector<std::unique_ptr<Request>> take;
      {
        std::lock_guard g(mutex);
        take.swap(incoming);
      }
      for (auto& holder : take) {
        Request* r = holder.get();
        curl_multi_add_handle(multi, r->easy);
        active[r->easy] = std::move(holder);
      }

      // 2) 摘除已取消的请求（契约：不再回调）
      for (auto it = active.begin(); it != active.end();) {
        if (it->second->token.cancelled()) {
          curl_multi_remove_handle(multi, it->first);
          release_request(*it->second);
          it = active.erase(it);
        } else {
          ++it;
        }
      }

      // 3) 驱动
      int still_running = 0;
      curl_multi_perform(multi, &still_running);

      // 4) 完成消息
      int queued = 0;
      while (CURLMsg* msg = curl_multi_info_read(multi, &queued)) {
        if (msg->msg != CURLMSG_DONE) continue;
        CURL* handle = msg->easy_handle;
        auto it = active.find(handle);
        if (it == active.end()) continue;
        Request* r = it->second.get();
        CURLcode rc = msg->data.result;

        if (!r->token.cancelled()) {
          if (rc != CURLE_OK) {
            bool timeout = (rc == CURLE_OPERATION_TIMEDOUT);
            if (r->cbs.on_error) r->cbs.on_error(HttpError{curl_easy_strerror(rc), timeout});
          } else if (r->cbs.on_complete) {
            r->cbs.on_complete();
          }
        }
        curl_multi_remove_handle(multi, handle);
        release_request(*r);
        active.erase(it);
      }

      // 5) 等待事件（send() 会 wakeup 立即唤醒）
      if (running.load()) curl_multi_poll(multi, nullptr, 0, 100, nullptr);
    }
  }
};

CurlMultiTransport::CurlMultiTransport() : impl_(std::make_unique<Impl>()) {}

CurlMultiTransport::~CurlMultiTransport() = default;

void CurlMultiTransport::send(HttpRequest const& req, HttpCallbacks cbs, core::CancelToken token) {
  // easy 句柄先建：失败时 cbs 尚在本函数手里，可按契约回调 on_error。
  // 若在此静默 return，上层（OpenAIChatModel::invoke 等待"恰好一次"回调）会永久阻塞。
  CURL* easy = curl_easy_init();
  if (easy == nullptr) {
    if (cbs.on_error) cbs.on_error(HttpError{"curl_easy_init failed", false});
    return;
  }
  Request* raw = impl_->make_request(req, easy, std::move(cbs), std::move(token));
  {
    std::lock_guard g(impl_->mutex);
    impl_->incoming.emplace_back(raw);
  }
  curl_multi_wakeup(impl_->multi);
}

}  // namespace cppchain::http