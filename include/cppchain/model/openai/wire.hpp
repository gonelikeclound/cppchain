// cppchain —— model/openai/wire.hpp
// OpenAI 兼容 wire 适配：Message ↔ OpenAI Chat Completions JSON。
// 覆盖 OpenAI 官方规范 + 常见兼容端点（DeepSeek/Ollama/vLLM）。
// 非标准字段一律透传进 Message::response_metadata（比 langchain 更宽松）。
#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <span>
#include <string>

#include "cppchain/message/chunk.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/model/error.hpp"
#include "cppchain/model/options.hpp"

namespace cppchain::model::openai {

using cppchain::message::InvalidToolCall;
using cppchain::message::Message;
using cppchain::message::MessageChunk;
using cppchain::message::ReasoningBlock;
using cppchain::message::Role;
using cppchain::message::TextBlock;
using cppchain::message::ToolCall;
using cppchain::message::ToolCallChunk;
using cppchain::message::UsageMetadata;

inline ModelError parse_error(std::string msg, nlohmann::json const& raw) {
  return ModelError{ModelError::Kind::Parse, 0, std::move(msg), raw};
}

// ---- 出站：Message → OpenAI messages 元素 ----

inline nlohmann::json message_to_openai(Message const& m) {
  nlohmann::json j = nlohmann::json::object();
  switch (m.type) {
    case Role::System: j["role"] = "system"; break;
    case Role::Human: j["role"] = "user"; break;
    case Role::Ai: j["role"] = "assistant"; break;
    case Role::Tool: j["role"] = "tool"; break;
    case Role::Chat: j["role"] = m.custom_role.value_or("user"); break;
  }
  // 出站只发文本块（reasoning/多模态暂不外发）；文本块拼为字符串，兼容性最好。
  j["content"] = m.text();

  if (!m.tool_calls.empty()) {
    auto calls = nlohmann::json::array();
    for (auto const& tc : m.tool_calls) {
      calls.push_back({{"id", tc.id},
                       {"type", "function"},
                       {"function", {{"name", tc.name}, {"arguments", tc.args.dump()}}}});
    }
    j["tool_calls"] = std::move(calls);
  }
  if (m.type == Role::Tool && m.tool_call_id) j["tool_call_id"] = *m.tool_call_id;
  if (m.name) j["name"] = *m.name;
  return j;
}

inline nlohmann::json build_request(std::span<Message const> msgs, CallOptions const& opts,
                                    ModelConfig const& cfg, bool stream) {
  nlohmann::json body = nlohmann::json::object();
  body["model"] = opts.model.value_or(cfg.model);

  auto arr = nlohmann::json::array();
  for (auto const& m : msgs) arr.push_back(message_to_openai(m));
  body["messages"] = std::move(arr);

  if (stream) body["stream"] = true;
  if (opts.temperature) body["temperature"] = *opts.temperature;
  if (opts.max_tokens) body["max_tokens"] = *opts.max_tokens;
  if (!opts.stop.empty()) body["stop"] = opts.stop;
  if (!opts.tools.empty()) body["tools"] = opts.tools;
  if (opts.tool_choice) body["tool_choice"] = *opts.tool_choice;

  for (auto it = opts.extra.begin(); it != opts.extra.end(); ++it) body[it.key()] = it.value();
  return body;
}

// ---- 用量 ----

inline UsageMetadata usage_from_json(nlohmann::json const& u) {
  UsageMetadata out;
  if (u.contains("prompt_tokens")) out.input_tokens = u["prompt_tokens"].get<long>();
  if (u.contains("completion_tokens")) out.output_tokens = u["completion_tokens"].get<long>();
  if (u.contains("total_tokens")) out.total_tokens = u["total_tokens"].get<long>();
  if (u.contains("reasoning_tokens")) out.reasoning_tokens = u["reasoning_tokens"].get<long>();
  if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object() &&
      u["completion_tokens_details"].contains("reasoning_tokens"))
    out.reasoning_tokens = u["completion_tokens_details"]["reasoning_tokens"].get<long>();
  return out;
}

// ---- 入站：非流式响应 → Message ----

