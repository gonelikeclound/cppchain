// cppchain —— tool/tool.hpp
// 工具规格：显式 JSON Schema + handler（反射实测不可用，见 02-arch-design/03 §3）。
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
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

  // 结果时效性：超过该时长即视为"可能已过期"（余额/库存/价格/文件内容这类易变状态）。
  // 留空 = 不过期（静态知识类工具）。由 agent::StaleStateGuard 消费，见 04-problems/06。
  std::optional<std::chrono::seconds> ttl;

  // 工具类别（wfr-㊵）：供"历史精简"按类别过滤（如丢掉联网取数的原始长文）。
  // 词表见下面的 kToolCategories / is_known_category。**不进 to_openai()** ——
  // 类别是库内分类，不是 function schema 的一部分；真端点对未知字段敏感（见 to_openai 的教训）。
  std::string category = "general";

  // 链式设置类别：`reg.add(make_tool(...).with_category("retrieval"))`
  ToolSpec& with_category(std::string c) {
    category = std::move(c);
    return *this;
  }

  // → {"type":"function","function":{name,description,parameters}}，直接填入 CallOptions::tools
  [[nodiscard]] nlohmann::json to_openai() const;
};

// 类别词表（封闭）。语义粗略但够用 —— 目的是"按用途筛选历史"，不是权限模型：
//   retrieval 联网/外部取数（易变、返回往往很长）  compute 纯本地确定性计算
//   action    有副作用（通知/写文件/下单）        model  反过来调模型（llm / 结构化抽取）
//   time      时间基准                             general 未归类（不匹配任何显式类别）
inline constexpr std::string_view kToolCategories[] = {"retrieval", "compute", "action",
                                                       "model", "time", "general"};

[[nodiscard]] inline bool is_known_category(std::string_view c) {
  for (std::string_view const k : kToolCategories)
    if (c == k) return true;
  return false;
}

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