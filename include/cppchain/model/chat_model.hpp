// cppchain —— model/chat_model.hpp
// ChatModel 抽象 + 流式回调 + 四个能力。
// 参照 langchain: invoke / stream / bind_tools(wire 通道) —— 见 02-arch-design/02 §2。
//
// 异步契约：异步接口在发起前**必须复制输入消息**（本层实现保证），
// 调用方无需保证 span 在异步完成前存活。
#pragma once

#include <expected>
#include <functional>
#include <generator>
#include <memory>
#include <span>
#include <string>

#include "cppchain/core/cancel.hpp"
#include "cppchain/message/chunk.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/model/error.hpp"
#include "cppchain/model/options.hpp"
#include "cppchain/model/task.hpp"

namespace cppchain::model {

using cppchain::message::Message;
using cppchain::message::MessageChunk;

struct StreamHandlers {
  std::function<void(MessageChunk const&)> on_delta;  // 每个增量块
  std::function<void(Message)> on_done;               // 已 finalize 的完整消息
  std::function<void(ModelError)> on_error;           // 失败（取消则不触发）
};

class ChatModel {
 public:
  virtual ~ChatModel() = default;

  // 同步：阻塞直到收敛为完整消息。返回 expected（错误是常态）。
  virtual std::expected<Message, ModelError> invoke(std::span<Message const> msgs,
                                                   CallOptions const& opts) = 0;

  // 同步流式：按到达顺序产出增量块。出错抛 ModelException（生成器无法返回 expected）。
  virtual std::generator<MessageChunk> stream(std::span<Message const> msgs,
                                             CallOptions const& opts) = 0;

  // 异步回调（核心）：立即返回取消句柄；delta/done/error 在 IO 线程回调。
  virtual core::CancelToken stream_async(std::span<Message const> msgs, CallOptions const& opts,
                                         StreamHandlers handlers) = 0;

  // 异步可等待：co_await 得 expected<Message, ModelError>。
  virtual Task<std::expected<Message, ModelError>> invoke_async(std::span<Message const> msgs,
                                                                CallOptions opts) = 0;
};

}  // namespace cppchain::model