inline std::expected<Message, ModelError> parse_response(nlohmann::json const& body) {
  try {
    if (!body.contains("choices") || !body["choices"].is_array() || body["choices"].empty())
      return std::unexpected(parse_error("response has no choices", body));

    auto const& choice = body["choices"][0];
    if (!choice.contains("message")) return std::unexpected(parse_error("choice has no message", body));
    auto const& src = choice["message"];

    Message m;
    m.type = Role::Ai;

    // 推理内容（DeepSeek reasoner 等）→ ReasoningBlock，置于文本块之前
    if (src.contains("reasoning_content") && src["reasoning_content"].is_string())
      m.content.push_back(ReasoningBlock{src["reasoning_content"].get<std::string>(), std::nullopt});

    if (src.contains("content") && !src["content"].is_null()) {
      if (src["content"].is_string()) {
        m.content.push_back(TextBlock{src["content"].get<std::string>(), std::nullopt});
      } else if (src["content"].is_array()) {
        for (auto const& blk : src["content"]) {
          if (blk.is_object() && blk.value("type", "") == "text" && blk.contains("text"))
            m.content.push_back(TextBlock{blk["text"].get<std::string>(), std::nullopt});
        }
      }
    }

    if (src.contains("tool_calls") && src["tool_calls"].is_array()) {
      for (auto const& tc : src["tool_calls"]) {
        std::string id = tc.value("id", "");
        std::string name;
        std::string args_text;
        if (tc.contains("function")) {
          name = tc["function"].value("name", "");
          args_text = tc["function"].value("arguments", "");
        }
        nlohmann::json args = nlohmann::json::object();
        bool ok = true;
        if (!args_text.empty()) {
          try {
            args = nlohmann::json::parse(args_text);
          } catch (nlohmann::json::exception const&) {
            m.invalid_tool_calls.push_back(InvalidToolCall{id, name, "arguments not parseable"});
            ok = false;
          }
        }
        if (ok) m.tool_calls.push_back(ToolCall{id, name, std::move(args)});
      }
    }

    if (body.contains("usage") && body["usage"].is_object())
      m.usage = usage_from_json(body["usage"]);

    // 元数据 + 非标准字段透传
    nlohmann::json meta = nlohmann::json::object();
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null())
      meta["finish_reason"] = choice["finish_reason"];
    if (body.contains("model")) meta["model"] = body["model"];
    if (body.contains("id")) meta["response_id"] = body["id"];
    for (auto it = src.begin(); it != src.end(); ++it) {
      auto const& k = it.key();
      if (k == "content" || k == "role" || k == "tool_calls" || k == "reasoning_content") continue;
      meta["extra_" + k] = it.value();
    }
    m.response_metadata = std::move(meta);
    return m;
  } catch (nlohmann::json::exception const& e) {
    return std::unexpected(parse_error(e.what(), body));
  }
}

// ---- 入站：流式 delta → MessageChunk ----

inline std::expected<MessageChunk, ModelError> parse_chunk(nlohmann::json const& ev,
                                                           std::string* finish_reason_out = nullptr) {
  try {
    MessageChunk c;
    c.type = Role::Ai;

    if (ev.contains("usage") && ev["usage"].is_object()) c.usage = usage_from_json(ev["usage"]);
    if (!ev.contains("choices") || !ev["choices"].is_array() || ev["choices"].empty())
      return c;  // 可能是 usage-only 帧

    auto const& choice = ev["choices"][0];
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null() &&
        finish_reason_out)
      *finish_reason_out = choice["finish_reason"].get<std::string>();

    if (!choice.contains("delta")) return c;
    auto const& d = choice["delta"];

    if (d.contains("reasoning_content") && d["reasoning_content"].is_string())
      c.content.push_back(ReasoningBlock{d["reasoning_content"].get<std::string>(), std::nullopt});

    if (d.contains("content") && d["content"].is_string())
      c.content.push_back(TextBlock{d["content"].get<std::string>(), std::nullopt});

    if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
      for (auto const& tc : d["tool_calls"]) {
        ToolCallChunk t;
        t.index = tc.value("index", 0);
        if (tc.contains("id") && tc["id"].is_string()) {
          t.id = tc["id"].get<std::string>();
          t.has_id = true;
        }
        if (tc.contains("function") && tc["function"].is_object()) {
          auto const& f = tc["function"];
          if (f.contains("name") && f["name"].is_string()) {
            t.name = f["name"].get<std::string>();
            t.has_name = true;
          }
          if (f.contains("arguments") && f["arguments"].is_string()) {
            t.args = f["arguments"].get<std::string>();
            t.has_args = true;
          }
        }
        c.tool_call_chunks.push_back(std::move(t));
      }
    }
    return c;
  } catch (nlohmann::json::exception const& e) {
    return std::unexpected(parse_error(e.what(), ev));
  }
}

}  // namespace cppchain::model::openai