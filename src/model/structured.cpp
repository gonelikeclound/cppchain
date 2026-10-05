// cppchain —— src/model/structured.cpp
#include "cppchain/model/structured.hpp"

#include <utility>

namespace cppchain::model {

using message::Message;

namespace {

std::string type_name_of(nlohmann::json const& v) { return v.type_name(); }

bool type_matches(std::string const& want, nlohmann::json const& v) {
  if (want == "object") return v.is_object();
  if (want == "array") return v.is_array();
  if (want == "string") return v.is_string();
  if (want == "number") return v.is_number();
  if (want == "integer") return v.is_number_integer() || v.is_number_unsigned();
  if (want == "boolean") return v.is_boolean();
  if (want == "null") return v.is_null();
  return true;  // 未知类型不判（保持宽容）
}

}  // namespace

std::optional<std::string> validate_json(nlohmann::json const& schema,
                                        nlohmann::json const& value) {
  if (!schema.is_object()) return std::nullopt;

  // type（支持字符串与联合数组）
  if (schema.contains("type")) {
    auto const& t = schema["type"];
    if (t.is_string()) {
      if (!type_matches(t.get<std::string>(), value))
        return "expected type '" + t.get<std::string>() + "' but got '" + type_name_of(value) + "'";
    } else if (t.is_array()) {
      bool any = false;
      for (auto const& x : t)
        if (x.is_string() && type_matches(x.get<std::string>(), value)) any = true;
      if (!any) return "value type '" + type_name_of(value) + "' not in allowed union " + t.dump();
    }
  }

  // enum
  if (schema.contains("enum") && schema["enum"].is_array()) {
    bool found = false;
    for (auto const& e : schema["enum"])
      if (e == value) found = true;
    if (!found) return "value " + value.dump() + " not in enum " + schema["enum"].dump();
  }

  // 对象：required + 递归 properties
  if (value.is_object()) {
    if (schema.contains("required") && schema["required"].is_array()) {
      for (auto const& r : schema["required"]) {
        if (r.is_string() && !value.contains(r.get<std::string>()))
          return "missing required property: '" + r.get<std::string>() + "'";
      }
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
      for (auto it = value.begin(); it != value.end(); ++it) {
        if (!schema["properties"].contains(it.key())) continue;
        if (auto v = validate_json(schema["properties"][it.key()], it.value()))
          return "property '" + it.key() + "': " + *v;
      }
    }
  }

  // 数组：递归 items
  if (value.is_array() && schema.contains("items") && schema["items"].is_object()) {
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (auto v = validate_json(schema["items"], value[i]))
        return "item[" + std::to_string(i) + "]: " + *v;
    }
  }

  return std::nullopt;
}

CallOptions StructuredOutput::prepare(CallOptions opts, std::vector<Message>& convo) const {
  if (opts_.mode == StructuredMode::ToolCall) {
    nlohmann::json fn = {{"name", opts_.tool_name},
                         {"description", "Return the answer as structured data."},
                         {"parameters", schema_}};
    opts.tools = nlohmann::json::array({nlohmann::json{{"type", "function"}, {"function", fn}}});
    opts.tool_choice = nlohmann::json{{"type", "function"}, {"function", {{"name", opts_.tool_name}}}};
  } else {
    opts.response_format = nlohmann::json{{"type", "json_object"}};
    convo.push_back(message::system_message(
        "你必须只输出一个 JSON 对象，且满足以下 JSON Schema（不要输出解释文字）：\n" +
        schema_.dump()));
  }
  return opts;
}

std::expected<nlohmann::json, ModelError> StructuredOutput::invoke(
    std::span<Message const> msgs, CallOptions opts) {
  std::vector<Message> convo(msgs.begin(), msgs.end());
  opts = prepare(std::move(opts), convo);

  std::string last_violation = "no attempt made";

  for (int attempt = 0; attempt <= opts_.max_retries; ++attempt) {
    auto reply = model_->invoke(convo, opts);
    if (!reply) return std::unexpected(reply.error());

    // 取候选 JSON（两种策略来源不同）
    std::optional<nlohmann::json> candidate;
    if (opts_.mode == StructuredMode::ToolCall) {
      if (reply->tool_calls.empty()) {
        last_violation = "model did not call the structured output tool";
      } else {
        candidate = reply->tool_calls[0].args;
      }
    } else {
      auto parsed = nlohmann::json::parse(reply->text(), nullptr, false);
      if (parsed.is_discarded())
        last_violation = "output is not valid JSON: " + reply->text().substr(0, 200);
      else
        candidate = std::move(parsed);
    }

    if (candidate) {
      if (auto violation = validate_json(schema_, *candidate); !violation) return *candidate;
      else last_violation = *violation;
    }

    if (attempt == opts_.max_retries) break;

    // 失败回喂：保留模型自己的输出 + 一条带违规说明的纠正指令
    convo.push_back(*reply);
    // 协议不变式（message.hpp validate_sequence）：带 tool_calls 的 AI 消息之后，必须紧跟
    // 覆盖其全部 id 的 ToolMessage，否则真实端点直接 400。ToolCall 策略下失败的那次 reply
    // 恰恰带 tool_calls（args 不合规），必须逐个补齐回执，纠正文案才发得出去。
    for (auto const& call : reply->tool_calls) {
      convo.push_back(message::tool_message("error: " + last_violation, call.id, "error"));
    }
    convo.push_back(message::human_message(
        "你上一次的输出不符合要求：" + last_violation +
        "\n请重新输出：只给出满足 JSON Schema 的 JSON。"));
  }

  return std::unexpected(ModelError{ModelError::Kind::Parse, 0,
                                    "structured output failed: " + last_violation, {}});
}

std::expected<std::string, ModelError> StructuredOutput::classify(
    std::span<Message const> msgs, std::vector<std::string> const& labels, CallOptions opts) {
  nlohmann::json label_enum = nlohmann::json::array();
  for (auto const& l : labels) label_enum.push_back(l);

  StructuredOutput inner(*model_,
                         nlohmann::json{{"type", "object"},
                                        {"properties", {{"label", {{"type", "string"},
                                                                   {"enum", label_enum}}}}},
                                        {"required", nlohmann::json::array({"label"})}},
                         opts_);
  auto r = inner.invoke(msgs, std::move(opts));
  if (!r) return std::unexpected(r.error());
  return r->at("label").get<std::string>();
}

}  // namespace cppchain::model