// cppchain —— model/openai/openai_chat_model.hpp
// OpenAI 兼容 ChatModel 实现。传输通过 http::Transport 注入（仿真/curl_multi 可换）。
#pragma once

#include <memory>
#include <span>

#include "cppchain/http/transport.hpp"
#include "cppchain/model/chat_model.hpp"

namespace cppchain::model::openai {

class OpenAIChatModel final : public ChatModel {
 public:
  OpenAIChatModel(ModelConfig cfg, std::shared_ptr<http::Transport> transport);

  std::expected<Message, ModelError> invoke(std::span<Message const> msgs,
                                           CallOptions const& opts) override;

  std::generator<MessageChunk> stream(std::span<Message const> msgs,
                                      CallOptions const& opts) override;

  core::CancelToken stream_async(std::span<Message const> msgs, CallOptions const& opts,
                                 StreamHandlers handlers) override;

  Task<std::expected<Message, ModelError>> invoke_async(std::span<Message const> msgs,
                                                        CallOptions opts) override;

  [[nodiscard]] ModelConfig const& config() const { return cfg_; }

 private:
  http::HttpRequest build_http_request(std::span<Message const> msgs, CallOptions const& opts,
                                       bool stream) const;

  ModelConfig cfg_;
  std::shared_ptr<http::Transport> transport_;
};

}  // namespace cppchain::model::openai