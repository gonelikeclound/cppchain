// cppchain —— src/model/dashscope_embeddings.cpp
// 同步门面（信号量桥接异步传输）；批量自动分批；按 text_index 重排。
#include "cppchain/model/dashscope/embeddings.hpp"

#include <algorithm>
#include <optional>
#include <semaphore>
#include <string>
#include <utility>
#include <vector>

namespace cppchain::model::dashscope {

namespace {

constexpr char kPath[] = "/api/v1/services/embeddings/text-embedding/text-embedding";
constexpr std::size_t kServerMaxBatch = 20;  // 服务端硬上限（实测）

std::string join_url(std::string base, std::string_view path) {
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + std::string(path);
}

// 服务端错误码 + HTTP 状态 → 统一 ModelError
ModelError make_error(long status, nlohmann::json const& body, std::string fallback) {
  std::string code = body.is_object() ? body.value("code", "") : "";
  std::string message = body.is_object() ? body.value("message", "") : "";
  if (message.empty()) message = std::move(fallback);

  ModelError::Kind kind = ModelError::Kind::Unknown;
  if (code == "InvalidApiKey" || status == 401 || status == 403)
    kind = ModelError::Kind::Auth;
  else if (status == 429)
    kind = ModelError::Kind::RateLimit;
  else if (status >= 500)
    kind = ModelError::Kind::Server;
  else if (status >= 400)
    kind = ModelError::Kind::Unknown;  // 参数类错误
  return ModelError{kind, static_cast<int>(status), std::move(message), body};
}

struct SyncState {
  std::string body;
  long status = 0;
  std::optional<ModelError> error;
  std::counting_semaphore<> done{0};
};

}  // namespace

DashScopeEmbeddings::DashScopeEmbeddings(EmbeddingConfig cfg,
                                         std::shared_ptr<http::Transport> transport)
    : cfg_(std::move(cfg)), transport_(std::move(transport)) {}

std::expected<std::vector<Embedding>, ModelError> DashScopeEmbeddings::embed_batch(
    std::span<std::string const> texts) {
  nlohmann::json input = nlohmann::json::object();
  auto arr = nlohmann::json::array();
  for (auto const& t : texts) arr.push_back(t);
  input["texts"] = std::move(arr);

  nlohmann::json body = {{"model", cfg_.model}, {"input", std::move(input)}};

  http::HttpRequest req;
  req.url = join_url(cfg_.base_url, kPath);
  req.body = body.dump();
  req.timeout_ms = cfg_.timeout_ms;
  req.ca_info = cfg_.ca_info;
  req.headers = {"Content-Type: application/json",
                 "Authorization: Bearer " + cfg_.api_key};

  auto st = std::make_shared<SyncState>();
  core::CancelToken token;

  http::HttpCallbacks cbs;
  cbs.on_status = [st](long status) { st->status = status; };
  cbs.on_body_chunk = [st](std::string_view bytes) { st->body.append(bytes); };
  cbs.on_error = [st](http::HttpError err) {
    st->error = ModelError{err.timeout ? ModelError::Kind::Timeout : ModelError::Kind::Network, 0,
                           std::move(err.message), nlohmann::json::object()};
    st->done.release();
  };
  cbs.on_complete = [st] { st->done.release(); };

  transport_->send(req, std::move(cbs), token);
  st->done.acquire();

  if (st->error) return std::unexpected(std::move(*st->error));

  auto parsed = nlohmann::json::parse(st->body, nullptr, false);
  if (parsed.is_discarded())
    return std::unexpected(ModelError{ModelError::Kind::Parse, static_cast<int>(st->status),
                                      "response is not JSON", {{"body", st->body}}});

  if (st->status < 200 || st->status >= 300)
    return std::unexpected(make_error(st->status, parsed, "HTTP " + std::to_string(st->status)));

  // 解析 output.embeddings，按 text_index 重排
  if (!parsed.contains("output") || !parsed["output"].is_object() ||
      !parsed["output"].contains("embeddings") || !parsed["output"]["embeddings"].is_array())
    return std::unexpected(ModelError{ModelError::Kind::Parse, static_cast<int>(st->status),
                                      "missing output.embeddings", parsed});

  std::vector<std::pair<int, Embedding>> indexed;
  for (auto const& item : parsed["output"]["embeddings"]) {
    int const idx = item.value("text_index", -1);
    if (idx < 0 || !item.contains("embedding")) continue;
    indexed.emplace_back(idx, item["embedding"].get<Embedding>());
  }
  if (indexed.size() != texts.size())
    return std::unexpected(ModelError{ModelError::Kind::Parse, static_cast<int>(st->status),
                                      "embedding count mismatch", parsed});

  std::sort(indexed.begin(), indexed.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });
  for (std::size_t i = 0; i < indexed.size(); ++i)
    if (indexed[i].first != static_cast<int>(i))
      return std::unexpected(ModelError{ModelError::Kind::Parse, static_cast<int>(st->status),
                                        "text_index out of order", parsed});

  if (parsed.contains("usage") && parsed["usage"].is_object())
    total_tokens_ += parsed["usage"].value("total_tokens", 0L);

  std::vector<Embedding> out;
  out.reserve(indexed.size());
  for (auto& [idx, vec] : indexed) {
    (void)idx;
    if (dimension_ == 0) dimension_ = vec.size();
    out.push_back(std::move(vec));
  }
  return out;
}

std::expected<std::vector<Embedding>, ModelError> DashScopeEmbeddings::embed(
    std::span<std::string const> texts) {
  if (texts.empty()) return std::vector<Embedding>{};

  std::size_t const batch =
      std::max<std::size_t>(1, std::min({cfg_.max_batch_size, kServerMaxBatch}));
  std::vector<Embedding> out;
  out.reserve(texts.size());

  for (std::size_t i = 0; i < texts.size(); i += batch) {
    std::size_t const end = std::min(i + batch, texts.size());
    auto part = embed_batch(texts.subspan(i, end - i));
    if (!part) return std::unexpected(part.error());
    for (auto& e : *part) out.push_back(std::move(e));
  }
  return out;
}

}  // namespace cppchain::model::dashscope