// cppchain —— model/structured.hpp
// Schema 驱动的结构化输出（M4）：两种策略 + 最小校验 + 失败回喂重试。
// 实测前提见 02-arch-design/05：本端点不支持 response_format=json_schema，
// 但「强制工具调用」5/5 满足 schema → 作为默认策略。
#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/error.hpp"
#include "cppchain/model/options.hpp"

namespace cppchain::model {

// 最小 JSON Schema 校验（子集：type / enum / required / properties / items）。
// 通过 → nullopt；否则返回人类可读的违规说明（可直接回喂给模型）。
std::optional<std::string> validate_json(nlohmann::json const& schema,
                                        nlohmann::json const& value);

enum class StructuredMode {
  ToolCall,    // 默认：schema 包成工具 + tool_choice 强制（本端点实测遵循度最高）
  JsonObject,  // response_format=json_object + prompt 描述 schema（依赖模型自觉）
};

struct StructuredOptions {
  StructuredMode mode = StructuredMode::ToolCall;
  int max_retries = 2;                           // 失败回喂重试次数
  std::string tool_name = "structured_output";   // ToolCall 策略的伪工具名
};

class StructuredOutput {
 public:
  StructuredOutput(ChatModel& model, nlohmann::json schema, StructuredOptions opts = {})
      : model_(&model), schema_(std::move(schema)), opts_(std::move(opts)) {}

  // 返回**已通过 schema 校验**的 JSON；校验不过不会返回（重试耗尽则报 Parse 错误）
  std::expected<nlohmann::json, ModelError> invoke(std::span<message::Message const> msgs,
                                                   CallOptions opts);

  // route/judge 便捷件：schema = {label: enum(labels)} 的特例
  std::expected<std::string, ModelError> classify(std::span<message::Message const> msgs,
                                                  std::vector<std::string> const& labels,
                                                  CallOptions opts);

  [[nodiscard]] nlohmann::json const& schema() const { return schema_; }
  [[nodiscard]] StructuredOptions const& options() const { return opts_; }

 private:
  // 组装本次尝试的调用参数（ToolCall / JsonObject 两种策略不同）
  CallOptions prepare(CallOptions opts, std::vector<message::Message>& convo) const;

  ChatModel* model_;
  nlohmann::json schema_;
  StructuredOptions opts_;
};

}  // namespace cppchain::model