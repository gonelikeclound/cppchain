// cppchain —— openai_chat_model.cpp
// 回调内核（stream_async）为唯一真实路径；invoke / stream / invoke_async 都是它的封装：
//   invoke       = stream 累积 → finalize （同步阻塞）
//   stream       = 生成器 + 队列，把异步回调转成同步迭代
//   invoke_async = CallbackAwaiter 把回调内核桥接成 co_await
#include "cppchain/model/openai/openai_chat_model.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <utility>
#include <vector>

#include "cppchain/message/chunk.hpp"
#include "cppchain/model/openai/sse.hpp"
#include "cppchain/model/openai/wire.hpp"

namespace cppchain::model::openai {

namespace {

std::string join_url(std::string base, std::string_view path) {
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + std::string(path);
}

ModelError::Kind kind_for_status(long status) {
  if (status == 401 || status == 403) return ModelError::Kind::Auth;
  if (status == 429) return ModelError::Kind::RateLimit;
  if (status >= 500) return ModelError::Kind::Server;
  return ModelError::Kind::Unknown;
}

// 流式请求的共享状态（回调可能跨线程触发）
struct StreamState {
  SseParser sse;
  MessageChunk acc;
  long status = 0;
  std::string finish_reason;
  std::string raw_body;          // 供错误诊断（截断保存）
  std::optional<ModelError> error;
  bool finished = false;         // 保证 on_done/on_error 恰好触发一次
};

constexpr std::size_t kRawBodyLimit = 8192;

void finish_with_error(StreamState& st, StreamHandlers const& h, ModelError err) {
  if (st.finished) return;
  st.finished = true;
  if (h.on_error) h.on_error(std::move(err));
}

}  // namespace

OpenAIChatModel::OpenAIChatModel(ModelConfig cfg, std::shared_ptr<http::Transport> transport)
    : cfg_(std::move(cfg)), transport_(std::move(transport)) {}

http::HttpRequest OpenAIChatModel::build_http_request(std::span<Message const> msgs,
                                                     CallOptions const& opts, bool stream) const {
  http::HttpRequest req;
  req.url = join_url(cfg_.base_url, "/chat/completions");
  req.body = openai::build_request(msgs, opts, cfg_, stream).dump();
  req.timeout_ms = cfg_.timeout_ms;
  req.headers.reserve(cfg_.extra_headers.size() + 2);
  req.headers.push_back("Content-Type: application/json");
  req.headers.push_back("Authorization: Bearer " + cfg_.api_key);
  for (auto const& h : cfg_.extra_headers) req.headers.push_back(h);
  return req;
}

core::CancelToken OpenAIChatModel::stream_async(std::span<Message const> msgs,
                                                CallOptions const& opts,
                                                StreamHandlers handlers) {
  // 异步安全：输入消息复制进本次请求的生命周期
  auto owned = std::make_shared<std::vector<Message>>(msgs.begin(), msgs.end());
  http::HttpRequest req = build_http_request(*owned, opts, /*stream=*/true);

  auto st = std::make_shared<StreamState>();
  auto h = std::make_shared<StreamHandlers>(std::move(handlers));
  core::CancelToken token;  // 拷贝共享 stop-state，可交给调用方取消

  http::HttpCallbacks cbs;
  cbs.on_status = [st](long status) { st->status = status; };

  cbs.on_body_chunk = [st, h, token](std::string_view bytes) {
    if (token.cancelled()) return;
    if (st->raw_body.size() < kRawBodyLimit) {
      auto take = std::min(kRawBodyLimit - st->raw_body.size(), bytes.size());
      st->raw_body.append(bytes.substr(0, take));
    }
    auto parsed = st->sse.feed(bytes);
    for (auto& ev : parsed.data_events) {
      auto j = nlohmann::json::parse(ev, nullptr, false);
      if (j.is_discarded()) {
        finish_with_error(*st, *h, ModelError{ModelError::Kind::Parse, 0, "SSE data not JSON", {}});
        token.cancel();  // 已报错，停止继续接收
        return;
      }
      auto chunk = openai::parse_chunk(j, &st->finish_reason);
      if (!chunk) {
        finish_with_error(*st, *h, chunk.error());
        token.cancel();
        return;
      }
      if (!chunk->content.empty() || !chunk->tool_call_chunks.empty() || chunk->usage) {
        st->acc = st->acc + *chunk;
        if (h->on_delta) h->on_delta(*chunk);
      }
    }
  };

  cbs.on_error = [st, h, token](std::string message) {
    if (token.cancelled()) return;
    nlohmann::json raw = nlohmann::json{{"body", st->raw_body}};
    finish_with_error(*st, *h,
                      ModelError{ModelError::Kind::Network, 0, std::move(message), std::move(raw)});
  };

  cbs.on_complete = [st, h, token]() {
    if (token.cancelled() || st->finished) return;
    if (st->status < 200 || st->status >= 300) {
      nlohmann::json raw = nlohmann::json{{"body", st->raw_body}};
      finish_with_error(*st, *h,
                        ModelError{kind_for_status(st->status), static_cast<int>(st->status),
                                   "HTTP " + std::to_string(st->status), std::move(raw)});
      return;
    }
    Message m = message::finalize_chunk(st->acc);
    if (!st->finish_reason.empty()) m.response_metadata["finish_reason"] = st->finish_reason;
    if (st->acc.usage) m.usage = st->acc.usage;
    st->finished = true;
    if (h->on_done) h->on_done(std::move(m));
  };

  transport_->send(req, std::move(cbs), token);
  return token;
}

std::generator<MessageChunk> OpenAIChatModel::stream(std::span<Message const> msgs,
                                                     CallOptions const& opts) {
  struct Queue {
    std::mutex m;
    std::deque<MessageChunk> items;
    // 可计数信号量：同步投递时多个 release 会先于消费发生（binary_semaphore 会溢出断言）
    std::counting_semaphore<> ready{0};
    bool done = false;
    std::optional<ModelError> err;
  };
  auto q = std::make_shared<Queue>();

  StreamHandlers handlers;
  handlers.on_delta = [q](MessageChunk const& c) {
    {
      std::lock_guard g(q->m);
      q->items.push_back(c);
    }
    q->ready.release();
  };
  handlers.on_done = [q](Message) {
    {
      std::lock_guard g(q->m);
      q->done = true;
    }
    q->ready.release();
  };
  handlers.on_error = [q](ModelError e) {
    {
      std::lock_guard g(q->m);
      q->err = std::move(e);
      q->done = true;
    }
    q->ready.release();
  };

  // 注意：msgs 仅在调用期间有效，stream_async 内部会复制
  stream_async(msgs, opts, std::move(handlers));

  while (true) {
    q->ready.acquire();
    std::unique_lock g(q->m);
    if (q->items.empty()) {
      if (q->err) {
        ModelError e = *q->err;
        g.unlock();
        throw ModelException(std::move(e));
      }
      co_return;
    }
    MessageChunk c = std::move(q->items.front());
    q->items.pop_front();
    g.unlock();
    co_yield c;
  }
}

std::expected<Message, ModelError> OpenAIChatModel::invoke(std::span<Message const> msgs,
                                                           CallOptions const& opts) {
  // 直接建在回调内核之上：拿 on_done 给出的**权威终态消息**
  // （含 finish_reason / usage 等只存在于终态的信息；不能靠重新 finalize 累积块得到）。
  struct Sync {
    std::optional<Message> message;
    std::optional<ModelError> error;
    std::counting_semaphore<> ready{0};
  };
  auto sync = std::make_shared<Sync>();

  StreamHandlers handlers;
  handlers.on_done = [sync](Message m) {
    sync->message = std::move(m);
    sync->ready.release();
  };
  handlers.on_error = [sync](ModelError e) {
    sync->error = std::move(e);
    sync->ready.release();
  };

  stream_async(msgs, opts, std::move(handlers));
  sync->ready.acquire();

  if (sync->error) return std::unexpected(std::move(*sync->error));
  if (!sync->message) return std::unexpected(ModelError{ModelError::Kind::Unknown, 0,
                                                        "stream finished without result", {}});
  return std::move(*sync->message);
}

Task<std::expected<Message, ModelError>> OpenAIChatModel::invoke_async(std::span<Message const> msgs,
                                                                       CallOptions opts) {
  std::vector<Message> owned(msgs.begin(), msgs.end());
  auto result = co_await CallbackAwaiter<std::expected<Message, ModelError>>{
      [this, owned = std::move(owned), opts = std::move(opts)](
          std::function<void(std::expected<Message, ModelError>)> resume) {
        StreamHandlers handlers;
        handlers.on_done = [resume](Message m) {
          resume(std::expected<Message, ModelError>{std::move(m)});
        };
        handlers.on_error = [resume](ModelError e) {
          resume(std::expected<Message, ModelError>{std::unexpected(std::move(e))});
        };
        stream_async(owned, opts, std::move(handlers));
      }};
  co_return result;
}

}  // namespace cppchain::model::openai