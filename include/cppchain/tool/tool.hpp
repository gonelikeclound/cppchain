// cppchain —— tool/tool.hpp
// 工具规格：显式 JSON Schema + handler（反射实测不可用，见 02-arch-design/03 §3）。
#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "cppchain/core/cancel.hpp"

namespace cppchain::tool {

struct ToolError {
  std::string message;
};

struct ToolException : std::runtime_error {
  explicit ToolException(std::string const& msg) : std::runtime_error(msg) {}
};

// 执行上下文：注入给工具（后续扩展 state / store / 审批）
struct ToolContext {
  std::string tool_call_id;
  core::CancelToken cancel;
  // 预留：StateRef state; Store* store; StreamWriter* writer;
};

struct ToolSpec {
  // handler 契约：可能被并行调用 → 必须线程安全；args 为已解析的 JSON 参数
  using Handler = std::function<std::expected<nlohmann::json, ToolError>(
      nlohmann::json const& args, ToolContext& ctx)>;

  std::string name;
  std::string description;
  nlohmann::json parameters = nlohmann::json::object();  // JSON Schema
  Handler handler;
  bool read_only = true;  // 副作用标记（M7 审批中间件用）

  // → {"type":"function","function":{name,description,parameters}}，直接填入 CallOptions::tools
  [[nodiscard]] nlohmann::json to_openai() const;
};

// ---- 参数取值助手（无反射，显式取值 + 明确报错）----

template <typename T>
std::expected<T, ToolError> arg(nlohmann::json const& args, std::string_view key,
                               bool required = true) {
  auto it = args.find(std::string(key));
  if (it == args.end() || it->is_null()) {
    if (!required) return T{};
    return std::unexpected(ToolError{"missing required argument: '" + std::string(key) + "'"});
  }
  try {
    return it->get<T>();
  } catch (nlohmann::json::exception const& e) {
    return std::unexpected(ToolError{"argument '" + std::string(key) + "' invalid: " + e.what()});
  }
}

// ---- Schema builder（写入参数即成 schema）----

class SchemaBuilder {
 public:
  SchemaBuilder& prop(std::string name, std::string type, std::string description,
                      bool required = true) {
    std::string const key = name;
    properties_[key] = {{"type", std::move(type)}, {"description", std::move(description)}};
    if (required) required_.push_back(key);
    return *this;
  }

  [[nodiscard]] nlohmann::json build() const {
    nlohmann::json out = {{"type", "object"}, {"properties", properties_}};
    if (!required_.empty()) out["required"] = required_;
    return out;
  }

 private:
  nlohmann::json properties_ = nlohmann::json::object();
  nlohmann::json required_ = nlohmann::json::array();
};

// 便捷构造
inline ToolSpec make_tool(std::string name, std::string description, nlohmann::json parameters,
                          ToolSpec::Handler handler, bool read_only = true) {
  ToolSpec spec;
  spec.name = std::move(name);
  spec.description = std::move(description);
  spec.parameters = std::move(parameters);
  spec.handler = std::move(handler);
  spec.read_only = read_only;
  return spec;
}

}  // namespace cppchain::tool