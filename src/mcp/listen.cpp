// cppchain —— mcp/listen.cpp
// McpListener 实现：积组装 SSE 帧 → 识别 notifications/tools/list_changed → on_change()。
#include "cppchain/mcp/listen.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace cppchain::mcp {

using Json = nlohmann::json;

namespace {

// 增量 SSE 组装：把碎片 chunk 攒成完整"事件"，每个完整事件产出一段 data 正文；
// data 跨行按 SSE 语义用 '\n' 连接，半行留缓冲等下块（真实 TCP 会分片）。
class SseAssembler {
 public:
  // 消费 chunk，返回本块里走完的完整事件 data 列表（0..n 个）。
  std::vector<std::string> push(std::string_view chunk) {
    partial_ += chunk;
    std::vector<std::string> events;

    // 抽完整行（到换行为界；尾部半行留在 partial_）。
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
      std::size_t const nl = partial_.find('\n', start);
      if (nl == std::string::npos) break;
      std::string line = partial_.substr(start, nl - start);
      start = nl + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF 归一
      lines.push_back(std::move(line));
    }
    partial_.erase(0, start);

    for (std::string const& line : lines) {
      if (line.starts_with("data:")) {
        std::string v = line.substr(5);
        if (!v.empty() && v.front() == ' ') v.erase(0, 1);
        if (!data_buf_.empty()) data_buf_ += '\n';
        data_buf_ += v;
        in_event_ = true;
      } else if (line.empty()) {
        // 事件边界：data 非空才算有意义的事件。
        if (in_event_ && !data_buf_.empty()) events.push_back(std::move(data_buf_));
        data_buf_.clear();
        in_event_ = false;
      } else {
        in_event_ = true;  // event:/id:/retry: 等非 data 行也归属当前事件
      }
    }
    return events;
  }

 private:
  std::string partial_;    // 未消费的半行（下一块继续拼）
  std::string data_buf_;   // 当前事件已积累的 data 文本
  bool in_event_ = false;
};

}  // namespace

McpListener::McpListener(MCPClient& client, std::function<void()> on_change,
                          std::string preset_error)
    : worker_(&McpListener::worker_loop, this),  // worker 先就位：首件事是等 cv，事件在构造完前到不了
      client_(&client),
      server_(client.server_name()),
      on_change_(std::move(on_change)) {
  if (!preset_error.empty()) {  // 未声明能力等栅栏：不开流，只留观察位
    error_ = std::move(preset_error);
    return;
  }
  auto sse = std::make_shared<SseAssembler>();
  http::StreamCallbacks scbs;
  scbs.on_status = [](long) {};  // 状态行用不上（200 与否已由开流成败体现）
  scbs.on_body_chunk = [this, sse](std::string_view chunk) {
    for (std::string const& ev : sse->push(chunk)) {
      Json const j = Json::parse(ev, nullptr, /*allow_exceptions=*/false);
      if (j.is_discarded() || !j.is_object()) continue;
      if (j.value("method", "") == "notifications/tools/list_changed") {
        changes_.fetch_add(1);
        notify_change();  // 只在 IO 线程置位唤醒；on_change 由 worker 线程跑
      }
    }
  };
  scbs.on_error = [this](http::HttpError e) {
    if (error_.empty()) error_ = "通知流错误：" + e.message;
    closed_.store(true);
  };
  scbs.on_complete = [] {};  // 正常关流：靠 on_close 收尾
  scbs.on_close = [this] { closed_.store(true); };

  auto r = client_->listen(std::move(scbs), token_);
  if (!r) {
    error_ = r.error();
    return;
  }
  closed_.store(false);
}

McpListener::~McpListener() { stop(); }

void McpListener::stop() {
  token_.cancel();  // 幂等；传输层随后停止回调并摘除流（无回调出于契约）
  {
    std::lock_guard g(mu_);
    worker_stop_ = true;
  }
  cv_.notify_one();
  if (worker_.joinable()) worker_.join();
}

void McpListener::notify_change() {
  {
    std::lock_guard g(mu_);
    event_pending_ = true;
  }
  cv_.notify_one();
}

void McpListener::worker_loop() {
  while (true) {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [this] { return event_pending_ || worker_stop_; });
    if (worker_stop_) break;
    event_pending_ = false;
    lk.unlock();
    // 在 worker 线程执行用户回调 —— refresh 的同步 client 调用不再挡 IO 线程。
    if (on_change_) on_change_();
  }
}

}  // namespace cppchain::mcp